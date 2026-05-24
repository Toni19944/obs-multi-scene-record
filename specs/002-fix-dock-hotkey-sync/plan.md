# Implementation Plan: Fix Dock UI sync after hotkey-triggered recording

**Branch**: `002-fix-dock-hotkey-sync` | **Date**: 2026-05-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/002-fix-dock-hotkey-sync/spec.md`

## Summary

Inside `SceneSlot::on_record_hotkey` (`src/slot.cpp:909-920`), after the `start()` / `stop()` call, post a queued refresh to the dock using the same pattern already used by the external-stop signal handlers in the same file (`on_rec_output_stop` at `:966-967`, `on_replay_output_stop` at `:981-982`): `if (auto* dock = get_dock()) QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);`. The change is two lines of code and touches no other file.

`on_save_hotkey` is deliberately left unchanged — `save_replay()` is a transient action, not a state transition, and the dock state column does not need to refresh for it (per FR-008).

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: Qt 6 (`QMetaObject::invokeMethod`, `Qt::QueuedConnection`) and the plugin's existing `get_dock()` accessor (`src/plugin-main.cpp:19`, declared in `src/plugin-main.hpp:9`).

**Storage**: N/A — no persisted state changes.

**Testing**: manual verification via the OBS dock + a bound hotkey. See [quickstart.md](./quickstart.md).

**Target Platform**: Windows x64, macOS, Ubuntu 24.04 — identical Qt API surface across all three.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: N/A — the refresh is event-driven; one queued slot invocation per hotkey press.

**Constraints**:

- Hotkey callbacks fire on libobs's hotkey worker thread (not the UI thread). Any UI update MUST be marshalled to the UI thread; direct Qt-widget mutation from the callback is forbidden by Constitution Principle IV. `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` is the established mechanism (already used in `on_rec_output_stop` / `on_replay_output_stop`).
- The dock may be closed or hidden when a hotkey fires. `get_dock()` returns the cached pointer (or null when the dock has never been created); the `if (auto* dock = get_dock())` guard, used identically in the existing call sites, handles this.

**Scale/Scope**: 1-line code change inside one callback. Two lines if counting the `if` guard.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | Uses only Qt + the plugin's own `get_dock()` accessor + libobs APIs already in use. No new libobs surface. |
| II. Clear Ownership & Minimal Shared State | ✅ | No new fields, no new resources, no new shared state. The dock pointer is already exposed via `get_dock()` and used by other slot.cpp callbacks. |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | The refresh is posted via `Qt::QueuedConnection`, which queues onto the dock's owning thread (the UI thread) regardless of which thread the hotkey callback runs on. No new locking. No change to the global lock order. |
| IV. UI / Logic Separation | ✅ | The new line lives in `src/slot.cpp` and does NOT touch any Qt widget directly — it invokes a public Qt slot (`MultiSceneRecordDock::refresh`) by name via `QMetaObject::invokeMethod`. This is the exact pattern the file already uses for external-stop notifications. |
| V. Encoder Robustness & Graceful Fallback | ✅ | Unrelated to encoder selection. |

**Result**: PASS, no Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/002-fix-dock-hotkey-sync/
├── plan.md              # This file
├── spec.md              # Feature spec
├── research.md          # Phase 0: design decisions (which callback, what pattern)
├── quickstart.md        # Phase 1: manual verification procedure
└── checklists/
    └── requirements.md  # Created by /speckit-specify; spec-quality checklist
```

No `data-model.md` (no entities, no new fields, no state transitions); no `contracts/` (this is an internal bug fix — the only "contract" is the existing public Qt slot `MultiSceneRecordDock::refresh()`, which is unchanged).

### Source Code (repository root)

```text
src/
├── slot.cpp            # TOUCHED: add dock refresh in on_record_hotkey after start/stop
├── slot.hpp            # (unchanged)
├── manager.cpp         # (unchanged)
├── manager.hpp         # (unchanged)
├── ui-dock.cpp         # (unchanged — refresh() slot already exists and is what we'll invoke)
├── ui-dock.hpp         # (unchanged)
├── plugin-main.cpp     # (unchanged — get_dock() accessor already exposed)
└── plugin-main.hpp     # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. The change touches only `src/slot.cpp`. No new translation units, no header changes, no CMake changes.

## Phase 0 — Research

The full research is in [research.md](./research.md). Summary of decisions:

| Question | Decision |
|---|---|
| Which hotkey callback gets the refresh? | Only `on_record_hotkey`. `on_save_hotkey` is unchanged — replay-save is a transient action and does not change the slot's `is_running()` state. |
| What refresh mechanism? | `QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection)` — the same pattern already used by `on_rec_output_stop` and `on_replay_output_stop` in the same file. |
| Refresh before or after the `start()` / `stop()` call? | After. The refresh queues onto the UI thread; by the time it runs, the `start()` / `stop()` has returned and `is_running()` reflects the actual post-transition state (including the false return from a failed start). This satisfies FR-003 (no fake "active" state on a failed start). |
| Add coalescing / debouncing for rapid presses? | No. `Qt::QueuedConnection` already serializes invocations on the dock's event loop; the last queued refresh always reflects the final state. Debouncing would add complexity without changing the observable outcome. |
| Touch the dock pointer guard / accessor? | No. The existing `get_dock()` returns the cached dock or null; the `if (auto* dock = get_dock())` pattern (already used 2x in slot.cpp) is reused verbatim. |

## Phase 1 — Design & Contracts

### data-model.md

Not generated — this feature introduces no entities, no new fields, and no state transitions. The only "state" involved is `SceneSlot::running_` (`std::atomic<bool>`), which is read by `MultiSceneRecordDock::refresh()` via `SlotManager::slot_at(i)->is_running()` and is updated by the existing `start()` / `stop()` calls. Nothing changes here.

### contracts/

Not generated — this is an internal change. The only public surface touched is the existing Qt slot `MultiSceneRecordDock::refresh()` (signature: `void refresh()`), which is invoked by name via `QMetaObject::invokeMethod`. The slot's contract is preserved: rebuild all rows from current `SlotManager` state, idempotent, safe to call repeatedly from any thread via QueuedConnection.

### quickstart.md

Manual verification procedure covering the two user stories and the edge cases from the spec. See [quickstart.md](./quickstart.md).

### Agent context update

The `<!-- SPECKIT START --> ... <!-- SPECKIT END -->` block in the repo-root `CLAUDE.md` is updated to point to this plan file (replaces the previous pointer to `specs/001-fix-hotkey-registration/plan.md`).

## Re-check Constitution after Phase 1

No new resources, no new threads, no new locks, no new UI dependencies, no new translation units. The Phase 1 artifacts confirm the gate evaluation from above. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
