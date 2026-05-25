# Implementation Plan: Replay file uniqueness across slots sharing an output directory + truthful replay-save logging

**Branch**: `007-fix-replay-collision` | **Date**: 2026-05-25 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/007-fix-replay-collision/spec.md`

## Summary

The bug ([spec.md](./spec.md) US1) is that the replay-buffer output's filename pattern at `src/slot.cpp:801` is the literal `"Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` — no slot identifier, only a second-resolution timestamp. When two or more slots are configured with the same output directory and the same container extension, any saves landing in the same wall-clock second produce identical filenames; the second writer overwrites the first and one clip is silently and permanently lost. The continuous recording filename construction at `src/slot.cpp:96-104` already embeds `cfg.name` and is unaffected — only the replay path is broken.

US2 is the natural follow-on: a user inspecting the output directory cannot tell which slot produced which file because no file name carries any slot identity. US3 (from [clarifications](./spec.md#clarifications)) is the observability hole: `src/slot.cpp:1066` emits `'<slot>' replay save OK` whenever `proc_handler_call(ph, "save", &cd)` returns true, which only confirms the proc was dispatched — not that any file was written. When a write fails (cross-slot collision, Windows share-violation race, disk full, output dir missing, OBS-internal muxer error), the plugin still logs OK and the user has no signal of loss.

The fix is built around **three small, structurally-coupled changes** in `src/slot.cpp` (with one helper namespace in `src/slot.hpp`):

- **Filename format helper** (`namespace replay_util` in `slot.hpp`, impl in `slot.cpp`): one function `build_replay_format(const SceneSlot::Config &) -> std::string` that returns the format string to pass to the replay-buffer output's `"format"` setting. Output shape: `"<sanitized-name>_<id6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"`, where `<sanitized-name>` is `cfg.name` with non-portable characters (Windows-illegal: `<>:"/\|?*`, control chars `\x00-\x1F\x7F`, percent `%`, leading/trailing dots and spaces) replaced by `_` and runs of `_` collapsed, and `<id6>` is the **last** 6 hex chars of `cfg.id` (covers the counter and low-bits of ns from `generate_slot_id()` at `slot.cpp:61-69`, so two slots created within the same OS-tick still differ). When `cfg.name` sanitizes to empty (or is empty), the name component is replaced with the literal `"slot"` to match the existing fallback at `slot.cpp:99` and the id6 alone carries identity. Implements FR-001 / FR-002 / FR-003 / FR-004 / FR-004a / FR-005 / FR-010.

- **Format string set-site redirect** (`slot.cpp:801`): replace the literal with a call to the helper. One-line behavioural change. The `"directory"` and `"extension"` keys at the surrounding `slot.cpp:800` / `slot.cpp:802` are unchanged. No new `obs_data_t` keys.

- **Truthful save-result log via `saved` signal subscription** (`slot.cpp` `setup_outputs` / `teardown_locked` / `save_replay` + two new static callbacks): replace the misleading `'<slot>' replay save OK` line at `slot.cpp:1066` with `'<slot>' replay save requested` (LOG_INFO when the proc dispatched; LOG_WARNING with `proc-dispatch FAILED` when it did not). Subscribe to the OBS replay-buffer output's `"saved"` signal (the `signal_handler_add(sh, "void saved()")` at `.deps/obs-studio-31.1.1/plugins/obs-ffmpeg/obs-ffmpeg-mux.c:964`, emitted at `mux.c:1133` only on a successful write per the `mux.c:1130 if (!error)` gate). When `saved` fires, call the output's `"get_last_replay"` proc handler (`mux.c:961`, `mux.c:943-948`) to retrieve the on-disk path and emit `'<slot>' replay save wrote '<path>'`. The `request` line + `wrote` line pair is the user's truthful audit trail: a `request` with no `wrote` follow-up is the explicit signal of failure (whether from a cross-slot collision at the OBS layer, a Windows share-violation race, disk full, output dir missing, or any other OBS-internal muxer error). Implements FR-011 / FR-012 / FR-013.

The two log lines are emitted from **different threads**: `request` from the hotkey/UI thread that initiated `save_replay()`, `wrote` from the OBS muxer thread that ran `replay_buffer_mux_thread` (`mux.c:1089`). The `wrote` callback takes **no plugin locks** — it reads `cfg_.name` and `replay_out_` without `slot_mtx_`, matching the existing pattern in `on_replay_output_stop` (`slot.cpp:1028-1042`). `signal_handler_disconnect("saved", ...)` in `teardown_locked` runs synchronously with any in-flight dispatch (libobs's `signal.c` serializes dispatch and disconnect on the signal's mutex), so by the time the disconnect returns the callback is guaranteed not running and `obs_output_release(replay_out_)` at `slot.cpp:683` can safely proceed without racing the callback.

The fix is **non-contract-affecting** with respect to the on-disk save format: no new `obs_data_t` keys; no change to the persisted slot configuration (the `path`, `container`, `replay_enabled`, `replay_seconds` keys are untouched in shape and meaning); no migration of pre-fix on-disk replay files. Pre-fix files keep their `Replay_<timestamp>` names and co-exist in the same directory with post-fix `<name>_<id6>_Replay_<timestamp>` files; both are valid filenames and neither side scans or cares about the other (per FR-007).

Net code change (estimated): ~120 LOC added across 2 files (`slot.hpp`, `slot.cpp`); ~5 LOC removed; one new namespace (`replay_util`) and two new methods on `SceneSlot` (`on_replay_saved` static signal callback, `log_replay_saved` instance method). No new translation units, no CMake changes, no save-format additions.

The full design and call-site audit is in [research.md](./research.md); the data-model / state-transition view is in [data-model.md](./data-model.md); the filename-construction and log-truthfulness contracts are in [contracts/replay-save-correctness.md](./contracts/replay-save-correctness.md); the manual verification procedure is in [quickstart.md](./quickstart.md).

## Technical Context

**Language/Version**: C++17 (per constitution Build & Platform Requirements). No C++20 features introduced.

**Primary Dependencies**: libobs (`obs_output_create`, `obs_output_get_proc_handler`, `obs_output_get_signal_handler`, `obs_data_set_string`, `proc_handler_call`, `calldata_init`, `calldata_string`, `calldata_free`, `signal_handler_connect`, `signal_handler_disconnect`). No new libobs APIs; all are already used in this codebase. Specifically the `"saved"` signal and the `"get_last_replay"` proc are exposed by the OBS replay-buffer output type (`obs-ffmpeg-mux.c:961-964`) and have been part of OBS Studio since the replay-buffer feature shipped (OBS Studio 19.0 in 2017) — comfortably below the 31.1.1+ floor in `buildspec.json`.

**Storage**: scene-collection JSON via libobs `obs_data_t`. The per-slot `path` (output directory), `container`, and replay-buffer settings keys (`replay_enabled`, `replay_seconds`, etc.) are unchanged in shape and meaning. No new persisted keys are introduced. The runtime-constructed `"format"` setting passed to the replay-buffer output (`obs_data_set_string(rb, "format", ...)` at `slot.cpp:801`) changes shape — this is a transient setting, not a persisted slot field.

**Testing**: manual verification per [quickstart.md](./quickstart.md). 10 tests covering US1 headline bug (T1), US1 N-slot generalization (T2), US1 different-container same-dir (T3), US2 attribution by name (T4), US2 same-name-same-dir collision (T5), US2 empty-name fallback (T6), US3 truthful log on success (T7), US3 truthful log on failure (T8), edge case Windows-reserved character name (T9), regression case for the pre-fix mixed-directory state (T10). Manual coverage is **Windows-only** (the maintainer's test environment, per the 006 plan's same scoping); cross-platform correctness rests on the platform-agnostic nature of the filename construction (a `std::string` build of a `"format"` setting passed to libobs; no platform-specific branches) and on the OBS `"saved"` signal / `"get_last_replay"` proc being part of libobs's portable API surface.

**Target Platform**: Windows x64 (primary and the **only manually-verified** target — the bug repro and the FS-race subset of US3 are Windows-flavored, and the maintainer's test environment is Windows-only). macOS (Xcode 16.0) and Ubuntu 24.04 receive the same code path; cross-platform coverage rests on the structural guarantees and on future community reports — same scoping principle as feature 006.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: the filename helper is one short string-build per slot start (called once when `setup_outputs` configures the replay-buffer output's `"format"` setting). The `saved` signal callback runs once per Save Replay event (a user-action-frequency code path — hotkey press / programmatic save, never a hot path). The `get_last_replay` proc call is a single `obs_data` accessor returning a cached string from inside the OBS muxer (`mux.c:947`). No hot-path code is touched.

**Constraints**:

- Constitution Principle III (lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf) — preserved. The `saved` signal callback takes **NO plugin locks**. It reads `cfg_.name` and `replay_out_` lock-free, matching the existing pattern in `on_replay_output_stop` (`slot.cpp:1028-1042`). The `request`-line log at the proc-call site is emitted while `slot_mtx_` is held (entered at `slot.cpp:1056`) — same lock state as the existing `OK / FAILED` log; the change is wording-only at that site. No new locks; no new threads.
- Constitution Principle IV (UI / Logic Separation) — preserved. No Qt widget changes. The two log changes and the format-string change live entirely in `slot.cpp` (SlotManager-internal). The editor (`ui-slot-editor.cpp`) and dock (`ui-dock.cpp`) are unchanged.
- Constitution Principle V (Encoder Robustness & Graceful Fallback) — preserved. Encoder construction is untouched.
- Constitution Principle VI (Pipeline Isolation From OBS Main) — preserved. No interaction with OBS main outputs.
- Constitution Principle VII (Recording & Replay Buffer Correctness, NON-NEGOTIABLE) — preserved and **strengthened**: replay saves can no longer silently lose files to cross-slot filename collision, and the plugin log no longer falsely claims success for failed saves. The replay-buffer length, mode, and per-slot independence are unchanged. The change is to (a) the OBS replay-buffer output's `"format"` setting (a string we pass to libobs), and (b) the plugin's own log line for the save outcome — nothing about the encoded data, the buffered content, or the save trigger semantics changes.
- Constitution Principle VIII (Shared Encoder — Literal Semantics) — preserved. No encoder changes.
- Constitution Principle IX (Configurable Settings Parity) — preserved. No setting is removed. The user-configurable per-slot output directory is unchanged; users who deliberately configure two slots to the same directory continue to do so, but without silent data loss (which was the headline bug).
- No new fields on `SceneSlot::Config`. The filename derivation reads existing `cfg_.id`, `cfg_.name`, `cfg_.container` only.
- No new persisted file-format keys. The runtime-built `"format"` setting changes shape, but that's a transient libobs setting, not part of the plugin's persisted save.
- Patch notes: `CHANGELOG.md` will gain entries under the existing `[Unreleased]` section — under **Fixed** for US1 (silent replay loss on shared directory) and **Changed** for US3 (replay-save log line semantics).

**Scale/Scope**: 2 source files modified — `slot.hpp` (add `namespace replay_util` with declarations; add `on_replay_saved` / `log_replay_saved` member declarations on `SceneSlot`), `slot.cpp` (implement `replay_util::sanitize_for_filename` + `replay_util::build_replay_format`; redirect `setup_outputs` format-string set-site; redirect `save_replay`'s log wording; implement `on_replay_saved` + `log_replay_saved`; add the signal connect in `setup_outputs` after the existing `stop` connect; add the matching signal disconnect in `teardown_locked` before the existing `stop` disconnect or alongside it). Estimated ~120 LOC added, ~5 LOC removed.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | PASS | Only existing libobs APIs used (`signal_handler_connect/_disconnect`, `proc_handler_call`, `obs_data_set_string`, `calldata_*`). The `"saved"` signal and `"get_last_replay"` proc are public OBS replay-buffer output APIs (`obs-ffmpeg-mux.c:961-964`), part of libobs since 2017. |
| II. Clear Ownership & Minimal Shared State | PASS | No cross-slot state added. The filename helper is pure (reads `Config` by const-ref, returns `std::string`). The `saved` callback reads only the slot's own `cfg_.name` and `replay_out_`; no reach into another slot's pipeline. |
| III. Thread Safety (NON-NEGOTIABLE) | PASS | The `saved` callback takes NO plugin locks (mirroring `on_replay_output_stop`). The disconnect-before-release order in `teardown_locked` is preserved. libobs's `signal_handler_disconnect` synchronizes with in-flight dispatch (its internal signal mutex), so the callback cannot fire after teardown's disconnect call returns. The lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` is unaffected; no new locks. See [contracts/replay-save-correctness.md](./contracts/replay-save-correctness.md) § Threading. |
| IV. UI / Logic Separation | PASS | All changes live in `slot.hpp` / `slot.cpp` (SlotManager-internal). No `ui-slot-editor.cpp` or `ui-dock.cpp` changes. |
| V. Encoder Robustness & Graceful Fallback | PASS | The encoder construction path in `SharedEncoder::build` and the x264/CBR fallback are untouched. |
| VI. Pipeline Isolation From OBS Main | PASS | No interaction with OBS main outputs. The plugin's per-slot replay-buffer output remains an isolated `obs_output_t` with the `replay_buffer` ID. |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | PASS, strengthened | Two latent silent-loss paths are closed: (a) cross-slot filename collision is impossible by construction (the unconditional id-derived component in the format string makes two slots' replay paths structurally distinct), (b) the plugin's `replay save OK` line no longer falsely claims success for failed saves. The replay-buffer's configured duration, save trigger semantics, and per-slot independence are unchanged. |
| VIII. Shared Encoder — Literal Semantics | PASS | No encoder changes. |
| IX. Configurable Settings Parity | PASS | No user-configurable setting removed or rendered inoperative. The output directory remains user-configurable; the only behavioural change is that pointing two slots at the same directory no longer causes silent data loss. |

**Result**: PASS. No Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/007-fix-replay-collision/
├── plan.md              # This file (/speckit-plan output)
├── spec.md              # Feature spec (/speckit-specify + /speckit-clarify output)
├── research.md          # Phase 0: design decisions + OBS API audit
├── data-model.md        # Phase 1: filename-construction inputs + state transitions for the truthful-log pair
├── contracts/
│   └── replay-save-correctness.md  # Phase 1: filename + log-truthfulness contracts + threading
├── quickstart.md        # Phase 1: 10-test manual verification procedure
├── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
└── checklists/
    └── requirements.md  # Spec-quality checklist (from /speckit-specify)
```

### Source Code (repository root)

```text
src/
├── manager.hpp          # (unchanged)
├── manager.cpp          # (unchanged)
├── plugin-main.hpp      # (unchanged)
├── plugin-main.cpp      # (unchanged)
├── slot.hpp             # TOUCHED: add namespace replay_util { sanitize_for_filename, build_replay_format }; declare SceneSlot::on_replay_saved static callback and SceneSlot::log_replay_saved instance method.
├── slot.cpp             # TOUCHED: implement replay_util helpers; redirect setup_outputs format-string set-site (line 801) through replay_util::build_replay_format; replace save_replay log wording (line 1066) from "save OK / FAILED" to "save requested / save proc-dispatch FAILED"; connect "saved" signal in setup_outputs (next to existing "stop" connect at line 821-822); disconnect "saved" signal in teardown_locked (next to existing "stop" disconnect at line 667-668); implement on_replay_saved + log_replay_saved.
├── ui-dock.hpp          # (unchanged)
├── ui-dock.cpp          # (unchanged)
├── ui-slot-editor.hpp   # (unchanged)
└── ui-slot-editor.cpp   # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. Two `.cpp`/`.hpp` files modified across three logical changes (filename helper, format-string redirect, truthful-log + signal subscription). No new translation units, no CMake changes, no save-format additions, no UI changes.

## Phase 0 — Research

The Phase 0 deliverable is [research.md](./research.md). All eight design decisions are resolved there with rationale + alternatives considered. No NEEDS CLARIFICATION items remain.

Topics covered:

- **D1**: OBS replay-buffer outcome-confirmation API surface — `"saved"` signal + `"get_last_replay"` proc handler.
- **D2**: Choice of identifier source — `cfg_.id` (opaque persisted), `cfg_.name` (user-facing), or both.
- **D3**: Identifier length and extraction window — last 6 hex chars of `cfg_.id`.
- **D4**: Sanitization character set policy — Windows-illegal + control + `%` + reserved-name handling.
- **D5**: Filename component order — slot identity first, then "Replay_" literal, then timestamp.
- **D6**: Lifetime and locking model for the `saved` signal callback.
- **D7**: Wording of the "neutral when outcome unknown" log line.
- **D8**: Where to emit the truthful follow-up "wrote" log line.

## Phase 1 — Design & Contracts

### data-model.md

[data-model.md](./data-model.md) documents:

- `SceneSlot::Config` fields read by the filename helper (`id`, `name`, `container`) — no new fields.
- The runtime `"format"` setting passed to the replay-buffer output: composition rule, sanitization rule, fallback rule when name sanitizes empty.
- State transitions for a single Save Replay operation: `request` → (proc dispatched | proc-dispatch FAILED) → (saved signal fires | no signal) → corresponding log line emissions.
- Threading: which method runs on which thread, what synchronization libobs provides at the `saved`-signal dispatch site, why the callback takes no plugin locks.
- Backward / forward compatibility for pre-fix on-disk replay files (none — they co-exist; the plugin does not scan them).

### contracts/

[contracts/replay-save-correctness.md](./contracts/replay-save-correctness.md) documents:

- **Filename contract**: the runtime `"format"` string is `"<sanitized-name>_<id6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"`; sanitization rules; id6 derivation rule; empty-name fallback rule.
- **Log-truthfulness contract**: the `request` line is emitted at the proc-call site (LOG_INFO with the slot name); the `wrote` line is emitted from the `saved` signal callback (LOG_INFO with the slot name and the `get_last_replay` path); a `request` with no corresponding `wrote` is the failure signal.
- **Threading contract**: the `saved` callback takes no plugin locks; the disconnect-before-release order in `teardown_locked` is the synchronization barrier; libobs's `signal_handler_disconnect` is documented to block on in-flight dispatch.
- **OBS API surface consumed**: `signal_handler_connect/_disconnect`, `obs_output_get_signal_handler`, `obs_output_get_proc_handler`, `proc_handler_call`, `calldata_init/_string/_free` — all already in use elsewhere in this codebase.
- The constitution-principle mapping (every principle accounted for).

### quickstart.md

[quickstart.md](./quickstart.md) covers 10 tests:

1. US1 headline bug — two slots same dir, simultaneous saves, both files present.
2. US1 generalization — three slots same dir, simultaneous saves, all three files present.
3. US1 different-container same-dir — mp4 + mkv co-existence preserved.
4. US2 attribution — file names embed slot identity readably.
5. US2 same-name-same-dir — distinct file names by construction.
6. US2 empty-name fallback — slot identity carried by id6 component when `cfg.name` is empty.
7. US3 truthful log on success — `request` then `wrote '<path>'` pair appears for every successful save.
8. US3 truthful log on failure — `request` with no `wrote` follow-up when output dir is unwritable.
9. Edge case — slot name containing Windows-reserved characters (`<>:"/\|?*`) is sanitized, save succeeds.
10. Regression — pre-fix `Replay_<ts>.<ext>` files co-existing in the directory cause no errors at scan/load/save.

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point to this plan, replacing the pointer to feature 006's plan.

## Re-check Constitution after Phase 1

No new threads. No new locks. No new persisted state. No new UI dependencies. No new translation units. The save-format shape is unchanged (the only change touching libobs settings is the transient runtime `"format"` setting passed to the replay-buffer output, which is not persisted). All nine principles remain satisfied — including the two NON-NEGOTIABLE ones (III Thread Safety, VII Recording & Replay Buffer Correctness). No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
