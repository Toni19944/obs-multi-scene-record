# Tasks: Remove Dead obs_output_set_mixers Calls

**Input**: Design documents from `specs/009-remove-dead-mixer-call/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, quickstart.md

**Tests**: No automated test tasks — verification is manual runtime testing per quickstart.md.

**Organization**: Tasks are grouped by user story. Both user stories share a single code change (the two-line deletion) but have distinct verification criteria.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: Confirm preconditions before making the change

- [x] T001 Verify exactly two `obs_output_set_mixers` call sites exist in plugin source via grep across all files under `src/`
- [x] T002 Read `src/slot.cpp` and confirm both calls are inside `SceneSlot::setup_outputs()`, each immediately after an `obs_output_set_audio_encoder` loop

**Checkpoint**: Preconditions verified — the change is safe to apply

---

## Phase 2: User Story 1 — Clean Slot-Start Logs (Priority: P1) + User Story 2 — Audio Track Routing Preserved (Priority: P1)

**Goal**: Remove both dead `obs_output_set_mixers` calls so encoded outputs no longer trigger OBS warnings, while preserving correct audio track routing.

**Independent Test (US1)**: Start any slot and confirm the OBS log contains no `Tried to use obs_output_set_mixers on an encoded output` warning.

**Independent Test (US2)**: Record with non-contiguous track selection (e.g., tracks 1 and 3), inspect output file, and confirm correct audio streams are present.

### Implementation

- [x] T003 [US1] Delete `obs_output_set_mixers(rec_out_, cfg_.audio_tracks);` (line 983) from recording output setup in `src/slot.cpp`
- [x] T004 [US1] Delete `obs_output_set_mixers(replay_out_, cfg_.audio_tracks);` (line 1072) from replay buffer setup in `src/slot.cpp`
- [x] T005 Verify no remaining `obs_output_set_mixers` call sites exist anywhere in `src/` after deletion

**Checkpoint**: Both user stories satisfied by the same two-line deletion — code change complete

---

## Phase 3: Polish & Cross-Cutting Concerns

**Purpose**: Documentation and final validation

- [x] T006 Add CHANGELOG.md entry documenting the fix: removed dead `obs_output_set_mixers` calls that produced OBS warnings on every slot start
- [x] T007 Build the plugin and confirm successful compilation with no new warnings
- [ ] T008 Run quickstart.md validation: start a slot with multi-track audio and replay enabled, confirm no warning in log and correct audio tracks in output files

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — starts immediately
- **User Stories (Phase 2)**: Depends on Setup (T001, T002) confirming preconditions
- **Polish (Phase 3)**: Depends on Phase 2 completion

### Task Dependencies

- T001, T002 → T003, T004 (confirm before deleting)
- T003, T004 → T005 (verify after deleting)
- T005 → T006, T007, T008 (polish after code change verified)
- T007, T008 can run in parallel

### User Story Dependencies

- **US1 and US2** are satisfied by the same code change (T003 + T004) — no cross-story dependency issue
- Both stories share P1 priority and are delivered as a single atomic unit

---

## Implementation Strategy

### MVP (Single Atomic Change)

1. Complete Phase 1: Verify preconditions (T001–T002)
2. Complete Phase 2: Delete both lines (T003–T005)
3. Complete Phase 3: CHANGELOG + build + runtime verification (T006–T008)
4. **DONE**: Both user stories delivered

This feature is a single atomic change — there is no incremental delivery path because the change is two line deletions in the same function.

---

## Notes

- Line numbers (983, 1072) are based on the current state of `src/slot.cpp` and may shift if prior commits land first — match by content, not line number
- The `obs_output_set_audio_encoder` loops immediately above each deleted line MUST be preserved — they are the correct audio routing mechanism
- No header, build-system, or other source file changes are needed (FR-004)
