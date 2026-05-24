# Implementation Plan: General performance pass (non-recording subsystems)

**Branch**: `004-general-perf-pass` | **Date**: 2026-05-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/004-general-perf-pass/spec.md`

## Summary

The Phase 0 audit (see [research.md](./research.md)) read every subsystem in scope per FR-006 and surfaced **two actionable findings**; everything else was already at the "minimum required for correctness" bar and stays put.

**Finding F1** (matches spec FR-002 verbatim): the 1 Hz stats QTimer runs continuously whenever `stats_enabled_` is true, regardless of whether any slot is actually recording. With 10 stopped slots, the timer still fires every second and `refresh_stats()` iterates all 10 rows. **Fix**: pause the timer when zero slots are running; restart it the moment any slot starts. Centralise the start/stop decision in `MultiSceneRecordDock::refresh()` since `refresh()` already runs after every state transition.

**Finding F2** (matches spec FR-005 verbatim): `MultiSceneRecordDock::refresh()` recreates the per-row state-toggle `QPushButton` cell widget on every call. State changes (single-row start/stop via dock click, hotkey toggle, or external stop) currently trigger a full rebuild of every row's button. `refresh_stats()` already has the right pattern (mutates the existing button in place); `refresh()` should adopt the same pattern. **Fix**: reuse the existing cell widget when present; only allocate a new one when the slot was just added.

Both fixes live in `src/ui-dock.cpp`, plus one new public method on `SlotManager` (`bool any_running() const`) to support F1's check.

Memory baseline (FR-008) and idle-CPU verification (US2) are quickstart deliverables — no code change planned; the audit by inspection found nothing to fix, and the live tests are how we confirm.

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: Qt 6 (`QTimer`, `QTableWidget`, `QPushButton`), libobs (`SceneSlot`, `SlotManager`).

**Storage**: N/A — no persisted state changes.

**Testing**: manual verification per [quickstart.md](./quickstart.md). FPS overlay / Task Manager / Resource Monitor for idle-CPU measurement.

**Target Platform**: Windows x64 (primary; CPU measurement is most actionable here), macOS, Ubuntu 24.04.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: per spec — actions complete within ~100 ms (FR-001), idle CPU at or near 0% (SC-002, SC-003).

**Constraints**:

- All UI work runs on the Qt main (UI) thread. The cross-thread refresh path (slot.cpp → `QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection)`) is preserved.
- Constitution Principle III (lock order): `SlotManager::any_running()` takes `mtx_` to scan `slots_`. Since UI thread can call this, and slot.cpp callbacks (which can also enter `SlotManager`) run on libobs worker threads, the lock-order rule must be observed. `mtx_` is the outermost lock in the chain, so a UI-thread call that takes only `mtx_` (no other lock held first) is safe.
- Per-slot independence (constitution Principle II, feature 003 reaffirmation) is unaffected.
- No new fields on `SceneSlot`. No persisted state changes. No save-format changes.

**Scale/Scope**: two functions modified (`MultiSceneRecordDock::refresh()` in `src/ui-dock.cpp`, plus a new `SlotManager::any_running()` in `src/manager.{hpp,cpp}`). Estimated total: ~15 lines of code added/changed.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | No new libobs API surface. `any_running()` reads `SceneSlot::is_running()` (an `std::atomic<bool>::load()`) — same accessor the dock already uses. |
| II. Clear Ownership & Minimal Shared State | ✅ | No new shared state. `any_running()` is a derived predicate computed at call time from existing `slots_`. |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | `any_running()` takes `mtx_` to read `slots_`. Called from UI thread inside `refresh()`. The UI thread does not hold any other lock when entering, so this acquisition does not invert the global order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_`. The `SceneSlot::is_running()` reads inside the scan are lock-free (atomic load) — they do not nest under `slot_mtx_` here. |
| IV. UI / Logic Separation | ✅ | The new `any_running()` is logic-side (SlotManager). The dock continues to call it via the existing `SlotManager::instance()` accessor — same pattern as `slot_count()` and `slot_at()`. No new direct Qt calls from logic code. |
| V. Encoder Robustness & Graceful Fallback | ✅ | Unrelated. |

**Result**: PASS, no Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/004-general-perf-pass/
├── plan.md              # This file
├── spec.md              # Feature spec
├── research.md          # Phase 0: the audit document (the US4 deliverable)
├── quickstart.md        # Phase 1: manual verification procedure
└── checklists/
    └── requirements.md  # Spec-quality checklist (from /speckit-specify)
```

No `data-model.md` (no entities, no new fields, no state transitions); no `contracts/` (internal change; the audit document itself is the US4 deliverable per the same pattern as feature 003).

### Source Code (repository root)

```text
src/
├── ui-dock.cpp         # TOUCHED: refresh() reuses cell widgets; refresh() also pauses/resumes
│                       #          the stats timer based on SlotManager::any_running().
├── ui-dock.hpp         # (unchanged)
├── manager.cpp         # TOUCHED: new SlotManager::any_running() implementation.
├── manager.hpp         # TOUCHED: declare bool any_running() const;
├── slot.cpp            # (unchanged)
├── slot.hpp            # (unchanged)
├── ui-slot-editor.cpp  # (unchanged — audit found no actionable findings)
├── ui-slot-editor.hpp  # (unchanged)
├── plugin-main.cpp     # (unchanged)
└── plugin-main.hpp     # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. Two functions modified (`refresh()` in `src/ui-dock.cpp`, new `any_running()` in `src/manager.{hpp,cpp}`). No new translation units, no CMake changes.

## Phase 0 — Research (the audit)

The Phase 0 deliverable is the **audit document itself** — [research.md](./research.md) — fulfilling User Story 4 (P3) and Functional Requirements FR-006 / FR-007. Every subsystem in scope per FR-006 has its own section; every finding has a disposition.

Summary table (full details in research.md):

| # | Subsystem | Finding | Disposition |
|---|---|---|---|
| F1 | Stats poll (1 Hz QTimer) | Runs continuously even when zero slots are running | **CLOSE** — pause when no slots running, restart on first start |
| F2 | Dock refresh() | Recreates cell-widget buttons on every call, including for single-row state changes | **CLOSE** — reuse existing cell widgets when present |
| F3 | refresh_stats per-row updates | Calls `setText("--")` on stopped-slot stat cells every tick (redundant) | **KEEP** — Qt's setText with identical value is internally near-free; not worth the bookkeeping |
| F4 | refresh() calls slot_at() N times under separate locks | Per-iteration lock/unlock | **KEEP** — uncontended mutex; cost is sub-microsecond; restructuring complicates the lock-order story |
| F5 | on_start_all path-validation loop | N slot_at() calls before starting | **KEEP** — same as F4; trivial cost |
| F6 | SlotEditor populates combos on every open | Enumerates encoder types, scenes, properties | **KEEP** — opens on user action; not a hot path; ~50 ms is within FR-001's 100 ms budget |
| F7 | Memory baseline | ~few KB plugin loaded, ~1 KB per stopped slot (hotkey sentinel output) | **KEEP** — already minimal; measured baseline becomes institutional memory in quickstart |
| F8 | Frontend event callback | Switches on specific events; no work on default | **KEEP** — already at parity |
| F9 | Save/load callback | Linear in slot count; ~hash-table-of-strings serialization per slot | **KEEP** — sub-millisecond for ≤10 slots; well inside FR-004's 16 ms budget |

**Net code change**: F1 + F2 only.

## Phase 1 — Design & Contracts

### data-model.md

Not generated — this feature adds no entities, no new fields, no state transitions. The new `any_running()` is a derived predicate.

### contracts/

Not generated — this is an internal optimization. The audit document is the institutional-memory artifact.

### quickstart.md

Verification procedure covering idle CPU baseline, dock responsiveness, save-callback latency, memory baseline, and regression checks for features 001/002/003. See [quickstart.md](./quickstart.md).

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point to this plan (replaces the pointer to feature 003).

## Re-check Constitution after Phase 1

No new threads, no new locks, no new persisted state, no new UI dependencies, no new translation units. All five principles remain satisfied. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
