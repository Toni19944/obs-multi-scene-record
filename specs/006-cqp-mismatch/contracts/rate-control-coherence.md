# Contract: Rate-control four-way coherence

**Feature**: 006-cqp-mismatch

This document defines the **four-way coherence contract** (FR-011) that every surface in the plugin must honour, and the libobs API surface the implementation depends on.

## The four surfaces

For any slot `S` at any moment, the rate-control mode `M` and value `V` reported by these four surfaces MUST be identical:

| # | Surface | Where read in source |
|---|---|---|
| 1 | Editor display | `SlotEditor::update_shared_encoder_visibility` (post-006: read-only inherited rows for consumers; existing editable rows for owners) |
| 2 | Persisted config | `SlotManager::slot_to_data` / `slot_from_data` after load-time normalization |
| 3 | Encoder at start | `SharedEncoder::build` → `apply_encoder_settings` → `obs_data_set_*` calls; final state queryable via `obs_encoder_get_settings` |
| 4 | Slot start log | `SceneSlot::start()`'s `blog(LOG_INFO, "[multi-scene-rec] ... started ...")` line |

Divergence on any pair (1↔2, 1↔3, 1↔4, 2↔3, 2↔4, 3↔4) is a defect.

## Resolution rule (single source of truth)

For an **owner** slot:

> The four surfaces all read from the slot's own `Config::rate_control` / `Config::rc_value`. After load, those values have already been clamped / mode-substituted (Decisions 3 and 4); the encoder receives the same values; the editor displays the same values; the log emits the same values.

For a **consumer** slot:

> The four surfaces all read from `SlotManager::effective_rate_control(consumer_cfg)`, which resolves to the **owner's** `Config::rate_control` / `Config::rc_value` (or the fallback values when the owner's encoder context built under fallback).

The contract is enforced at exactly one read function (`effective_rate_control`); a violation is impossible without bypassing it.

## API surface (libobs)

| API | Header | How this feature uses it |
|---|---|---|
| `obs_get_encoder_properties(id) -> obs_properties_t*` | `libobs/obs.h` | New: called once at load (per consumer-slot Config that's standalone, i.e., owner-eligible) to discover the encoder's RC mode list and value range for clamp / substitute. Already called in `set_quality_value` (slot.cpp) and `populate_rate_control_combo` / `update_rc_value_field` (ui-slot-editor.cpp). |
| `obs_properties_get(props, name) -> obs_property_t*` | `libobs/obs.h` | New: walk the `rate_control` property and the quality keys; existing call sites already do this. |
| `obs_property_get_type(p)` | `libobs/obs.h` | Used to confirm a property is `OBS_PROPERTY_LIST` (for `rate_control` modes) or `OBS_PROPERTY_INT` (for value ranges). |
| `obs_property_list_item_count(p)` / `obs_property_list_item_string(p, i)` | `libobs/obs.h` | Walk the mode list (Decision 4) and the substitution target ("first listed"). Already used in `populate_rate_control_combo`. |
| `obs_property_int_min(p)` / `obs_property_int_max(p)` | `libobs/obs.h` | Read the bitrate / quality range for the clamp (Decision 3). Already used by `introspect_int_range` in the editor. |
| `obs_properties_destroy(props)` | `libobs/obs.h` | Release the properties handle after use. |
| `obs_data_set_string(s, "rate_control", mode)` | `libobs/obs-data.h` | Existing — encoder-build path; **unchanged**. |
| `obs_data_set_int(s, "bitrate" \| <quality key>, value)` | `libobs/obs-data.h` | Existing — encoder-build path; **unchanged**. |

**No new libobs APIs introduced.** All calls already exist in the codebase. The contract change is who calls them, when, and against which Config.

## Internal contract: `SlotManager::effective_rate_control`

```cpp
struct EffectiveRC {
    std::string mode;
    uint32_t    value;
    bool        fallback;
    std::string owner_slot_name;
};

EffectiveRC SlotManager::effective_rate_control(const SceneSlot::Config& c) const;
```

**Inputs**: a `SceneSlot::Config` reference (typically `slot.config()` or the editor's `cfg_`).

**Outputs**: see the struct. `mode` is the encoder mode literal (`"CBR"`, `"CQP"`, `"Lossless"`, ...). `value` is bitrate kbps or quality level; undefined when `rc_util::is_lossless(mode)`. `fallback` is true iff the owner's `SharedEncoder` is currently built and `encoder_fallback_ == true`. `owner_slot_name` is non-empty iff the input is a consumer.

**Resolution**:

1. If `c.shared_encoder_slot_id.empty()` — owner: return `{c.rate_control, c.rc_value, false, ""}`.
2. Else look up owner Config by id (existing `config_by_slot_id`, briefly under `mtx_`). If lookup fails (e.g., owner deleted), return `{"CBR", 6000, false, ""}` as a safe last resort (the same defaults `slot_from_data` already uses).
3. Else, briefly under `shared_mtx_`, check whether `shared_.find(c.shared_encoder_slot_id)` has a built context with `encoder_fallback_ == true`. If yes, override `mode = "CBR"`, `value = is_bitrate_based(owner.rate_control) ? owner.rc_value : 6000`, `fallback = true`. (Matches the fallback values at `src/slot.cpp:315-316`.)
4. Return.

**Threading**: takes `mtx_` and `shared_mtx_` independently and briefly. Never holds both at once. Lock order preserved.

**Load-time normalization threading**: `SlotManager::slot_from_data` is called from `load_from` while holding `SlotManager::mtx_`. The three new normalization steps (Decisions 2/3/4 in [research.md](../research.md)) call only pure libobs property accessors (`obs_get_encoder_properties`, `obs_property_list_*`, `obs_property_int_*`, `obs_properties_destroy`) and acquire no plugin locks. The resolution helper `effective_rate_control` is **never** called during load — only at slot start, log emit, editor refresh, and replay-buffer-estimate precompute sites. Therefore the helper's own `mtx_` acquisition cannot collide with `slot_from_data`'s caller already holding `mtx_`.

**Idempotency / purity**: pure function of inputs + current `SlotManager` state. No mutation. Safe to call from any context that doesn't already hold `mtx_` or `shared_mtx_`.

**Forbidden contexts**:

- Inside `SlotManager::config_by_slot_id` (would deadlock: it already holds `mtx_`).
- Inside `SharedEncoder::build` or under `shared_mtx_` (would deadlock if the helper tried to take `shared_mtx_`).
- From `SceneSlot::teardown_locked` while holding `slot_mtx_` (would not deadlock — `mtx_` is above `slot_mtx_` in the global order — but the existing pattern is "release slot_mtx_ first, then call manager"; we preserve that by **only** calling the helper from `start()` post-acquire-pre-slot_mtx_ and from UI-thread read paths).

## Internal contract: load-time normalization steps

In `slot_from_data` (`manager.cpp`), the three new normalization steps run after existing back-compat defaults. They are **idempotent** — running them twice on the same `c` is a no-op.

| Step | When applied | Effect |
|---|---|---|
| Decision 2 (clear sentinel) | `c.shared_encoder_slot_id` non-empty | `c.rate_control = "<inherited>"`, `c.rc_value = 0`. Skip remaining steps. |
| Decision 4 (mode substitute) | `c.shared_encoder_slot_id` empty; `c.rate_control` not in encoder's reported `rate_control` list | Substitute `c.rate_control` with the first listed mode. Emit one warning. |
| Decision 3 (value clamp) | `c.shared_encoder_slot_id` empty; `c.rc_value` outside the introspected range for `c.rate_control` | Clamp `c.rc_value` to `[min, max]`. Emit one warning. |

Both warning lines are at `LOG_WARNING` level, prefixed with `[multi-scene-rec]`, and include the slot name, original value, encoder id, range / list, and substituted/clamped value (full text in research.md). One log line per migration per slot per load — no spam.

## Internal contract: editor inherited rows (FR-005)

**Canonical label strings (single source of truth — referenced by spec.md, data-model.md, quickstart.md, and tasks.md):**

- Rate-control row label: `Rate control (inherited from <owner-name>)`
- Value row label: `Value (inherited from <owner-name>)`
- When the helper returns `fallback == true`, append `" [CBR fallback]"` to both labels.
- When the consumer is an orphan (owner deleted; `owner_slot_name.empty()`), substitute `Rate control (inherited — owner missing)` and `Value (inherited — owner missing)`.

When `SlotEditor::update_shared_encoder_visibility` is called with the venc combo's data starting with `"shared:"`:

1. Rate-control combo:
   - Has exactly one item, populated from `effective_rate_control(cfg_).mode`.
   - Item text is that mode literally (e.g., `CQP`, `Lossless`, `CBR`).
   - Combo is disabled.
   - Label matches the canonical string above.

2. Value spinbox:
   - Range is `[V, V]` where `V` is `effective_rate_control(cfg_).value` (or `[0, 0]` with special text `"— (lossless)"` when mode is Lossless).
   - Spinbox is disabled.
   - Label matches the canonical string above.

3. Behaviour when the editor's `on_accept` runs while in the shared branch:
   - Both fields are written to the sentinel + 0 per Decision 2 (research.md), regardless of what the disabled widgets currently display. The widgets are display-only; the resolver provides truth.

## Internal contract: log line format (FR-001 / FR-008)

```
[multi-scene-rec] '<slot name>' started (<W>x<H>@<fps_num>, <rc_segment>, tracks=<tracks>, <mode>)[<inherited_suffix>]
```

`<rc_segment>` is:

- `Lossless` when `is_lossless(effective.mode)`.
- `<effective.mode>/<effective.value>` otherwise.
- Prefixed with `[CBR fallback] ` when `effective.fallback == true`.

`<inherited_suffix>` is:

- ` inherited from '<owner name>'` when the slot is a consumer.
- empty otherwise.

`<mode>` is unchanged: one of `replay-only` / `rec+replay` / `rec-only`.

## Constitution mapping

| Principle | How this contract honours it |
|---|---|
| I. Native OBS API Compliance | Only existing libobs / obs-frontend APIs are used. No new headers, no internal symbols. |
| II. Clear Ownership & Minimal Shared State | The resolution helper reads owner Config via the existing `config_by_slot_id` path; no slot reaches into another slot's pipeline directly. `SharedEncoder` ownership in `SlotManager` is untouched. |
| III. Thread Safety (NON-NEGOTIABLE) | The helper takes `mtx_` and `shared_mtx_` independently and briefly. No new locks. No new threads. Global lock order preserved. |
| IV. UI / Logic Separation | The editor reads from `SlotManager::effective_rate_control`; it does not call libobs directly to read encoder state. (The editor still calls `obs_get_encoder_properties` for the standalone-owner range / mode list — that's the existing introspection pattern from feature 003/005, unchanged.) |
| V. Encoder Robustness & Graceful Fallback | The fallback flag (`SharedEncoder::encoder_fallback_`) is surfaced via the helper's `fallback` field and rendered in both editor and log as `[CBR fallback]` — matching the existing UI convention. |
| VIII. Shared Encoder — Literal Semantics | Consumer slots no longer carry stale, contradictory rate-control fields. The owner's settings are what the encoder is built with and what every surface reports — literal semantics by construction. |
| IX. Configurable Settings Parity | No user-configurable setting is removed. Rate-control mode and value are still editable on owner slots; the editor now also displays (read-only) the inherited values on consumer slots, where previously the rows were hidden. Users are gaining information, not losing controls. |
