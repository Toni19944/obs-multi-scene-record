# Tasks: Full Codebase Performance & Stability Audit

**Input**: Design documents from `/specs/012-idle-slot-resource-audit/`

**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md

## Phase 1: Setup

**Purpose**: Verify audit scope and prepare report structure

- [X] T001 Verify all files in scope exist and are readable: `src/plugin-main.hpp`, `src/plugin-main.cpp`, `src/manager.hpp`, `src/manager.cpp`, `src/slot.hpp`, `src/slot.cpp`, `src/ui-dock.hpp`, `src/ui-dock.cpp`, `src/ui-slot-editor.hpp`, `src/ui-slot-editor.cpp`, `CMakeLists.txt`

---

## Phase 2: User Story 1 — Developer Reviews Audit Report (Priority: P1)

**Goal**: Produce a complete, accurate, actionable audit report covering every source file

**Independent Test**: Read `audit-report.md` and verify each finding references real code, describes a real problem, and proposes a concrete fix

### Implementation

- [X] T002 [P] [US1] Audit `CMakeLists.txt` for build configuration issues (duplicate dependencies, missing targets, unnecessary settings)
- [X] T003 [P] [US1] Audit `src/plugin-main.hpp` and `src/plugin-main.cpp` for module lifecycle issues (load/unload safety, global state, resource leaks)
- [X] T004 [P] [US1] Audit `src/manager.hpp` and `src/manager.cpp` for SlotManager issues (thread safety, lock order, resource lifecycle, persistence correctness, shared encoder registry)
- [X] T005 [P] [US1] Audit `src/slot.hpp` and `src/slot.cpp` for SceneSlot issues (start/stop safety, encoder setup, output lifecycle, hotkey management, stats accuracy, replay buffer correctness)
- [X] T006 [P] [US1] Audit `src/ui-dock.hpp` and `src/ui-dock.cpp` for dock widget issues (UI/logic separation, timer management, table performance, state consistency)
- [X] T007 [P] [US1] Audit `src/ui-slot-editor.hpp` and `src/ui-slot-editor.cpp` for editor dialog issues (combo population, introspection correctness, config persistence, memory management)

**Checkpoint**: All source files audited; all findings collected

---

## Phase 3: User Story 2 — Prioritization of Findings (Priority: P2)

**Goal**: Organize findings by category and severity for developer prioritization

- [X] T008 [US2] Categorize all findings into Performance, Stability/Safety, Correctness, Code Quality with severity ratings (Critical, High, Medium, Low)
- [X] T009 [US2] Assemble the final `audit-report.md` in `specs/012-idle-slot-resource-audit/audit-report.md` with: summary table, files-audited list, categorized findings with all required elements (FR-002), and navigable structure (SC-004)

**Checkpoint**: `audit-report.md` is complete and meets all acceptance scenarios

---

## Phase 4: Polish & Validation

- [X] T010 Validate that no source code files were modified (FR-004 / SC-003)
- [X] T011 Validate that every finding includes all five required elements: file path, line numbers, issue description, proposed fix, rationale (SC-002)
- [X] T012 Update CHANGELOG.md with audit report entry

---

## Dependencies & Execution Order

- **Phase 1** (T001): No dependencies
- **Phase 2** (T002–T007): Depend on T001; all marked [P] — can run in parallel
- **Phase 3** (T008–T009): Depend on Phase 2 completion
- **Phase 4** (T010–T012): Depend on Phase 3 completion
