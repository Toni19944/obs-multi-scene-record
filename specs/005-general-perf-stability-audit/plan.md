# Implementation Plan: General performance and stability audit across all source files

**Branch**: `005-general-perf-stability-audit` | **Date**: 2026-05-20 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/005-general-perf-stability-audit/spec.md`

## Summary

The Phase 0 audit ([research.md](./research.md)) walked the ten in-scope files (`manager.cpp`, `manager.hpp`, `plugin-main.cpp`, `plugin-main.hpp`, `slot.cpp`, `slot.hpp`, `ui-dock.cpp`, `ui-dock.hpp`, `ui-slot-editor.cpp`, `ui-slot-editor.hpp`) on three axes (performance / stability / cleanup) and surfaced **ten findings**: four closeable, six accepted with documented rationale.

**Closeable findings (four, all non-contract-affecting)**:

- **F-M1** (`manager.cpp`) — `stop_all()` holds `mtx_` across `s->stop()`, which can block ~5 s per slot inside `wait_for_output_stop()`. Mirror `start_all()`'s snapshot pattern: take `mtx_` briefly to collect raw pointers, release, then iterate. No signature change.
- **F-S1** (`slot.cpp`) — `SceneSlot::start()` redundantly writes `running_.store(true)` at the bottom of the success path; the CAS at the top of the function already set it. Single-line removal.
- **F-UD1** (`ui-dock.cpp`) — `MultiSceneRecordDock::refresh()` allocates fresh `QTableWidgetItem`s for eight text columns per row on every call. Mirror feature 004 F2's cell-widget reuse pattern: mutate the existing items in place; only allocate when the row was just added. No signature change.
- **F-USE1** (`ui-slot-editor.cpp`) — `update_encoder_specific_ui()` fetches `obs_get_encoder_properties(enc_id)` four+ times for the same encoder via repeated calls to `populate_combo_from_encoder_property`. Hoist the fetch once; widen the helper signature to accept an `obs_properties_t*`. The helper is `static private` to `SlotEditor`, called only inside `ui-slot-editor.cpp` — NOT contract-affecting per FR-001 (no external translation unit depends on it).

**Accepted with documented rationale (six)** — see research.md Summary table: F-PM1, F-M2, F-S2, F-S3, F-UD2, F-USE2. All are documented in research.md with the libobs / Qt / OBS-threading constraint that makes the issue non-reachable on the supported thread model.

**Pass log (FR-001)**: Pass 1 produced four edits, **zero contract-affecting**. Per the Clarifications definition (removed/renamed exported symbol, narrowed signature, removed public branch, tightened invariant), nothing in those four edits crosses a translation-unit boundary in a way that requires earlier files to be revisited. The audit converges in one pass. The pass log is recorded in [research.md § Pass log](./research.md#pass-log).

**Memory baseline** (FR-013) and idle-CPU re-verification (US3) are quickstart deliverables — no code change planned; the audit by inspection found nothing to fix on those bars beyond feature 004's existing state.

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: Qt 6 (`QTableWidget`, `QTableWidgetItem`, `QPushButton`, `QTimer`), libobs (`obs_get_encoder_properties`, `obs_output_*`, `obs_view_*`, `obs_encoder_*`, hotkey API), `obs-frontend-api`.

**Storage**: N/A — no persisted state changes.

**Testing**: manual verification per [quickstart.md](./quickstart.md). Task Manager / Resource Monitor for idle-CPU and memory measurement. FPS overlay (for hitch detection during state-toggle / save / scene-collection round-trip).

**Target Platform**: Windows x64 (primary; CPU + memory measurement is most actionable here), macOS, Ubuntu 24.04.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: per spec — dock actions complete within ~100 ms (FR-005), editor open within ~200 ms (FR-005), idle CPU at or near 0 % (SC-005, SC-006). The audit must not regress these.

**Constraints**:

- Constitution Principle III (lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf) must be preserved across all four (a)-disposition fixes. F-M1's refactor of `stop_all()` keeps the snapshot taken under `mtx_` and the per-slot `s->stop()` calls OUTSIDE `mtx_` — identical to the existing `start_all()` pattern, no new lock ordering.
- Constitution Principle IV (UI / Logic Separation) is unaffected — F-UD1 is internal to widget code; F-USE1 is internal to a Qt dialog; neither introduces a new direct libobs call from the widget layer beyond what is already there.
- Constitution Principle II (Clear Ownership) is unaffected — no slot reaches into another slot's pipeline; the `SharedEncoder` symmetric-consumer semantics are untouched.
- Constitution Principle I (Native OBS API Compliance) is unaffected — F-USE1 only changes WHEN `obs_get_encoder_properties` is called (once instead of four times), not WHAT it calls.
- Constitution Principle V (Encoder Robustness & Graceful Fallback) is unaffected — the x264/CBR fallback path in `SharedEncoder::build` and the `[CBR fallback]` UI indicator are not touched.
- Per-slot independence (feature 003 reaffirmation) is unaffected.
- No new fields on `SceneSlot` or `SceneSlot::Config`. No persisted state changes. No save-format changes.

**Scale/Scope**: four functions modified — `SlotManager::stop_all()` (manager.cpp), `SceneSlot::start()` (slot.cpp, one-line removal), `MultiSceneRecordDock::refresh()` (ui-dock.cpp), `SlotEditor::update_encoder_specific_ui()` + `SlotEditor::populate_combo_from_encoder_property()` (ui-slot-editor.cpp). Estimated total: ~30 LOC added/changed, ~5 LOC removed. No new translation units, no CMake changes.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | F-USE1 changes the call frequency of `obs_get_encoder_properties` (4× → 1× per `update_encoder_specific_ui`) but still uses only public libobs APIs. No new API surface introduced. |
| II. Clear Ownership & Minimal Shared State | ✅ | No slot reaches into another slot's pipeline. F-M1's `stop_all` snapshot mirrors `start_all`'s and does not change ownership semantics. `SharedEncoder` registry untouched. |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | F-M1's refactor REDUCES `mtx_` hold time (was up to 5 s × N slots; becomes microseconds) — strictly improves the lock-contention profile. The snapshot-then-iterate pattern matches `start_all` and preserves the global lock order. No new locks introduced. F-S1 removes a redundant atomic store — no lock interaction. F-UD1 / F-USE1 are widget-internal — no lock interaction. |
| IV. UI / Logic Separation | ✅ | F-UD1 and F-USE1 are internal to widget code; they do not introduce new direct libobs / obs-frontend-api calls from widgets (the existing `obs_get_encoder_properties` call site is already in widget code as part of the dialog's introspection — F-USE1 just deduplicates it). |
| V. Encoder Robustness & Graceful Fallback | ✅ | x264/CBR fallback path in `SharedEncoder::build` and the `[CBR fallback]` UI indicator are not touched. |

**Result**: PASS, no Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/005-general-perf-stability-audit/
├── plan.md             # This file (/speckit-plan output)
├── spec.md             # Feature spec (/speckit-specify + /speckit-clarify output)
├── research.md         # Phase 0: the audit document (US5 deliverable + per-pass log per FR-001)
├── quickstart.md       # Phase 1: manual verification procedure
└── checklists/
    └── requirements.md # Spec-quality checklist (from /speckit-specify)
```

No `data-model.md` (no entities, no new fields, no state transitions); no `contracts/` (internal change; the audit document itself is the deliverable per the same pattern as features 003 and 004).

### Source Code (repository root)

```text
src/
├── manager.cpp         # TOUCHED: F-M1 refactor of SlotManager::stop_all() to use the snapshot pattern.
├── manager.hpp         # (unchanged)
├── plugin-main.cpp     # (unchanged — F-PM1 = KEEP)
├── plugin-main.hpp     # (unchanged)
├── slot.cpp            # TOUCHED: F-S1 remove redundant running_.store(true) at end of SceneSlot::start().
├── slot.hpp            # (unchanged)
├── ui-dock.cpp         # TOUCHED: F-UD1 reuse existing QTableWidgetItem objects in refresh() per row.
├── ui-dock.hpp         # (unchanged)
├── ui-slot-editor.cpp  # TOUCHED: F-USE1 hoist obs_get_encoder_properties to once per update_encoder_specific_ui call.
└── ui-slot-editor.hpp  # TOUCHED: F-USE1 widens static-private helper signature to accept obs_properties_t* (intra-file change only).
```

**Structure Decision**: Single-project OBS plugin. Four functions modified across four `.cpp` files plus one signature change in one `.hpp` (intra-class, intra-file usage only). No new translation units, no CMake changes, no save-format changes.

## Phase 0 — Research (the audit)

The Phase 0 deliverable is the **audit document itself** — [research.md](./research.md) — fulfilling User Story 5 (P3) and Functional Requirements FR-008 / FR-009 / FR-010. Every file in scope has its own section; every finding has a disposition; the per-pass log records convergence per FR-001.

## Phase 1 — Design & Contracts

### data-model.md

Not generated — this feature adds no entities, no new fields, no state transitions. The four edits are pure refactors / one-line removals.

### contracts/

Not generated — this is an internal optimization / nitpicky cleanup pass. The audit document is the institutional-memory artifact.

### quickstart.md

Verification procedure covering: idle CPU baseline (no slots / 10 stopped slots / 1 running slot), dock responsiveness under 10 slots, editor open latency, save-callback latency, memory baseline + 50-cycle leak check, regression checks for features 001 / 002 / 003 / 004, plus the F-M1-specific check that `Stop All` no longer holds `mtx_` for the wait duration. See [quickstart.md](./quickstart.md).

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point to this plan (replaces the pointer to feature 004's plan).

## Re-check Constitution after Phase 1

No new threads, no new locks, no new persisted state, no new UI dependencies, no new translation units, no save-format change. All five principles remain satisfied. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
