---

description: "Task list for the Fix Dock UI sync after hotkey-triggered recording feature"
---

# Tasks: Fix Dock UI sync after hotkey-triggered recording

**Input**: Design documents from `specs/002-fix-dock-hotkey-sync/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [quickstart.md](./quickstart.md)

**Tests**: This project has no automated test harness for plugin behavior — verification is manual via the OBS GUI. Test tasks therefore reference the manual procedures in `quickstart.md` rather than code under `tests/`. No TDD tasks are generated.

**Organization**: This is a two-line bug fix in one callback. The code change lives in Phase 2 (Foundational); Phases 3–4 are per-story manual verification against the patched build. Same atomic-fix pattern as feature 001 — there is no per-story implementation slice because both stories are served by the same one-callback change.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1, US2)
- Each task includes the exact file path and (where relevant) the current line range it edits

---

## Phase 1: Setup

**Purpose**: Confirm working tree and branch.

- [X] T001 Verify the current branch is `002-fix-dock-hotkey-sync` and that `src/slot.cpp` is tracked (it was committed at the end of feature 001 in commit `8651826`). No code change in this task — read-only environment check.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Apply the two-line fix. Both user stories are satisfied by this single change.

**⚠️ CRITICAL**: No user story verification can begin until this phase is complete.

- [X] T002 [US1] [US2] Edit `SceneSlot::on_record_hotkey` in `src/slot.cpp` (currently at `src/slot.cpp:909-920`). After the `self->start()` / `self->stop()` branch (i.e., at the end of the function, just before the closing brace), add the two-line dock-refresh post:

  ```cpp
  if (auto* dock = get_dock())
      QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);
  ```

  This mirrors the existing pattern in `on_rec_output_stop` (`src/slot.cpp:965-967`) and `on_replay_output_stop` (`src/slot.cpp:980-982`). Required includes (`<QMetaObject>` and `plugin-main.hpp`) are already present in `src/slot.cpp` per the existing call sites. Do NOT edit `on_save_hotkey` (per FR-008 — save-replay is a transient action, not a state transition).

- [X] T003 Build the plugin against OBS Studio 31.1.1+ on the local platform. Confirm zero compilation errors and zero new warnings in `src/slot.cpp`, and that the plugin loads in OBS without missing-symbol errors. Depends on T002.

**Checkpoint**: The fix is in place and the plugin loads. Per-story verification can begin (Phases 3 and 4 may run in parallel).

---

## Phase 3: User Story 1 — Dock state column tracks hotkey-triggered recording (Priority: P1) 🎯 MVP

**Goal**: After a hotkey-initiated start/stop, the dock state column reflects the new state within 1 second, without any dock click.

**Independent Test**: Quickstart Tests 1 (start sync), 2 (stop sync), and 3 (cross-slot isolation) in `specs/002-fix-dock-hotkey-sync/quickstart.md`.

- [ ] T004 [P] [US1] Execute quickstart Test 1 (hotkey-initiated start → dock state column flips to active within 1 s, no click) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Record pass/fail. Depends on T003.

- [ ] T005 [P] [US1] Execute quickstart Test 2 (hotkey-initiated stop → dock state column flips to idle within 1 s) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Record pass/fail. Depends on T003.

- [ ] T006 [P] [US1] Execute quickstart Test 3 (pressing Slot A's hotkey updates only Slot A's row; Slot B is unaffected) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Record pass/fail. Depends on T003.

**Checkpoint**: User Story 1 verified — dock UI now follows hotkey-initiated state transitions.

---

## Phase 4: User Story 2 — Failed hotkey-triggered start leaves the dock honest (Priority: P2)

**Goal**: When a hotkey-initiated start fails (missing path, encoder failure, etc.), the dock state column stays at idle — no transient fake "active" appears.

**Independent Test**: Quickstart Test 4 in `specs/002-fix-dock-hotkey-sync/quickstart.md`.

- [ ] T007 [US2] Execute quickstart Test 4 (slot configured to guarantee a start failure → hotkey press → dock state column never shows active) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Record pass/fail. Depends on T003.

**Checkpoint**: User Story 2 verified — failed starts no longer mislead the dock.

---

## Phase 5: Polish & Cross-Cutting Concerns

**Purpose**: Edge cases and regression spot-checks from the quickstart.

- [ ] T008 [P] Execute quickstart Test 5 (hotkey works while dock is closed; dock shows correct state when re-shown) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Depends on T003.

- [ ] T009 [P] Execute quickstart Test 6 (rapid repeated hotkey presses → final dock state matches actual `is_running()`) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Depends on T003.

- [ ] T010 [P] Execute quickstart Test 7 (external/error stop still refreshes the dock — regression check for the existing `on_rec_output_stop` path) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Depends on T003.

- [ ] T011 [P] Execute quickstart Test 8 (hotkey across slot removal — no crash, no stale row reappears) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Depends on T003.

- [ ] T012 [P] Execute quickstart Test 9 (Save Replay hotkey does NOT redraw the dock state column — FR-008 regression check) from `specs/002-fix-dock-hotkey-sync/quickstart.md`. Depends on T003.

- [ ] T013 [P] Execute the Regression spot-checks at the bottom of `specs/002-fix-dock-hotkey-sync/quickstart.md`: dock-button click, Start All / Stop All, 1 Hz stats auto-refresh, slot edit/rename. Depends on T003.

- [X] T014 Apply the project's clang-format gate to `src/slot.cpp` (the only file touched). Depends on T002.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — read-only environment check.
- **Foundational (Phase 2)**: T002 stands alone. T003 (build, performed by the human contributor) depends on T002.
- **Phases 3 and 4 (User Stories)**: Both depend on T003. Independent of each other (different test procedures against the same build).
- **Phase 5 (Polish)**: All tasks depend on T003. All polish tasks T008–T013 are mutually parallel; T014 (formatting) depends only on T002.

### User Story Dependencies

- **US1 (P1)**: depends on T003. Independent of US2.
- **US2 (P2)**: depends on T003. Independent of US1.

No inter-story dependencies — the underlying implementation is shared; each story's verification exercises a different facet of the same two-line change.

### Parallel Opportunities

- All US1 verification tasks (T004, T005, T006) are mutually parallel.
- US1 (Phase 3) and US2 (Phase 4) can run in parallel once T003 lands.
- Polish tests T008–T013 are mutually parallel.

---

## Parallel Example: Story verification

```text
# After T003 completes:
Tester A — T004, T005, T006     (US1)
Tester B — T007                  (US2)
Tester C — T008..T013            (Polish edges + regressions)
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational (T002 + T003). The two-line code edit is the entire fix.
3. Complete Phase 3: User Story 1 verification (T004–T006).
4. **STOP and VALIDATE**: at this point the primary user-visible bug is gone. US2 (failed-start honesty) and polish are nice-to-have follow-ups.

### Incremental Delivery

The implementation is atomic — one 2-line change. There is no partial fix to ship. After T003, both stories are simultaneously addressed; further verification is about coverage depth, not additional code.

---

## Notes

- This feature has no automated test harness; every verification task is manual against a live OBS instance per `quickstart.md`. Record results when checking off each task.
- [P] tasks = different files OR independent manual procedures, with no dependency on incomplete tasks.
- The [Story] label on T002 (US1, US2) marks that the single code change directly underpins both user stories.
- Commit after T003 lands green, then again after the verification phases pass.
