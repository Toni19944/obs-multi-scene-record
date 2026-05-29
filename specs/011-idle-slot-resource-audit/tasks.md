# Tasks: Idle-State Background Resource Audit for Configured-but-Not-Running Slots

**Input**: Design documents from `/specs/011-idle-slot-resource-audit/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, quickstart.md

**Tests**: Not explicitly requested in the feature specification. Manual verification via quickstart.md is the test strategy.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Pre-flight Checks

- Verify `CLAUDE.md` at repo root points to `specs/011-idle-slot-resource-audit/plan.md` as the current plan

---

## Phase 1: Foundational (Blocking Prerequisites)

**Purpose**: Apply the single confirmed correctness fix (F-UD1 CLOSE) that all three user stories depend on to hold.

**Context**: The audit (research.md) found exactly one CLOSE — `on_stats_toggled(true)` starts the 1 Hz stats `QTimer` unconditionally at `ui-dock.cpp:406`, without the `any_running()` gate that `refresh()` already applies at `ui-dock.cpp:218`. This lets the timer tick at true idle (zero slots running) after a stats toggle, violating FR-005/SC-001.

- [X] T001 Gate `stats_timer_->start()` in `MultiSceneRecordDock::on_stats_toggled()` on `SlotManager::any_running()` in `src/ui-dock.cpp:406` — change the unconditional `stats_timer_->start();` to `if (mgr.any_running()) stats_timer_->start();`, reusing the `mgr` reference already obtained at line 402
- [X] T002 Remove the redundant pre-`refresh()` timer start at `src/ui-dock.cpp:117` — delete `if (stats_enabled_) stats_timer_->start();` since the immediately-following `refresh()` at line 119 already gates the timer correctly via the `stats_enabled_ && any_running()` check, making this the single source of truth for timer gating
- [X] T003 Build the plugin and verify the F-UD1 fix: with zero slots running, call `on_stats_toggled(true)` (toggle "Show stats" off then on) and confirm the stats timer does not start; with one slot running, toggle and confirm the timer does start

**Checkpoint**: F-UD1 fix complete. The stats timer now starts **only** when `stats_enabled_ && any_running()`, from both the `refresh()` gate and the `on_stats_toggled` path. No new fields, no signature changes, no lock-order changes.

---

## Phase 2: Audit Completeness

**Purpose**: Confirm the audit document (research.md) is complete against the FR-001 resource-class checklist before proceeding to runtime verification.

- [X] T004 Verify that `specs/011-idle-slot-resource-audit/research.md` covers every resource class enumerated in FR-001: memory, scheduled/periodic work, threads, GPU/render activity, OBS pipeline objects (scene source showing state, view, video output, video encoder, audio encoders, recording/replay outputs, replay-buffer memory), file/OS handles, input (hotkey) registrations, host per-frame/compositing callbacks against inactive outputs, and platform-specific device idle wakeups — confirm each has a finding ID and disposition

---

## Phase 3: User Story 1 — Idle slots impose near-zero standing cost (Priority: P1)

**Goal**: Verify that configured-but-not-running slots impose no per-second background work, no recording-pipeline memory/GPU/OBS objects, and do not keep their scenes' sources active.

**Independent Test**: Configure ten slots covering distinct scenes, start none, and observe the plugin's standing resource footprint per quickstart.md Tests 1–4.

### Implementation for User Story 1

- [ ] T005 [US1] Run quickstart.md Test 1 (F-UD1 repro): configure 10 slots, start none, toggle "Show stats" off then on, confirm no 1 Hz timer fires at idle — validates the F-UD1 fix from Phase 1 (also covers the never-started slot edge case from FR-009: all 10 slots have never been started)
- [ ] T006 [P] [US1] Run quickstart.md Test 2 (idle CPU baseline): measure OBS idle CPU% with 0 slots vs 10 stopped slots, confirm they are indistinguishable (SC-001)
- [ ] T007 [P] [US1] Run quickstart.md Test 3 (scene source not shown): configure a slot targeting a scene with an active capture source, confirm the source is NOT kept active while the slot is idle (FR-002/SC-004)
- [ ] T008 [P] [US1] Run quickstart.md Test 4 (no idle GPU wakeups): configure 10 slots with varied resolutions/encoders, start none, confirm no extra GPU engine usage attributable to the plugin (FR-014)

**Checkpoint**: User Story 1 validated — idle slots impose near-zero standing cost.

---

## Phase 4: User Story 2 — Stopping a slot fully releases its runtime resources (Priority: P2)

**Goal**: Verify that after stopping a running slot, every resource acquired for recording is released and the slot returns to its pre-start idle baseline.

**Independent Test**: Record the idle baseline for a single slot, start it, record briefly, stop it, re-measure — every resource class returns to pre-start values. Per quickstart.md Test 6.

### Implementation for User Story 2

- [ ] T009 [US2] Run quickstart.md Test 6 (stop returns to baseline): start a slot, record ~15 s, stop it, confirm handles + memory return to pre-start values within ~5 s (FR-007/SC-003)
- [ ] T010 [US2] Run quickstart.md Test 6 encoder-group variant: start two slots sharing an encoder group, stop one, confirm the peer keeps recording and the shared pipeline survives; stop the second, confirm the shared pipeline is fully released with no `leaked shared encoder context` in the log

**Checkpoint**: User Story 2 validated — stop fully releases runtime resources.

---

## Phase 5: User Story 3 — Repeated cycles do not accumulate idle cost (Priority: P3)

**Goal**: Verify that start/stop cycles, renames, and edits do not accumulate idle resources over time.

**Independent Test**: Run 100 start/stop cycles and several rename/edit cycles, confirm idle footprint does not grow monotonically. Per quickstart.md Tests 5 and 7.

### Implementation for User Story 3

- [ ] T011 [US3] Run quickstart.md Test 5 (100-cycle leak check): start and stop a single slot 100 times, confirm idle memory + handle count match the pre-cycle baseline with no monotonic growth (SC-005)
- [ ] T012 [US3] Run quickstart.md Test 7 (rename leak check): rename a slot ~10 times, confirm handle count unchanged, hotkey bindings preserved, and Settings group label reflects the latest name (FR-008)

**Checkpoint**: User Story 3 validated — no per-cycle accumulation.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Regression checks, documentation, and constitution compliance.

- [ ] T013 Run quickstart.md Test 8 (regression: features 004/005): start a slot, confirm stats columns update at 1 Hz; stop it, confirm columns show "--" and timer pauses; toggle stats while a slot is running, confirm timer resumes correctly (SC-007)
- [ ] T014 Confirm OBS log shows no `leaked` / `encoder leaked` / `output leaked` warnings after a full session exercising all tests above
- [X] T015 Verify all eight findings in `specs/011-idle-slot-resource-audit/research.md` carry a valid disposition (CLOSE, ACCEPT, or DEFER) with a stated rationale — confirm F-UD1 is CLOSE, F-TIMER-INT is DEFER with actionable recommendations, and the six ACCEPT findings each justify why the retention is the minimum necessary (FR-010/SC-006)
- [X] T016 Add a `CHANGELOG.md` entry for the F-UD1 fix describing the user-visible improvement (e.g., "Confirmed idle-state resource usage is true idle; fixed stats timer starting unnecessarily when toggling Show Stats while no slot is recording")

---

## Dependencies & Execution Order

### Phase Dependencies

- **Pre-flight**: No dependencies — verify before anything else
- **Foundational (Phase 1)**: BLOCKS all subsequent phases (the F-UD1 fix must land before verification)
- **Audit Completeness (Phase 2)**: Depends on Phase 1 — confirms research.md is complete before runtime verification
- **User Stories (Phases 3–5)**: All depend on Phase 2 completion
  - US1, US2, US3 can proceed in parallel once Phase 2 is done
- **Polish (Phase 6)**: Depends on all user stories being verified

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Phase 2 — no dependencies on other stories
- **User Story 2 (P2)**: Can start after Phase 2 — independent of US1
- **User Story 3 (P3)**: Can start after Phase 2 — independent of US1/US2

### Within Each User Story

- All verification tasks marked [P] within the same story can run in parallel
- US1 T005 should run first (it validates the F-UD1 fix directly), then T006–T008 in parallel

### Parallel Opportunities

- T006, T007, T008 (US1 Tests 2/3/4) can run in parallel — they measure different resource classes on independent slot configurations
- T011 and T012 (US3 cycle + rename checks) can run in parallel — they test different lifecycle paths
- T009 and T010 (US2 stop checks) are sequential — T010 builds on the single-slot baseline from T009
- US1, US2, US3 phases can run in parallel once Phase 2 completes (if team capacity allows)

---

## Parallel Example: User Story 1

```text
# After Phase 2, run T005 first to confirm the fix:
Task: T005 — Test 1 (F-UD1 repro validation + never-started edge case)

# Then launch Tests 2/3/4 in parallel:
Task: T006 — Test 2 (idle CPU baseline does not scale with slot count)
Task: T007 — Test 3 (scene source not shown at idle)
Task: T008 — Test 4 (no idle GPU/D3D11 wakeups)
```

---

## Implementation Strategy

### MVP First (Phase 1 + Phase 2 + User Story 1 Only)

1. Pre-flight: verify CLAUDE.md pointer
2. Complete Phase 1: Foundational (apply F-UD1 fix — ~2 LOC change in `src/ui-dock.cpp`, build and verify)
3. Complete Phase 2: Audit Completeness (confirm research.md covers FR-001)
4. Complete Phase 3: User Story 1 (run quickstart Tests 1–4)
5. **STOP and VALIDATE**: idle slots impose near-zero standing cost

### Incremental Delivery

1. Foundational → F-UD1 fix applied and verified
2. Audit Completeness → research.md confirmed complete
3. User Story 1 → idle-cost verification complete (MVP!)
4. User Story 2 → stop-release verification complete
5. User Story 3 → cycle-accumulation verification complete
6. Polish → regression + documentation + CHANGELOG confirmed

---

## Notes

- The entire code change is ~2–3 LOC in `src/ui-dock.cpp` (F-UD1 CLOSE from Phase 1)
- All other phases are verification/documentation — no additional code changes
- The DEFER item (F-TIMER-INT: stats timer interval + per-tick lock traffic) ships no code in this feature; it is documented in research.md with recommendations for a follow-up
- Six ACCEPT findings in research.md are verified correct at idle and require no action
