# Implementation Plan: Idle-State Background Resource Audit for Configured-but-Not-Running Slots

**Branch**: `011-idle-slot-resource-audit` | **Date**: 2026-05-29 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/011-idle-slot-resource-audit/spec.md`

## Summary

The Phase 0 audit ([research.md](./research.md)) traced every resource class a **configured-but-not-running** slot can hold while idle, across `slot.cpp`/`slot.hpp` (slot + `SharedEncoder` lifecycle), `manager.cpp`/`manager.hpp` (registry, CRUD, load/save), `ui-dock.cpp` (the stats timer), and `plugin-main.cpp` (module-level callbacks). It cross-referenced the three concerns raised during clarification: host per-frame callbacks against inactive outputs (FR-012), the dock timer's interval and per-tick scaling (FR-013), and platform-specific GPU/D3D11 idle wakeups from held video pipelines (FR-014).

**Result — one confirmed leak to CLOSE, one acceptable-but-reducible cost to DEFER, the rest ACCEPT:**

- **F-UD1 (CLOSE)** — `MultiSceneRecordDock::on_stats_toggled(true)` (`ui-dock.cpp:406`) starts the 1 Hz stats `QTimer` **unconditionally**, with no `SlotManager::any_running()` gate. Toggling "Show stats" off→on while no slot is running starts the timer and leaves it ticking at idle until the next `refresh()` (a state transition) re-gates it. This violates the zero-idle-work bar (FR-005 / SC-001) established by feature 004 and is exactly the idle-tick the clarification flagged. **Fix**: gate the `stats_timer_->start()` in `on_stats_toggled` on `mgr.any_running()`, mirroring the gate already in `refresh()` (`ui-dock.cpp:218`). Same change makes the dock constructor's pre-`refresh()` `stats_timer_->start()` (`ui-dock.cpp:117`) the single redundant start; it self-corrects via the `refresh()` at `:119`, but is simplified to rely on that one gate. ~3 LOC, widget-internal, no signature change.

- **F-TIMER-INT (DEFER)** — the stats timer interval is hardcoded at 1000 ms (`ui-dock.cpp:107`) and `refresh_stats()` takes `SlotManager::mtx_` once per slot via `slot_at()` each tick. Because the timer is **paused at true idle** (after F-UD1), this is a *while-recording* cost, not an idle cost — acceptable-but-reducible per the 2026-05-29 clarification. Documented with a recommendation (make interval configurable and/or snapshot slot pointers once per tick) and DEFERRED to a follow-up.

- **ACCEPT (verified correct at idle, no leak)**:
  - **Scene "showing" (FR-002)** — `obs_source_inc_showing` runs **only** inside `SharedEncoder::build` (`slot.cpp:470`) and is matched by `obs_source_dec_showing` in `~SharedEncoder` (`slot.cpp:561`). A not-running slot holds no `SharedEncoder`, so it keeps **no** scene showing → its cameras/captures are not kept active. Verified correct.
  - **Video pipeline / GPU (FR-003, FR-014)** — the `SharedEncoder` (scene/view/`video_t`/encoder) is refcounted, built on the first running consumer's `acquire_shared_encoder` and destroyed on the last `release_shared_encoder` (`manager.cpp:235`/`:273`). The `shared_` registry is **empty** when nothing runs; `shutdown()` logs a leak for any survivor (`manager.cpp:31`). No held-but-unused video pipeline at idle → no idle GPU/D3D11 wakeups attributable to this plugin.
  - **Outputs / replay buffer (FR-004)** — `rec_out_`, `replay_out_`, audio encoders, and replay-buffer memory are created in `setup_outputs`/`setup_encoders` and released in `teardown_locked` (`slot.cpp:843`). Idle holds none.
  - **Inert hotkey output + hotkeys (FR-006, FR-012)** — `hotkey_out_` is one never-started `ffmpeg_muxer` output per slot, created in `register_hotkeys` (`slot.cpp:1116`) solely for the Settings→Hotkeys group label, plus two registered hotkeys. An inactive output runs no encode/mux thread and receives **no per-frame/compositing callback** — verified against FR-012. This is the minimal legitimate idle retention (Constitution IX: hotkeys are a shipped setting). Rename destroy+recreates it leak-free (`unregister_hotkeys` releases it; dtor releases any pending arrays).
  - **Stop returns to baseline (FR-007) / no per-cycle accumulation (FR-008)** — `~SceneSlot` → `stop()` + `unregister_hotkeys()`; `remove_slot`/`load_from` rebuild release per-slot resources under the correct lock order. Verified by inspection; backstopped by the quickstart 100-cycle and rename checks.
  - **Module-level callbacks** — `frontend_event_cb` + `save_cb` are registered once in `init()` (`manager.cpp:11`), not per-slot and not periodic; no idle cost that scales with slot count.

**Pass log (FR-010)**: one inspection pass over the in-scope files; one CLOSE edit (`ui-dock.cpp`), zero contract-affecting changes (the edit is internal to a Qt slot, crosses no translation-unit boundary). The audit converges in one pass. Recorded in [research.md § Pass log](./research.md#pass-log).

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: Qt 6 (`QTimer`, `QCheckBox`, `QTableWidget`, `QPushButton`), libobs (`obs_source_inc_showing`/`dec_showing`, `obs_view_*`, `obs_encoder_*`, `obs_output_*`, hotkey API), `obs-frontend-api`.

**Storage**: N/A — no persisted-state changes. (The existing `stats_enabled` UI preference is unchanged.)

**Testing**: manual verification per [quickstart.md](./quickstart.md). Task Manager / Resource Monitor (idle CPU + memory + handle count); a GPU activity tool (Windows Task Manager GPU view, GPU-Z, or PresentMon) for idle GPU/D3D11 wakeups; OBS log window for `leaked … context` / `encoder leaked` / `output leaked` warnings.

**Target Platform**: Windows x64 (primary — CPU/handle/GPU-wakeup measurement is most actionable here per FR-014), macOS, Ubuntu 24.04.

**Performance Goals**: per spec — zero scheduled work at true idle (FR-005 / SC-001), idle memory bounded by a small per-slot constant with no recording-pipeline allocations (SC-002), all idle resources return to the pre-start baseline within ~5 s of stop (SC-003), no monotonic growth over 100 start/stop cycles (SC-005). The audit must not regress the running-slot path or features 004/005 (SC-007).

**Constraints**:

- Constitution Principle III (lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf): F-UD1 adds one `SlotManager::any_running()` call on the Qt main thread inside `on_stats_toggled`. `any_running()` takes only `mtx_` (outermost) and the per-slot `is_running()` reads are lock-free atomics — identical to the call `refresh()` already makes (`ui-dock.cpp:218`). No new lock, no lock-order change.
- Constitution Principle IV (UI / Logic Separation): F-UD1 is internal to the dock widget and routes through `SlotManager::any_running()` (a logic-side accessor) — the same pattern `refresh()` uses. No new direct libobs/`obs-frontend-api` call from the widget layer.
- Constitution Principle II (Clear Ownership), VI (Pipeline Isolation), VIII (Shared Encoder Literal Semantics): untouched — the fix does not enter the slot/encoder pipeline at all.
- No new fields on `SceneSlot`/`SceneSlot::Config`. No save-format change. No new translation units, no CMake change.

**Scale/Scope**: one function modified — `MultiSceneRecordDock::on_stats_toggled()` (`ui-dock.cpp`), gating the timer start on `any_running()`, plus a one-line tidy of the constructor's pre-`refresh()` start. Estimated ~3–4 LOC changed. The DEFER item ships no code.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | No new libobs API surface. F-UD1 only adds a read of `SlotManager::any_running()` (already public, already called by `refresh()`). |
| II. Clear Ownership & Minimal Shared State | ✅ | No ownership change. The audit confirms `SharedEncoder` symmetric-consumer refcounting releases cleanly to an empty registry at idle. |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | F-UD1's added `any_running()` call runs on the Qt main thread, takes only outermost `mtx_`, and matches the existing `refresh()` call — no new lock, no order inversion. |
| IV. UI / Logic Separation | ✅ | F-UD1 is widget-internal and routes through `SlotManager`; no new direct OBS call from the dock. |
| V. Encoder Robustness & Graceful Fallback | ✅ | The x264/CBR fallback path and `[CBR fallback]` indicator are untouched. |
| VI. Pipeline Isolation From OBS Main | ✅ | The fix never touches any output/encoder/view; OBS main pipeline unaffected. |
| Product Quality Bar (idle overhead) | ✅ | F-UD1 directly enforces "Plugin overhead in idle MUST be negligible" — it removes the one path that left a 1 Hz tick running with no slot recording. |

**Result**: PASS, no Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/011-idle-slot-resource-audit/
├── plan.md             # This file (/speckit-plan output)
├── spec.md             # Feature spec (/speckit-specify + /speckit-clarify output)
├── research.md         # Phase 0: the audit document (findings + CLOSE/ACCEPT/DEFER dispositions + pass log)
├── quickstart.md       # Phase 1: manual verification procedure
└── checklists/
    └── requirements.md # Spec-quality checklist (from /speckit-specify)
```

No `data-model.md` (no entities, no new fields, no state transitions) and no `contracts/` (internal change; the audit document is the deliverable — same pattern as features 003/004/005).

### Source Code (repository root)

```text
src/
├── manager.cpp         # (unchanged — audited: registry empty at idle; load_from/remove release cleanly)
├── manager.hpp         # (unchanged)
├── plugin-main.cpp     # (unchanged — audited: module callbacks registered once, not periodic)
├── plugin-main.hpp     # (unchanged)
├── slot.cpp            # (unchanged — audited: inc_showing/pipeline/outputs all confined to running state)
├── slot.hpp            # (unchanged)
├── ui-dock.cpp         # TOUCHED: F-UD1 — gate stats_timer_->start() in on_stats_toggled on any_running(); tidy ctor start.
├── ui-dock.hpp         # (unchanged)
├── ui-slot-editor.cpp  # (unchanged — out of idle-retention scope; note: carries unrelated in-progress feature-010 edit, NOT part of this feature)
└── ui-slot-editor.hpp  # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. One function modified in one `.cpp` file. No new translation units, no CMake changes, no save-format changes.

## Phase 0 — Research (the audit)

The Phase 0 deliverable is the **audit document itself** — [research.md](./research.md) — fulfilling FR-001 / FR-010 and the User Story deliverables. Every in-scope resource class has a section; every finding carries a CLOSE / ACCEPT / DEFER disposition with code references; the pass log records convergence per FR-010.

## Phase 1 — Design & Contracts

### data-model.md

Not generated — no entities, no new fields, no state transitions. The single edit is a guard added to an existing Qt slot.

### contracts/

Not generated — internal change with no external interface. The audit document is the institutional-memory artifact (same as 003/004/005).

### quickstart.md

Manual verification procedure covering: idle CPU baseline (0 slots / 10 stopped slots / the F-UD1 stats-toggle-while-idle repro), idle GPU/D3D11-wakeup check (FR-014), idle memory + 100-cycle leak check (SC-002/SC-005), scene-source-not-shown check (FR-002/SC-004), stop-returns-to-baseline check (FR-007/SC-003), rename leak check (FR-008), inactive-output no-frame-callback reasoning (FR-012), and regression checks for features 004/005 (SC-007). See [quickstart.md](./quickstart.md).

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point to this plan (replaces the stale pointer to feature 010's plan).

## Re-check Constitution after Phase 1

No new threads, no new locks, no new persisted state, no new UI→OBS dependency, no new translation units, no save-format change. All principles remain satisfied. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
