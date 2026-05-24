---

description: "Task list for the Fix Hotkey Registration feature"
---

# Tasks: Fix Hotkey Registration

**Input**: Design documents from `specs/001-fix-hotkey-registration/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [data-model.md](./data-model.md), [contracts/libobs-hotkey-api.md](./contracts/libobs-hotkey-api.md), [quickstart.md](./quickstart.md)

**Tests**: This project has no automated test harness for plugin behavior — verification is manual via the OBS GUI. Test tasks therefore reference the manual procedures in `quickstart.md` rather than code under `tests/`. No TDD tasks are generated.

**Organization**: This is a mechanism-level bug fix; the implementation is a single atomic change shared by all four user stories. The change lives in Phase 2 (Foundational); Phases 3–6 are per-story manual verification phases against the patched build. There is no per-story implementation slice because the underlying code path is identical for every story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: Which user story this task belongs to (US1, US2, US3, US4)
- Each task includes the exact file path and (where relevant) the current line range it edits

---

## Phase 1: Setup

**Purpose**: Confirm the working tree and toolchain are ready.

- [ ] T001 Verify the current branch is `001-fix-hotkey-registration` (the branch the spec/plan target) and that the unstaged-and-untracked files reported in `git status` at session start (the existing `src/slot.{cpp,hpp}`, `src/manager.{cpp,hpp}`, `src/plugin-main.{cpp,hpp}`, and the modifications to `CMakeLists.txt`, `buildspec.json`, `data/locale/en-US.ini`, `src/plugin-support.c.in`, `src/plugin-support.h`) match the change scope in `specs/001-fix-hotkey-registration/plan.md`. No code change in this task — read-only environment check.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Apply the single mechanism-level change that satisfies all four user stories. Every subsequent user-story phase verifies behavior against the build produced here.

**⚠️ CRITICAL**: No user story verification can begin until this phase is complete.

- [ ] T002 [P] Add `obs_output_t* hotkey_out_ = nullptr;` to the private members of `SceneSlot` in `src/slot.hpp`, immediately below the existing `obs_hotkey_id hotkey_record_` / `hotkey_replay_` declarations (currently at `src/slot.hpp:277-278`). Include a brief one-line comment that this is the inert sentinel output used solely as the Settings > Hotkeys group anchor (per `specs/001-fix-hotkey-registration/data-model.md` § "Sentinel output (new)"). Do not alter any other field.

- [ ] T003 [P] Remove the `obs_source_t* hotkey_group_source_` field and the `obs_source_t* hotkey_group_source()` member-function declaration from `src/manager.hpp` (the function declaration block at `src/manager.hpp:99-109` and the field at `src/manager.hpp:141-144`, including their explanatory comments — they document a mechanism that is being deleted).

- [ ] T004 [P] Remove the `SlotManager::hotkey_group_source()` implementation from `src/manager.cpp` (the lazy-create helper at `src/manager.cpp:158-184`) and the cleanup block inside `SlotManager::shutdown()` that releases `hotkey_group_source_` (at `src/manager.cpp:23-31`). Adjust surrounding whitespace/comments so the file reads cleanly with the helper gone.

- [ ] T005 [US1] [US2] [US4] Rewrite `SceneSlot::register_hotkeys()` in `src/slot.cpp` (currently at `src/slot.cpp:797-839`) per `specs/001-fix-hotkey-registration/data-model.md` § "register_hotkeys()": create `hotkey_out_` via `obs_output_create("ffmpeg_muxer", ("Multi-Scene Record: " + cfg_.name).c_str(), nullptr, nullptr)`; if successful, register both `hotkey_record_` and `hotkey_replay_` against it via `obs_hotkey_register_output(...)` using the existing `multi_scene_rec.record.<id>` / `multi_scene_rec.save_replay.<id>` names and the existing `"Toggle Recording: <slot name>"` / `"Save Replay: <slot name>"` descriptions; if `obs_output_create` returns null, fall back to `obs_hotkey_register_frontend(...)` for each hotkey and log a `LOG_WARNING`. Retain the existing `pending_hk_record_` / `pending_hk_replay_` → `obs_hotkey_load` restore path verbatim — bindings restoration is mechanism-agnostic and must keep working. Depends on T002.

- [ ] T006 [US1] [US2] [US4] Update `SceneSlot::unregister_hotkeys()` in `src/slot.cpp` (currently at `src/slot.cpp:897-907`) to release `hotkey_out_` after unregistering both hotkey ids. Order must be: `obs_hotkey_unregister(hotkey_record_)` → set `hotkey_record_ = OBS_INVALID_HOTKEY_ID` → `obs_hotkey_unregister(hotkey_replay_)` → set `hotkey_replay_ = OBS_INVALID_HOTKEY_ID` → `if (hotkey_out_) { obs_output_release(hotkey_out_); hotkey_out_ = nullptr; }`. Both hotkey IDs must be unregistered before the strong ref is dropped (the registered hotkeys hold a weak ref to `hotkey_out_`; see `contracts/libobs-hotkey-api.md` behavioral guarantee #2). Depends on T002, T005.

- [ ] T007 [US3] Review `SceneSlot::update_config()` in `src/slot.cpp` (currently at `src/slot.cpp:387-422`) and confirm the existing `capture_hotkey_bindings()` → `unregister_hotkeys()` → `register_hotkeys()` cycle at `:411-415` now also destroys and recreates `hotkey_out_` with the renamed name (it does, as long as T005 and T006 are correct — `unregister_hotkeys` releases the old output, `register_hotkeys` creates a new one from `cfg_.name`). No code change should be required here; if the cycle requires any adjustment to deliver FR-010's label refresh on rename, make it in this task and document why. Depends on T005, T006.

- [ ] T008 Review `SceneSlot::~SceneSlot()` in `src/slot.cpp` (currently at `src/slot.cpp:377-385`) and confirm it remains correct after T005/T006: the destructor's `unregister_hotkeys()` call must release `hotkey_out_` exactly once and never double-release. No code change required if the new `unregister_hotkeys` correctly nulls `hotkey_out_` after release. Depends on T006.

- [ ] T009 Build the plugin against OBS Studio 31.1.1+ on the local platform (Windows: MSVC 17 2022; macOS: Xcode 16; Ubuntu 24.04: gcc with ninja). Confirm zero compilation errors and zero new compiler warnings in `src/slot.{hpp,cpp}` and `src/manager.{hpp,cpp}`. Confirm the plugin loads in OBS without missing-symbol errors at startup. Depends on T002–T008.

**Checkpoint**: The fix is in place and the plugin loads. Per-story verification can now begin (Phases 3–6 may run in parallel).

---

## Phase 3: User Story 1 — Toggle Recording (Priority: P1) 🎯 MVP

**Goal**: A single bound hotkey starts the slot when idle and stops it when running, with no dock interaction. Per-slot isolation: pressing Slot A's hotkey never affects Slot B.

**Independent Test**: Quickstart Test 1 (single-slot toggle) and Test 2 (per-slot isolation) — both in `specs/001-fix-hotkey-registration/quickstart.md`.

- [ ] T010 [P] [US1] Execute quickstart Test 1 (single-slot toggle: start → stop via hotkey only) from `specs/001-fix-hotkey-registration/quickstart.md`. Record pass/fail and any deviation. Depends on T009.

- [ ] T011 [P] [US1] Execute quickstart Test 2 (per-slot isolation with Slot A and Slot B bound to different keys) from `specs/001-fix-hotkey-registration/quickstart.md`. Record pass/fail. Depends on T009.

**Checkpoint**: User Story 1 verified.

---

## Phase 4: User Story 2 — Save Replay (Priority: P1)

**Goal**: A single bound hotkey saves a replay clip when the replay buffer is active; the same hotkey is a silent no-op when replay is disabled or the slot is stopped.

**Independent Test**: Quickstart Test 3 (clip is written) and Test 4 (silent no-op when replay disabled).

- [ ] T012 [P] [US2] Execute quickstart Test 3 (Save Replay writes a clip when buffer is active) from `specs/001-fix-hotkey-registration/quickstart.md`. Record pass/fail. Depends on T009.

- [ ] T013 [P] [US2] Execute quickstart Test 4 (Save Replay is a silent no-op when replay is disabled in config) from `specs/001-fix-hotkey-registration/quickstart.md`. Confirm no error dialog, no crash, no `LOG_ERROR`. Depends on T009.

**Checkpoint**: User Story 2 verified.

---

## Phase 5: User Story 3 — Persistence across restart / rename / scene-collection switch (Priority: P2)

**Goal**: Per-slot hotkey bindings survive OBS restart, slot rename (with the Settings group label updating to the new name), and scene-collection round-trips.

**Independent Test**: Quickstart Tests 5, 6, 7.

- [ ] T014 [P] [US3] Execute quickstart Test 5 (bindings survive OBS restart) from `specs/001-fix-hotkey-registration/quickstart.md`. Confirm SC-002 (100% binding retention). Depends on T009.

- [ ] T015 [P] [US3] Execute quickstart Test 6 (slot rename: Settings group label updates AND binding is preserved) from `specs/001-fix-hotkey-registration/quickstart.md`. Confirm SC-003 (label reflects new name within one Settings open/close). Depends on T009.

- [ ] T016 [P] [US3] Execute quickstart Test 7 (scene-collection switch away and back: bindings restored) from `specs/001-fix-hotkey-registration/quickstart.md`. Depends on T009.

**Checkpoint**: User Story 3 verified.

---

## Phase 6: User Story 4 — Grouped Settings layout (Priority: P3)

**Goal**: With multiple slots configured, OBS Settings > Hotkeys shows each slot's two hotkeys grouped under a distinct `Multi-Scene Record: <slot name>` section, not interleaved with Front-End entries.

**Independent Test**: Quickstart Test 8.

- [ ] T017 [US4] Execute quickstart Test 8 (three slots → three distinct `Multi-Scene Record: <slot name>` groups, each with exactly two rows, not interleaved with unrelated entries) from `specs/001-fix-hotkey-registration/quickstart.md`. Depends on T009.

**Checkpoint**: User Story 4 verified.

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Edge cases, regression checks, formatting.

- [ ] T018 [P] Execute quickstart Test 9 (add a slot at runtime → its group appears in Settings without OBS restart) from `specs/001-fix-hotkey-registration/quickstart.md`. Depends on T009.

- [ ] T019 [P] Execute quickstart Test 10 (remove a slot at runtime → its group disappears, previously bound keys are inert) from `specs/001-fix-hotkey-registration/quickstart.md`. Depends on T009.

- [ ] T020 [P] Execute quickstart Test 11 (encoder-failure parity: hotkey-initiated start fails the same way a dock-initiated start fails; no crash) from `specs/001-fix-hotkey-registration/quickstart.md`. Depends on T009.

- [ ] T021 Execute quickstart Test 12 (full slot lifecycle stress: add → rename → start → stop → enable replay → save replay → stop → switch scene collection → restart OBS → remove) from `specs/001-fix-hotkey-registration/quickstart.md`. Confirm SC-006 (zero crashes attributable to hotkey registration across the full lifecycle). Depends on T009.

- [ ] T022 [P] Execute the Regression checks at the bottom of `specs/001-fix-hotkey-registration/quickstart.md`: per-slot recording paths still write to configured directories; dock start/stop buttons still work; `[CBR fallback]` indicator still appears on x264 fallback; shared-encoder slots (one slot referencing another's encoder via `shared_encoder_slot_id`) still record correctly. Depends on T009.

- [ ] T023 Apply the project's formatting gate locally (the same `check-format` workflow that runs in CI — typically `clang-format` for `src/slot.{hpp,cpp}` and `src/manager.{hpp,cpp}`, and `gersemi` for `CMakeLists.txt` if it was touched). No CMake change is expected in this feature, but run the check to be safe. Depends on T002–T009.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — read-only environment check.
- **Foundational (Phase 2)**: T002, T003, T004 can run in parallel (different files). T005 depends on T002. T006 depends on T002 and T005 (same file as T005). T007 and T008 depend on T005 and T006. T009 (build) depends on T002–T008.
- **Phases 3–6 (User Stories)**: All depend on T009. Within a story, the verification tasks can run in parallel. Across stories, all verification phases can run in parallel (they exercise the same build from different angles).
- **Phase 7 (Polish)**: All tasks depend on T009. T018–T020, T022 can run in parallel. T021 is sequential (it's a long, stateful procedure). T023 depends on T002–T009.

### User Story Dependencies

- **US1 (P1)**: depends on T009. Independent of US2/US3/US4 at verification time.
- **US2 (P1)**: depends on T009. Independent of US1/US3/US4 at verification time.
- **US3 (P2)**: depends on T009. Independent of US1/US2/US4 at verification time.
- **US4 (P3)**: depends on T009. Independent of US1/US2/US3 at verification time.

There are no inter-story dependencies because the underlying implementation is shared; each story's verification exercises a different facet of the same code path.

### Parallel Opportunities

- Phase 2 header-only / manager-only edits (T002 [slot.hpp], T003 [manager.hpp], T004 [manager.cpp]) can be done in parallel: three different files.
- All quickstart-based verification tasks within a story phase are mutually parallelizable.
- All user-story verification phases (3, 4, 5, 6) can run in parallel once T009 completes.
- Polish tasks T018–T020 and T022 can run in parallel.

---

## Parallel Example: Phase 2 foundational edits

```text
# After T001, these three can run concurrently (different files):
T002 — edit src/slot.hpp
T003 — edit src/manager.hpp
T004 — edit src/manager.cpp

# Then T005 (slot.cpp register_hotkeys rewrite) sequentially.
# Then T006 (slot.cpp unregister_hotkeys update), same file → sequential after T005.
# Then T007 / T008 (review-only of update_config and ~SceneSlot, same file) → sequential after T006.
# Then T009 (build).
```

## Parallel Example: Story verification

```text
# After T009 completes, four developers/sessions can each take one user story:
Dev A — T010, T011 (US1)
Dev B — T012, T013 (US2)
Dev C — T014, T015, T016 (US3)
Dev D — T017       (US4)

# All run against the same patched build; no cross-talk.
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational (T002–T009). This is the only code change in the feature — once it's done, all four user stories are simultaneously fixed at the mechanism level.
3. Complete Phase 3: User Story 1 verification (T010, T011).
4. **STOP and VALIDATE**: at this point the plugin's most critical hotkey path is verified working. Ship if needed.

### Incremental Delivery

The implementation is atomic — there is no way to ship a partial fix. Once T002–T009 lands, every story benefits at once. The "incremental" axis here is verification depth: ship after US1 is verified; verify US2/US3/US4 in subsequent rounds as time allows; finish with Polish.

### Parallel Team Strategy

For a multi-developer/multi-session pass: one developer drives Phase 2 (the edits must be coordinated because most live in `src/slot.cpp`); the other developers wait for T009, then take one user-story phase each in parallel, then converge on Polish.

---

## Notes

- This feature has no automated test harness; every verification task is a manual run against a live OBS instance per `quickstart.md`. Record results (pass/fail + any deviation) when checking off each task.
- [P] tasks = different files OR independent manual procedures, with no dependency on incomplete tasks.
- The [Story] label is applied to Phase 2 tasks that are direct prerequisites for a specific story's behavior (e.g., T005 enables US1/US2/US4 because the new registration mechanism is what those stories test). It's a traceability hint, not a partitioning of work.
- Commit after Phase 2 lands (T009 green) and again after each verification phase passes.
- Do not skip T021 (full lifecycle stress) — it is the only task that exercises SC-006 across the full slot lifecycle.
