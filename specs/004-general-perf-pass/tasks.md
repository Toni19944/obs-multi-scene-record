---

description: "Task list for the General performance pass feature"
---

# Tasks: General performance pass (non-recording subsystems)

**Input**: Design documents from `specs/004-general-perf-pass/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [quickstart.md](./quickstart.md)

**Tests**: This project has no automated test harness for plugin behavior — verification is manual via the OBS GUI, Task Manager / Resource Monitor (for CPU/memory), and optionally a frame-time tool. Test tasks reference the manual procedures in `quickstart.md` rather than code under `tests/`.

**Organization**: The Phase 0 audit ([research.md](./research.md)) identified two actionable findings (F1, F2); everything else was disposed as KEEP / at parity. Code work is three foundational edits + a build + a format pass. Per-story phases (Phase 3 through Phase 6) are manual verification against the patched build.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel (different files OR independent manual procedures, no dependencies on incomplete tasks)
- **[Story]**: Which user story this verification task belongs to (US1, US2, US3, US4). Only applied to user-story-phase tasks per the spec-kit checklist format.
- Each task includes the exact file path or quickstart test reference

---

## Phase 1: Setup

**Purpose**: Confirm working tree and branch.

- [X] T001 Verify the current branch is `004-general-perf-pass` and that `src/manager.{hpp,cpp}` and `src/ui-dock.cpp` are tracked. No code change in this task — read-only environment check.

---

## Phase 2: Foundational

**Purpose**: Apply F1 + F2 closures. Both findings are in scope for all four user stories' verification.

**⚠️ CRITICAL**: T002 must land before T003 (T003 calls the new `any_running()` method); T004 (build) depends on both.

- [X] T002 Add `bool any_running() const;` to `SlotManager` in `src/manager.hpp` (place the declaration in the public-method block between `unregister_all_hotkeys()` and the persistence section). Implement it in `src/manager.cpp` (place the implementation near `slot_count()` / `slot_at()` for cohesion). The implementation MUST take `std::lock_guard<std::mutex> lk(mtx_);`, iterate `slots_` once, and return `true` if any `s->is_running()` is true (the inner check is a lock-free atomic load on `SceneSlot::running_`, so no nested locking under `slot_mtx_`). Returns `false` for an empty `slots_`. This addresses research finding F1 by giving the dock a single-call probe for "should the stats timer be running."

- [X] T003 In `src/ui-dock.cpp` `MultiSceneRecordDock::refresh()` (currently at lines 140-194), apply two changes:
  1. **Cell-widget reuse (F2)**: in the per-row loop, before the `auto* sb = new QPushButton(...)` block at lines 162-171, look up any existing state-button widget via `qobject_cast<QPushButton*>(table_->cellWidget(i, COL_STATE))`. If it's non-null, reuse it: call `setText(state_btn_text(running, c.replay_only))` and `setStyleSheet(state_btn_style(running))` on it. Only allocate a new `QPushButton` (with the `setFlat`, `setCursor`, `connect`, and `setCellWidget` calls) when the lookup returned null (i.e., the row was just added). The signal/slot connection on existing buttons stays valid across refreshes — the lambda captures `i` by value and `i` matches the button's row position (rows below a deletion are auto-truncated by `setRowCount(n)`, not shifted; rows above a deletion never move).
  2. **Stats-timer gating (F1)**: after the row loop completes (just before `refresh_stats();` at line 193), add a block that consults `mgr.any_running()` and starts or stops `stats_timer_` accordingly: if `stats_enabled_ && mgr.any_running()` is true, call `stats_timer_->start()` if `!stats_timer_->isActive()`; otherwise call `stats_timer_->stop()` if `stats_timer_->isActive()`. This keeps the timer in sync with running state across every state transition (add, remove, edit, start, stop, hotkey toggle, external stop) because `refresh()` is called after each.

  Depends on T002 (uses `any_running()`).

- [X] T004 Build the plugin against OBS Studio 31.1.1+ on the local platform. Confirm zero compilation errors and zero new warnings in `src/manager.{hpp,cpp}` and `src/ui-dock.cpp`, and that the plugin loads in OBS. Depends on T002, T003.

**Checkpoint**: The two closeable audit findings are closed. Verification phases can begin.

---

## Phase 3: User Story 1 — Dock and slot controls feel instant (Priority: P1)

**Goal**: confirm dock UI actions remain responsive (~100 ms) with F2's cell-widget reuse in place.

**Independent Test**: Quickstart Test 4.

- [ ] T005 [US1] Execute quickstart Test 4 (10 slots configured; click each dock action — Add, Edit, Remove, state toggle, Start All, Stop All — and confirm visible effect within ~100 ms with no perceptible UI freeze) from `specs/004-general-perf-pass/quickstart.md`. Compare against pre-fix feel if possible. Depends on T004.

**Checkpoint**: User Story 1 verified.

---

## Phase 4: User Story 2 — Plugin has near-zero cost when idle (Priority: P1)

**Goal**: confirm F1's timer-gating eliminates per-stopped-slot idle CPU tax.

**Independent Test**: Quickstart Tests 1, 2, and 3.

- [ ] T006 [P] [US2] Execute quickstart Test 1 (cold-start OBS with no slots, 60-second idle CPU measurement) from `specs/004-general-perf-pass/quickstart.md`. Record the median plugin-attributable CPU value as institutional memory. Depends on T004.

- [ ] T007 [P] [US2] Execute quickstart Test 2 (10 stopped slots, 60-second idle CPU measurement). Confirm post-fix value is essentially identical to Test 1's baseline (no per-stopped-slot tax) per SC-003. Depends on T004.

- [ ] T008 [P] [US2] Execute quickstart Test 3 (timer pauses when no slots running, resumes when first slot starts, pauses again when last slot stops). Verify F1's fix end-to-end across single-slot and Start-All / Stop-All transitions. Depends on T004.

**Checkpoint**: User Story 2 verified — idle CPU is at parity with the "no slots" baseline regardless of configured slot count.

---

## Phase 5: User Story 3 — Lifecycle operations don't block the UI thread (Priority: P2)

**Goal**: confirm the dock and save callback don't freeze OBS UI under realistic workloads.

**Independent Test**: Quickstart Tests 5 and 6.

- [ ] T009 [P] [US3] Execute quickstart Test 5 (10 slots, 5 running + 5 idle; rapid-fire 20× Ctrl+S; no visible OBS UI freeze; dock remains clickable between saves) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

- [ ] T010 [P] [US3] Execute quickstart Test 6 (scene-collection switch round-trips with 5 running slots; no visible freeze across 5 round-trips) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

**Checkpoint**: User Story 3 verified.

---

## Phase 6: User Story 4 — Audit document captures every finding (Priority: P3)

**Goal**: confirm the audit deliverable (research.md) is complete and suitable for future regression triage.

- [ ] T011 [US4] Read through `specs/004-general-perf-pass/research.md` and confirm: (a) every subsystem listed in spec FR-006 has its own section (Stats poll, Dock refresh paths, Slot lifecycle, SlotEditor, Memory baseline, Idle behavior summary); (b) each finding F1–F9 has a CLOSE or KEEP disposition with cited file:line anchors and quoted rationale; (c) the "items considered and ruled out" section captures the three rejected alternatives (rapid-refresh coalescing, static encoder-enum caching, button pooling). No code change in this task — review only.

**Checkpoint**: User Story 4 verified — the audit document is the institutional-memory artifact.

---

## Phase 7: Polish & Cross-Cutting Concerns

- [ ] T012 [P] Execute quickstart Test 7 (cold-start + 10-slot resident memory measurements; record the per-stopped-slot delta as institutional memory) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

- [ ] T013 [P] Execute quickstart Test 8 (1 slot running for 10 seconds; Frames / Dropped / Kbps columns update once per second — regression check that F1's timer-gating doesn't break the running-slot stats display) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

- [ ] T014 [P] Execute quickstart Test 9 (Show-stats checkbox toggle off → columns hide and timer pauses; toggle back on → columns reappear and timer resumes) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

- [ ] T015 [P] Execute quickstart Test 10 (hotkey-driven start/stop on 1 slot; confirm dock state column tracks every transition correctly — feature 002 regression check on the cross-thread refresh path) from `specs/004-general-perf-pass/quickstart.md`. Depends on T004.

- [ ] T016 Execute quickstart Test 11 (full regression sweep — run the entire quickstart procedures of features 001, 002, and 003 against the feature-004 build). This is the long-pole verification task. Depends on T004.

- [ ] T017 Execute quickstart Test 12 (crash check; monitor OBS log + Event Viewer throughout T005–T016 for any plugin crash, OBS crash, or hang attributable to feature 004 changes). Depends on T004.

- [ ] T018 Apply the project's clang-format gate to the three files touched (`src/manager.hpp`, `src/manager.cpp`, `src/ui-dock.cpp`). Depends on T002, T003.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — read-only environment check.
- **Foundational (Phase 2)**: T002 stands alone. T003 depends on T002 (uses `any_running()`). T004 (build) depends on both.
- **Phases 3, 4, 5, 6 (User Stories)**: All depend on T004. Independent of each other (different test procedures against the same build).
- **Phase 7 (Polish)**: T012–T017 depend on T004; T018 depends only on T002 + T003 (formatting can run as soon as edits land).

### User Story Dependencies

- **US1 (P1)**: depends on T004. Independent of US2/US3/US4.
- **US2 (P1)**: depends on T004. Independent of US1/US3/US4.
- **US3 (P2)**: depends on T004. Independent of US1/US2/US4.
- **US4 (P3)**: depends only on research.md existing. Independent of the code build.

### Parallel Opportunities

- All US2 verification tasks (T006, T007, T008) are mutually parallel — independent benchmark runs.
- All US3 verification tasks (T009, T010) are mutually parallel.
- US1, US2, US3, US4 verification phases can all run in parallel against the same build once T004 is green.
- Polish tasks T012–T015 are mutually parallel (independent quickstart tests).
- T016 (full regression sweep) and T017 (crash monitoring) are best run after the per-story phases land, as they confirm no regressions.
- T018 (clang-format) is independent of all verification work.

---

## Parallel Example: Story verification

```text
# After T004 completes:
Tester A — T005                  (US1: dock latency)
Tester B — T006, T007, T008      (US2: idle CPU + timer behavior)
Tester C — T009, T010            (US3: no UI freeze)
Tester D — T011                  (US4: audit doc review — short)
Tester E — T012–T015             (Polish: memory baseline + regression spot-checks)
```

---

## Implementation Strategy

### MVP First (User Story 1 + User Story 2)

The two P1 stories (US1 snappy dock, US2 near-zero idle) are the headline outcomes. After T001–T004 land, completing T005 (US1) and T006/T007/T008 (US2) verifies both. That's the minimum shippable increment for this feature.

### Incremental Delivery

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational (T002 + T003 + T004). Three locations modified.
3. Verify US1 (T005) + US2 (T006–T008). MVP shipped.
4. Verify US3 (T009, T010) and US4 (T011) for full story coverage.
5. Polish: regression sweep (T016), crash check (T017), format (T018), institutional-memory captures (T012, T013, T014, T015).

### Parallel Team Strategy

For a multi-tester pass: one developer drives T002 + T003 + T004 (the code work). Once T004 is green, four testers can split the four user-story phases and the polish phase in parallel (see "Parallel Example" above). T016 (long-pole regression sweep) is the bottleneck — start it early.

---

## Notes

- The audit (US4) is the institutional-memory deliverable. Future changes to the dock or stats path should be cross-checked against research.md to flag any regression that reintroduces the F1 or F2 patterns OR violates FR-002 / FR-005.
- Quickstart Tests 1, 2, and 7 record numerical values (idle CPU, memory baseline) — these are forward-looking artifacts, not pass/fail. If a future feature pushes them up, that's a regression signal.
- T016 (regression sweep across features 001/002/003) is the longest task — schedule it early as the long pole.
- T018 (clang-format) is small but easy to forget — run it before committing.
