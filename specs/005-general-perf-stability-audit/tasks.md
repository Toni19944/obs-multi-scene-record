---

description: "Implementation tasks for the general performance and stability audit"
---

# Tasks: General performance and stability audit across all source files

**Input**: Design documents from `specs/005-general-perf-stability-audit/`

**Prerequisites**: [plan.md](./plan.md) (required), [spec.md](./spec.md) (required for user stories), [research.md](./research.md) (audit findings + pass log), [quickstart.md](./quickstart.md) (verification procedure).

**Tests**: Manual only. No automated test harness exists in this project; verification is via [quickstart.md](./quickstart.md) and OBS GUI per the spec's Assumptions. No test tasks are generated.

**Organization**: Tasks are grouped by user story. Each (a)-disposition finding from [research.md](./research.md) is mapped to the user story whose acceptance scenario it serves. The six (b)-disposition findings produce no implementation tasks (they are documented in research.md and stand as-is).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4, US5)
- File paths are exact.

## Path Conventions

- Single C++ project. Source under `src/`. No `tests/` directory (manual verification only).

---

## Phase 1: Setup

**Purpose**: Confirm working environment is ready for the four edits.

- [X] T001 Confirm current branch is `005-general-perf-stability-audit` and working tree is clean of unstaged changes outside this feature's spec directory (run `git status`); abort and re-sync if not.
- [ ] T002 Open OBS Studio 31.1.1+ with the in-tree plugin built once to confirm the baseline build passes prior to any edit (per Constitution Build & Platform Requirements). *(manual — requires user to run build; deferred to T012)*

---

## Phase 2: Foundational

**Purpose**: No foundational work. The four (a)-disposition findings are independent, each in its own file, and the Phase 0 audit (`research.md`) is already the deliverable for US5. Proceed directly to user-story phases.

**Checkpoint**: Phase 0 audit document is complete; the four (a) findings and their target files are unambiguous in [plan.md § Summary](./plan.md#summary) and [research.md § Summary table](./research.md#summary-table).

---

## Phase 3: User Story 1 — Dock and slot controls feel instant (Priority: P1) 🎯 MVP

**Goal**: close every closeable UI-latency finding identified by the audit so dock actions remain ≤100 ms and editor open ≤200 ms under 10-slot workloads, and so a long-running `Stop All` does not block the UI thread on `SlotManager::mtx_`.

**Independent Test**: run [quickstart.md](./quickstart.md) Tests 2, 3, 4 (F-M1 responsiveness during Stop All, F-UD1 dock refresh, F-USE1 editor open). Each visible effect within its target window; no UI freeze during a multi-slot stop.

### Implementation for User Story 1

- [X] T003 [P] [US1] Refactor `SlotManager::stop_all()` in `src/manager.cpp` to mirror `start_all`'s snapshot-then-iterate pattern (finding F-M1). Take `mtx_` once, set `started_ = false`, copy each `slots_[i].get()` raw pointer into a local `std::vector<SceneSlot*> snapshot`, release `mtx_`, then iterate the snapshot calling `s->stop()`. Result: `mtx_` is held microseconds instead of up to ~5 s × N slots while `wait_for_output_stop` runs. Signature unchanged (still `void SlotManager::stop_all()`). Add a one-line comment referencing F-M1.
- [X] T004 [P] [US1] Refactor `MultiSceneRecordDock::refresh()` in `src/ui-dock.cpp` to reuse existing `QTableWidgetItem` objects instead of allocating fresh ones per cell on every call (finding F-UD1). For each text column (`COL_NAME`, `COL_SCENE`, `COL_RES`, `COL_ENC`, `COL_FRAMES`, `COL_DROPPED`, `COL_KBPS`, `COL_REPLAY`): if `table_->item(i, col)` returns non-null, call `setText(...)` on it; only call `setItem(i, col, mk_item(...))` when it returns null (row was just added). Mirror the cell-widget reuse pattern already used for `COL_STATE` (introduced by feature 004 F2). Preserve the `setForeground(...)` color choices on the COL_ENC fallback warning. Signature unchanged.
- [X] T005 [P] [US1] Refactor `SlotEditor::update_encoder_specific_ui()` in `src/ui-slot-editor.cpp` (and widen the signature of the static helper `SlotEditor::populate_combo_from_encoder_property` in `src/ui-slot-editor.hpp` + `src/ui-slot-editor.cpp`) to fetch `obs_get_encoder_properties(enc_id)` exactly once per call (finding F-USE1). Hoist the fetch to the top of `update_encoder_specific_ui`; pass the resulting `obs_properties_t*` into `populate_combo_from_encoder_property` so each of the four combo-population calls (preset / profile / tune / multipass) reuses the same properties object. Destroy the properties object once at the end. The helper's new signature is non-contract-affecting per FR-001 because it is `static private` to `SlotEditor` and not called from any other translation unit (verified in research.md F-USE1 disposition).

**Checkpoint**: build the plugin and confirm it compiles. Then run quickstart Tests 2, 3, 4 manually. All three target windows met. US1 is now independently verified.

---

## Phase 4: User Story 3 — Latent stability risks closed (Priority: P1)

**Goal**: close the one (a)-disposition stability finding (F-S1) and confirm the six (b)-disposition stability findings remain non-reachable on the supported OBS thread model.

**Independent Test**: run [quickstart.md](./quickstart.md) Test 1 (F-S1 regression) and Test 11 (F-S2 latent-race informational probe). No state inconsistency between dock display and recording status.

### Implementation for User Story 3

- [X] T006 [P] [US3] Remove the redundant `running_.store(true);` at the bottom of `SceneSlot::start()` in `src/slot.cpp` (line ~551 — the line directly after the optional replay-buffer start block, just before the `char tracks_str[16] = {0};` log-formatting setup) (finding F-S1). The compare-exchange at the top of `start()` (`if (!running_.compare_exchange_strong(expected, true)) return true;`) already sets `running_` to `true`; every failure path between the CAS and this line resets it to `false` via `running_.store(false); teardown_locked(); return false;`. The line is therefore dead. Removing it tightens the state machine and removes the ambiguity that contributes to the F-S2 latent race window (though F-S2 itself remains (b) — see its disposition).

### No-code verification tasks for User Story 3

- [X] T007 [US3] Verify the six (b)-disposition findings (F-PM1, F-M2, F-S2, F-S3, F-UD2, F-USE2) remain accepted by inspecting `research.md` against current source: each (b) rationale (UI-thread-only callers, leaf-lock discipline, bounded-poll cost, uncontended-mutex argument, dialog-accept-on-UI-thread) should still hold against the post-T003/T004/T005/T006 code. If any rationale is invalidated by an edit in this feature, escalate that finding to (a) and add an implementation task. *(Done: F-S2's rationale strengthened — F-S1's fix changes worst-case symptom from "running_=true with no outputs" to "running_=false with no outputs", a self-consistent post-race state. Other five (b) findings unchanged. Documented in research.md F-S2 row + § slot.cpp F-S2.)*

**Checkpoint**: build and run quickstart Test 1. The 10-cycle start/stop and 10 hotkey toggles produce consistent dock display and OBS log shows no leaked-handle / output-leaked warnings. US3 is now independently verified.

---

## Phase 5: User Story 2 — Idle cost (Priority: P1)

**Goal**: confirm the audit did NOT introduce any new always-on work; feature 004's idle-CPU bar (≈0 % plugin-attributable with 0 / 10 stopped slots) still holds.

**Independent Test**: [quickstart.md](./quickstart.md) Test 5 (idle CPU baseline + no per-stopped-slot tax).

### No-code verification tasks for User Story 2

- [ ] T008 [US2] Run [quickstart.md](./quickstart.md) Test 5. Sample idle CPU% with 0 slots configured (60 s median) and with 10 stopped slots (60 s median). Confirm both medians are at or near 0 % and indistinguishable within measurement noise. Confirm the 1 Hz stats `QTimer` remains paused on idle (the gating logic in `MultiSceneRecordDock::refresh()` at lines 200–209 is untouched by T004's item-reuse refactor — verify with code review and runtime observation). *(manual — code review confirms F1 gating logic untouched by T004; runtime measurement deferred to user / T014.)*

**Checkpoint**: idle-CPU bar holds. US2 is now independently verified — no code change needed for it.

---

## Phase 6: User Story 4 — Multi-pass methodology (Priority: P2)

**Goal**: confirm the multi-pass methodology converged in one pass — i.e., none of the four code edits (T003–T006) produced a contract-affecting change that would force a revisit pass on earlier files. The pass log in [research.md § Pass log](./research.md#pass-log) records this prediction; this phase verifies it against the actual edits.

**Independent Test**: code review + grep — no stale references in any of the ten files to any contract changed by another file.

### No-code verification tasks for User Story 4

- [X] T009 [US4] Run a Pass-2 grep scan across all ten in-scope files (`manager.cpp`, `manager.hpp`, `plugin-main.cpp`, `plugin-main.hpp`, `slot.cpp`, `slot.hpp`, `ui-dock.cpp`, `ui-dock.hpp`, `ui-slot-editor.cpp`, `ui-slot-editor.hpp`). For each (a)-disposition edit made in Phase 3 / Phase 4, search the OTHER nine files for references to: (a) any symbol the edit removed or renamed, (b) any guard / branch the edit removed, (c) any invariant the edit tightened. Expected: zero hits per the prediction in research.md (T003 / T004 are body-only refactors; T005 widens a static-private helper not called externally; T006 removes one line). If any hit is found, that's a contract-affecting edit and FR-001 requires another pass — open a follow-up task to clean up the stale reference and re-run T009. *(Done: scan returned zero stale references; result table recorded in research.md § Pass 2 — grep verification.)*
- [X] T010 [US4] Update `research.md` § Pass log with the **actual** Pass-1 edits (T003, T004, T005, T006) — confirm they match the predicted four. If T009 surfaced any contract-affecting edit, add a Pass-2 row to the table documenting the revisit. If T009 was clean (expected outcome), append a one-line confirmation: "Pass 2 not required — Pass-2 grep scan (T009) found zero stale references; convergence confirmed."

**Checkpoint**: pass log in `research.md` matches reality. US4 is now independently verified.

---

## Phase 7: User Story 5 — Audit document complete (Priority: P3)

**Goal**: confirm `research.md` covers every file in scope with a dedicated section and that every finding has an FR-008 disposition.

**Independent Test**: a maintainer reading `research.md` cold can identify, per file, every finding and its disposition.

### No-code verification tasks for User Story 5

- [X] T011 [US5] Read `research.md` end-to-end. Verify every one of the ten in-scope files has its own `### <filename>` section. Verify the Summary table includes every finding (F-PM1, F-M1, F-M2, F-S1, F-S2, F-S3, F-UD1, F-UD2, F-USE1, F-USE2) with a disposition column populated. Verify the Pass log table is current after T010. Fix any gaps surfaced. *(Done: 10 file sections confirmed (lines 72/81/93/102/117/126/156/163/181/188); 10 findings present across summary table + per-file detail + pass log (40 occurrences); pass log updated by T010.)*

**Checkpoint**: US5 is now independently verified.

---

## Phase 8: Polish & cross-cutting

**Purpose**: final-state validation across the whole feature.

- [X] T012 Build the plugin (Windows: CMake + MSVC 2022 per Constitution Build Requirements). Confirm clean build with zero new warnings introduced by T003–T006. *(Done: `cmake --build build_x64 --config RelWithDebInfo` succeeded — all four touched .cpp files compiled cleanly; `multi-scene-record.dll` produced and copied to rundir; zero warnings.)*
- [ ] T013 Run `check-format` (CMake formatting + C++ source formatting per Constitution Development Workflow). Re-run on each touched file (`src/manager.cpp`, `src/slot.cpp`, `src/ui-dock.cpp`, `src/ui-slot-editor.cpp`, `src/ui-slot-editor.hpp`). Apply any formatter-driven changes. *(Deferred to CI: the project's `.run-format.zsh` pins exactly clang-format 19.1.1; locally-available versions (VS-bundled 19.1.5, system installers) produce subtly different output and flag pre-existing lines that CI accepts. Build passed clean (T012); edits visually match surrounding style. Push the branch and let GitHub Actions' `check-format` job at 19.1.1 be the source of truth; fix any reported lines via `git clang-format` against pinned 19.1.1 if CI flags anything.)*
- [ ] T014 Run the full [quickstart.md](./quickstart.md) sweep (Tests 1 through 14). Record results in a `specs/005-general-perf-stability-audit/results/` directory (gitignored institutional memory). All process-bar SCs (SC-001, SC-002, SC-003, SC-004, SC-009, SC-010) must hold; numeric SCs (SC-005, SC-006, SC-007, SC-008) are recorded as institutional memory. *(Manual — requires user to run OBS GUI with the freshly built `multi-scene-record.dll`.)*
- [ ] T015 Run prior-feature regression sweeps: `specs/001-fix-hotkey-registration/quickstart.md`, `specs/002-fix-dock-hotkey-sync/quickstart.md`, `specs/003-perf-parity-audit/quickstart.md`, `specs/004-general-perf-pass/quickstart.md`. Every test in each must still pass — SC-010. *(Manual — requires user to run OBS GUI.)*

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies.
- **Foundational (Phase 2)**: empty — no blocking prerequisites for this feature.
- **User Story 1 (Phase 3)**: depends on Setup. The three implementation tasks (T003, T004, T005) are independent (different files) and [P]-marked.
- **User Story 3 (Phase 4)**: depends on Setup. T006 is independent of T003–T005 (different file). T007 is verification only, runs after all code edits.
- **User Story 2 (Phase 5)**: depends on Setup; benefits from Phase 3 + Phase 4 completion to verify under final code state. T008 is verification only.
- **User Story 4 (Phase 6)**: depends on T003–T006 being complete (Pass-2 grep needs the actual edits).
- **User Story 5 (Phase 7)**: depends on T010 having updated the pass log.
- **Polish (Phase 8)**: depends on all prior phases.

### User Story Dependencies

- US1 (P1), US2 (P1), US3 (P1) are independently verifiable; their tasks operate on disjoint files / no code (US2).
- US4 (P2) depends on US1 + US3 code edits (it verifies their multi-pass convergence).
- US5 (P3) depends on US4 (it verifies the audit doc is current including the pass log).

### Within Each User Story

- US1: T003, T004, T005 can run fully in parallel (different files, no shared state).
- US3: T006 is independent of US1's tasks. T007 runs after all code edits across US1+US3.
- US2, US4, US5: verification-only, sequential per their internal dependencies.

### Parallel Opportunities

- **All four code edits (T003, T004, T005, T006) can run in parallel.** They touch four disjoint source files (`manager.cpp`, `ui-dock.cpp`, `ui-slot-editor.{hpp,cpp}`, `slot.cpp`). No symbol overlap, no shared header changes (the `ui-slot-editor.hpp` change in T005 is a static-private signature widen and is consumed only by T005's own changes in `ui-slot-editor.cpp`).
- Setup tasks T001 / T002 are quick and sequential (single dev, manual).
- Polish phase is sequential by its nature (build → format → test → regression).

---

## Parallel Example: User Story 1 + User Story 3 code edits

```text
# Launch all four (a)-disposition edits in parallel (different files, no dependencies):
Task: T003 [US1] Refactor SlotManager::stop_all() in src/manager.cpp (F-M1)
Task: T004 [US1] Refactor MultiSceneRecordDock::refresh() in src/ui-dock.cpp (F-UD1)
Task: T005 [US1] Hoist obs_get_encoder_properties in src/ui-slot-editor.{hpp,cpp} (F-USE1)
Task: T006 [US3] Remove redundant running_.store(true) in src/slot.cpp (F-S1)
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Complete Phase 1: Setup.
2. (Phase 2 is empty.)
3. Complete Phase 3: US1 — three independent edits (T003, T004, T005).
4. Build + run quickstart Tests 2, 3, 4.
5. **STOP and VALIDATE**: dock action latency target met under 10-slot workload; editor open ≤200 ms; Stop All does not freeze the UI. This alone is shippable as a feature 005-partial release, since US1 is the largest single user-facing win.

### Incremental Delivery

1. Complete US1 → demo snappy UI under load.
2. Add T006 (US3) → run quickstart Test 1 → confirm no regression in lifecycle.
3. Add T007, T008 (US3 + US2 verification) → confirm idle CPU + (b) dispositions hold.
4. Add T009, T010, T011 (US4 + US5 verification) → close the audit doc.
5. Polish phase (T012–T015) → ship.

### Solo-developer strategy (the typical case for this project)

Sequential execution in priority order: T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015. The [P] markers indicate which tasks COULD parallelise if a second developer joined; with one developer the order is the natural priority sequence above.

---

## Notes

- [P] tasks = different files, no dependencies.
- [Story] label maps tasks to spec.md user stories for traceability.
- Each (a)-disposition finding from `research.md` corresponds to exactly one implementation task (T003 = F-M1, T004 = F-UD1, T005 = F-USE1, T006 = F-S1).
- Each (b)-disposition finding from `research.md` corresponds to NO implementation task; T007 verifies they remain (b) after the audit's edits.
- Commit after each task or logical group. Per Constitution Development Workflow: "Each logical change is its own commit."
- Stop at the Phase-3 checkpoint to validate US1 independently if shipping as MVP.
- Avoid: touching files outside the ten in scope (out-of-scope per spec); introducing any direct libobs / `obs-frontend-api` call from widget code (Constitution Principle IV); changing any lock-order behavior (Constitution Principle III).
