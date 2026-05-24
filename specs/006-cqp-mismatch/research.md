# Research: CQP value coherence across editor, log, and shared-encoder consumer

**Feature**: 006-cqp-mismatch
**Status**: Phase 0 — all NEEDS CLARIFICATION items resolved here.
**Date**: 2026-05-24

This document resolves the design decisions referenced in [spec.md](./spec.md) and grounds the implementation plan in concrete `src/` call sites.

## Decision 1 — single owner-resolution helper, used everywhere

**Decision**: introduce one helper on `SlotManager` that returns the **effective** rate-control mode/value for any slot, regardless of whether it's an owner or a consumer, and whether or not the owner's encoder context is currently built.

```cpp
struct EffectiveRC {
    std::string mode;   // e.g. "CBR", "CQP", "Lossless"
    uint32_t    value;  // bitrate kbps or quality level; undefined when mode=="Lossless"
    bool        fallback;   // owner's encoder construction fell back (CBR/safe-bitrate)
    std::string owner_slot_name; // empty when this slot is its own owner
};

EffectiveRC SlotManager::effective_rate_control(const SceneSlot::Config& c) const;
```

Resolution rule:

1. If `c.shared_encoder_slot_id` is empty → owner. Return `(c.rate_control, c.rc_value, /*fallback=*/false, /*owner_name=*/"")`.
2. Otherwise, look up the owner Config by id (existing `config_by_slot_id` path, briefly under `mtx_`). Return the owner's mode/value plus the owner's display name.
3. If the group has a currently-built `SharedEncoder` (consult `shared_` under `shared_mtx_`) and `encoder_fallback_` is true, **override** with the fallback values that were used at build time (mode `"CBR"`, value `6000` — see `src/slot.cpp:315-316`) and set `fallback=true`.

**Rationale**: every requirement in the spec (FR-002, FR-004, FR-005, FR-014, US1 acceptance #3, edge case "owner not running, consumer is") reduces to "ask one helper for the effective values." Centralising the rule keeps the four-way coherence contract (FR-011) enforceable by inspection: every read site goes through this one function.

**Alternatives considered**:

- Snapshot the owner's mode/value into the consumer's `Config` at switch time. Rejected — this is exactly the snapshot-drift hazard the spec calls out (Story 2 Acceptance #1).
- Read the live `obs_encoder_t*`'s settings via `obs_encoder_get_settings`. Rejected — only works when the encoder is built (owner stopped → empty), and OBS docs don't guarantee these values match the build-time settings (encoder may mutate them).

## Decision 2 — load-time normalization of consumer-side stale fields

**Decision**: in `slot_from_data` (`src/manager.cpp:337-447`), after the standard back-compat defaults run, **clear** `c.rate_control` to the sentinel `"<inherited>"` and `c.rc_value` to `0` whenever `c.shared_encoder_slot_id` is non-empty. The sentinel is opaque to OBS (we never write it as a real `rate_control` key) and is the marker every read site can recognise as "go ask the owner."

```cpp
// after existing back-compat defaults, in slot_from_data
if (!c.shared_encoder_slot_id.empty()) {
    c.rate_control = "<inherited>";
    c.rc_value     = 0;
}
```

Symmetric write side: `on_accept` in `src/ui-slot-editor.cpp:756-758` runs the same clear when the user has selected a `shared:` encoder before exiting the dialog. The result on disk is the sentinel; the next save round-trips the normalized form.

**Rationale**:

- Matches the spec clarification "Normalize on load (clear or replace with an explicit 'inherited' sentinel before any other code reads them; next save persists the normalized form)."
- The fix happens at the load-time boundary, so every in-memory read after that point (log line at `slot.cpp:563`, replay-buffer memory estimate at `slot.cpp:748`, editor display, future diagnostics) cannot see stale standalone values — they see the sentinel and route through the resolution helper (Decision 1).
- A literal sentinel string (vs. an empty string) is cheap to grep for, distinguishes intentional clearing from accidentally-empty `obs_data`, and is forward-compatible: if a future feature needs to carry a "last-standalone" memory it can do so under a different key without disturbing this contract.

**Alternatives considered**:

- Use empty string `c.rate_control = ""` and `c.rc_value = 0`. Rejected — `slot_from_data` already coerces empty strings to `"CBR"` for back-compat; we'd have to special-case it before that line, and a future reader would confuse empty-by-omission with empty-by-normalization.
- Refuse to load a consumer slot whose persisted fields don't already match. Rejected — breaks every existing save on disk; the spec explicitly requires migration, not user action.

## Decision 3 — clamp-on-load + warn for out-of-range `rc_value` (FR-013)

**Decision**: in `slot_from_data`, after the standalone-vs-consumer normalization (Decision 2), and only when the slot is **standalone** (`c.shared_encoder_slot_id.empty()`):

1. Introspect the slot's selected encoder for the range corresponding to the current `c.rate_control` mode (bitrate-based → `bitrate` key range; quality-based → first quality-key range from the same list `set_quality_value` walks at `src/slot.cpp:98-115`).
2. If `c.rc_value` falls outside `[min, max]`, clamp to the nearest endpoint and emit one warning log line:

```
[multi-scene-rec] '<slot name>': rc value %u out of range for %s on %s [%d, %d]; clamped to %u
```

3. The clamped value is what the editor displays, what the encoder receives, and what the slot start log reports — preserving FR-011 (four-way coherence).
4. No write-back to disk happens at load time; the clamped value is held in memory and persisted on the next normal save (existing `slot_to_data` path).

**Rationale**:

- The spec's clarification is explicit: "Clamp on load, write back, warn." The clamp happens in memory; the "write back" is implicit via the next save — no special disk-write path is introduced (would risk write loops if introspection itself races load).
- The introspection helper used by the editor (`introspect_quality_range` at `src/ui-slot-editor.cpp:52-62`, `introspect_int_range` at `:36-48`) is the right source of truth. It is currently `static` in the editor TU; we extract it to a small helper visible to `manager.cpp` (the persistence module) so load-time clamping uses identical logic.

**Alternatives considered**:

- Defer clamping until first encoder build (`SharedEncoder::build`). Rejected — would leave the editor showing the un-clamped value until the first start, breaking FR-011 between editor and encoder.
- Clamp silently. Rejected — the warning line is required by the clarification and is the user's only signal that "the saved value you typed isn't valid for this encoder anymore."

## Decision 4 — substitute-on-load + warn for invalid `rate_control` mode string (FR-015)

**Decision**: same load-time path as Decision 3, before the range clamp:

1. Walk the encoder's `rate_control` property list (same path the editor uses at `src/ui-slot-editor.cpp:539-549`).
2. If `c.rate_control` is not in the list, substitute the first listed mode and emit one warning:

```
[multi-scene-rec] '<slot name>': rate-control '%s' not supported by %s; substituted '%s'
```

3. After substitution, run Decision 3's range check against the substituted mode (re-validate the value, which may now itself be out of range and trigger an additional warning).
4. The substituted mode is what the editor displays, what the encoder receives, and what the slot start log reports.

**Rationale**:

- Symmetric with Decision 3 and required verbatim by the spec clarification ("Same load-time normalize-and-warn pattern as FR-013, with the substitution rule being 'first listed mode of the current encoder.'").
- Closes the third silent-clobber path that the spec identifies: the editor would otherwise pick index 0 of the combo and `on_accept` would persist that, masking the original mismatch.

**Alternatives considered**:

- Force-substitute `"CBR"` (the safest universal mode). Rejected — not every encoder lists CBR (e.g. AV1 hardware encoders may not), and the spec is explicit about "first listed mode."
- Refuse to load the slot. Rejected — same reason as Decision 2.

## Decision 5 — uniform consumer-side read redirect (FR-014)

**Decision**: replace every read of `cfg_.rate_control` / `cfg_.rc_value` on a slot that is a **potential consumer** (i.e., wherever `cfg_.shared_encoder_slot_id` could be non-empty at the call site) with a call to the resolution helper from Decision 1.

Identified read sites today (grep of `src/*.{cpp,hpp}` for `rate_control` / `rc_value`):

| Site | File:line | Current code | Action |
|---|---|---|---|
| Slot start log | `slot.cpp:562-566` | reads `cfg_.rate_control.c_str()` and `cfg_.rc_value` directly | route through helper; print `Lossless` literally when mode is lossless; append `(inherited from <owner>)` when consumer; append `[CBR fallback]` when fallback flag set |
| Replay-buffer memory cap | `slot.cpp:748` | `is_bitrate_based(cfg_.rate_control) ? cfg_.rc_value : 12000` | route through helper |
| Owner-encoder build | `slot.cpp:233-241` (`apply_encoder_settings`) | `cfg.rate_control` / `cfg.rc_value` | **unchanged** — this is always called with the owner's Config (`SharedEncoder::build(owner_cfg)`); the helper is for consumer-side reads, not for the build itself |
| Owner-encoder fallback path | `slot.cpp:315-316` | reads `cfg.rate_control`/`cfg.rc_value` of the owner | unchanged for the same reason |
| Editor display | `ui-slot-editor.cpp:184-187, 601-605, 613-619` | reads `cfg_.rc_value` for spinbox initial value | unchanged for own-encoder slots; for consumer slots (the `is_shared` branch at `:1003-1004` already hides the row), the new code adds read-only inherited labels populated from the helper (see Decision 6) |
| Editor save | `ui-slot-editor.cpp:756-758` (`on_accept`) | writes `cfg_.rate_control`/`cfg_.rc_value` from the UI | unchanged for own-encoder slots; for consumer slots, write the `"<inherited>"` sentinel + `0` per Decision 2 |
| Persistence | `manager.cpp:295-296` (`slot_to_data`), `:352-353` (`slot_from_data`) | serializes the fields | unchanged surface; Decision 2/3/4 augment `slot_from_data` only |
| Dock UI | `ui-dock.cpp` | does **not** read these fields today; reads `encoder_fallback` from Stats only | unchanged |

**Rationale**: the spec's FR-014 generalises FR-002 from the slot start log to **every** consumer-side read. The audit table above is the closed list of read sites we have to update; nothing else needs to change. The owner-encoder build paths (`apply_encoder_settings`, `SharedEncoder::build`) are explicitly excluded — they receive the owner's Config already, so they always see authoritative values.

**Alternatives considered**:

- Wipe the consumer's fields on every read (i.e., reach into the SlotManager from `slot.cpp` mid-call). Rejected — would force a write under `slot_mtx_` from a read path and violate the lock order if it ever tried to take `mtx_`.
- Make `SceneSlot::Config::rate_control` / `rc_value` getters that consult the manager. Rejected — POD struct semantics are valuable for `slot_to_data` / `slot_from_data` and for the editor's local-copy pattern (`cfg_(std::move(cfg))` at `ui-slot-editor.cpp:71`); turning them into method calls would ripple across all 27 read sites and break the "Config is plain data" invariant.

## Decision 6 — editor read-only inherited rows (FR-005)

**Decision**: when the editor enters the "shared encoder" branch (`is_shared == true` at `ui-slot-editor.cpp:990`), **stop hiding** the rate-control mode/value rows. Instead:

1. Disable them (`setEnabled(false)`).
2. Set their text to the owner's effective values from the resolution helper (Decision 1).
3. Replace the labels with `"Rate control (inherited from <owner name>)"` and `"Value (inherited)"`.
4. When the owner has fallen back (`fallback == true` from the helper), append `" [CBR fallback]"` to the row, matching the dock's existing convention at `ui-dock.cpp:270`.
5. When the owner's effective mode is Lossless, the value row shows `— (lossless)` (matching the existing pattern at `ui-slot-editor.cpp:583`) and remains disabled.

**Rationale**:

- The current code hides the rows entirely (`set_form_row_visible(form_, rc_combo_, !is_shared)` at `:1003-1004`); the spec's Story 2 Acceptance #2 requires the rows to be visible, read-only, and labeled with the owner's name so the user knows where settings come from. The spec's clarification ("Yes — show them read-only, labeled 'inherited from <owner name>'; fall back values must be shown when the owner's encoder construction fell back") makes this unambiguous.
- The fallback indicator is the same string the dock surfaces; users already know what `[CBR fallback]` means (constitution Principle V).

**Alternatives considered**:

- Insert separate plain-text labels instead of disabled controls. Rejected — the existing `QSpinBox` / `QComboBox` widgets already render the value in a familiar form; disabling them is one line per widget and keeps visual symmetry with the standalone case.
- Show the inherited values as tooltip-only. Rejected — clarification "(b) which owner slot they are inherited from by name, and (c) the actual rate-control mode and value currently in effect" requires the values be visible without hover.

## Decision 7 — FR-007: owner-side editor range and encoder-build key alignment

**Decision**: factor the encoder's quality key list out of `set_quality_value` (`src/slot.cpp:91-117`) and `introspect_quality_range` (`src/ui-slot-editor.cpp:52-62`) into a single named list, shared by both. The same list governs (a) which property the editor introspects for a range and (b) which property the build path writes the value into.

```cpp
// new public helper, e.g. in slot.hpp under namespace rc_util:
const char* const* quality_keys(); // null-terminated list of single keys
const char* const* quality_split_keys(); // QSV qpi/qpp/qpb
```

The two existing functions then walk the same arrays. The contract:

- The first key in `quality_keys()` that the encoder exposes (via `obs_properties_get`) is **both** the range source (editor) and the write target (build).
- If none of `quality_keys()` match but any of `quality_split_keys()` match, set all matching split keys to the single user value; range comes from the first split key the encoder exposes (existing `introspect_quality_range` already lists `qpi` in its tail — but the order is sensitive; we make the parallel explicit).

**Rationale**:

- Today's code is "implicitly aligned" because `set_quality_value` walks `crf / cqp / cq_level / qp / icq_quality / global_quality` (plus QSV split `qpi/qpp/qpb`), and `introspect_quality_range` walks `crf / cqp / cq_level / qp / icq_quality / global_quality / qpi`. The lists overlap by happenstance; one PR that adds a key to one list without the other re-introduces the bug.
- The spec's Story 3 is P2 precisely because today's code happens to work but the alignment is undefended. The fix is to defend it by construction (one list, two callers).

**Alternatives considered**:

- Test the alignment in a unit test (loop over supported encoders, assert range key == write key). Rejected on this project — there is no test harness; manual quickstart steps cover it (see [quickstart.md](./quickstart.md)).
- Always write all known quality keys (the `set_quality_value` "last-resort fallback" already writes `cqp` if nothing matches). Rejected — writing into a key the encoder doesn't expose is harmless on OBS but is exactly the silent-coercion class the spec is trying to eliminate; clarity beats robustness here.

## Decision 8 — Lossless rendering in the log line (FR-008)

**Decision**: in the slot start log (`slot.cpp:562-566` reformat), branch on `rc_util::is_lossless(mode)`:

- Lossless → print `Lossless` with no numeric value, e.g. `(... 1920x1080@60, Lossless, tracks=1, rec-only)`.
- Otherwise → print `<mode>/<value>` as today.

When the helper from Decision 1 returns `fallback=true`, prefix the rate-control segment with `[CBR fallback] ` (matching the existing dock convention).

**Rationale**: spec's Edge Case "Lossless mode on the owner" and FR-008 are explicit. The current code unconditionally prints `cfg_.rc_value` even for lossless modes, which would print `0` (since `rc_value` is unused for lossless) — a number that has no meaning.

## Decision 9 — generation counter as concurrency safety net

**Decision**: the resolution helper (Decision 1) only reads owner Config via `config_by_slot_id` (takes `mtx_` briefly) and the shared-encoder fallback flag via `shared_mtx_` (leaf). No new locks. No new threads. No lock-order changes (mtx_ → slot_mtx_ → shared_mtx_ remains the only acquisition order; the helper takes mtx_ and shared_mtx_ separately, never nested).

Single edge: when called from the editor's "shared encoder visibility" path (UI thread), `mtx_` is taken briefly. The dock's `refresh()` also takes `mtx_` briefly; both are short, bounded reads. Constitution Principle III is preserved.

**Rationale**: documents that the new helper does not change the locking story — important because Principle III is NON-NEGOTIABLE.

## Per-encoder quality-key map (FR-007 evidence)

Confirmed by inspection of OBS's encoder-property definitions in `libobs-modules/`. For each supported encoder ID, the **first key** in `quality_keys()` that `obs_get_encoder_properties` exposes:

| Encoder ID (substring match) | Quality key written | Quality range key (editor) | Notes |
|---|---|---|---|
| `obs_x264` | `crf` | `crf` | RC modes: ABR / CBR / CRF / VBR. CRF range 0–51. |
| `jim_nvenc` / `obs_nvenc_*` | `cqp` | `cqp` | RC modes: CBR / CQP / VBR / VBR_HQ / Lossless. CQP range 0–51. |
| `ffmpeg_nvenc_*` | `cqp` | `cqp` | Same family — keys differ from the OBS-native NVENC plugin only in display labels, not in property keys. |
| `h264_texture_amf` / `h265_texture_amf` / `av1_texture_amf` | `cqp` | `cqp` | RC modes: CBR / CQP / VBR / VBR_LAT. |
| `obs_qsv11_*` (H264 / HEVC / AV1) | `qpi`,`qpp`,`qpb` (split) | first present split key (`qpi`) | RC modes: CBR / CQP / ICQ / VBR. ICQ uses `icq_quality`; CQP uses the split keys. **The editor's range introspection must check `qpi` (already does in the tail of `introspect_quality_range`)**; the build path's `set_quality_value` sets all three split keys as a unit. |
| `vt_h264_*` / `vt_hevc_*` (VideoToolbox) | `quality` (mapped) — uses `global_quality` in OBS's wrapper | `global_quality` | RC modes vary by Apple version; quality range typically 0–100. |
| `aom_av1`, `svt_av1` | `crf` | `crf` | CRF range 0–63. |

Editor-introspected RC modes always reflect what the encoder reports for the current build — this is libobs' standard pattern; the plugin doesn't hardcode any encoder-specific behaviour.

## Summary

Every NEEDS CLARIFICATION resolved. Net plan:

- One new helper on `SlotManager` (`effective_rate_control`) used at three reader sites (`slot.cpp` log line, `slot.cpp` replay-buffer estimate, `ui-slot-editor.cpp` inherited-row population).
- `slot_from_data` (`manager.cpp`) gains three migration steps: stale-fields clear (Decision 2), mode-substitute-and-warn (Decision 4), value-clamp-and-warn (Decision 3).
- `on_accept` (`ui-slot-editor.cpp`) writes the sentinel for consumer slots.
- `update_shared_encoder_visibility` (`ui-slot-editor.cpp`) shows read-only inherited rows instead of hiding them (Decision 6).
- One shared quality-key list extracted (Decision 7); two existing call sites refactored to walk it.
- Slot start log reformat for Lossless / fallback / inherited (Decisions 5, 8).

No new persisted fields. The on-disk format is unchanged in shape: `rate_control` (string) and `rc_value` (int) are still the keys; for a consumer slot, their values are simply normalized to the sentinel + zero before any other code can read them.
