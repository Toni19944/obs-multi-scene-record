# Tasks: Apply Codebase Audit Fixes

**Input**: Design documents from `/specs/013-audit-fixes/`

**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, quickstart.md

**Tests**: Not requested — no test tasks included.

**Organization**: Tasks are grouped by user story (audit category) to enable independent implementation and testing of each category.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3, US4)

## Story ↔ Audit Category Mapping

- **US1**: Stability & Safety (S-001 through S-009) — Priority P1
- **US2**: Performance (P-001 through P-007) — Priority P2
- **US3**: Correctness (C-001 through C-005) — Priority P3
- **US4**: Code Quality (Q-001 through Q-006) — Priority P4

---

## Phase 1: Setup

**Purpose**: No project initialization needed — this is a fix-only pass on an existing codebase. Read the audit report and verify build.

- [x] T001 Read audit report at specs/012-idle-slot-resource-audit/audit-report.md and confirm all 27 findings are understood
- [x] T002 Verify clean build on primary dev platform before applying any fixes

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: S-002 (shared_ptr migration) is the single blocking prerequisite. S-003, P-001, and the signal-handler safety pattern all depend on it.

**⚠️ CRITICAL**: S-003 and P-001 cannot begin until S-002 is complete.

- [x] T003 [US1] **S-002 Step 1**: Change `slots_` type from `std::vector<std::unique_ptr<SceneSlot>>` to `std::vector<std::shared_ptr<SceneSlot>>` in src/manager.hpp and update all `make_unique` → `make_shared` call sites in src/manager.cpp
- [x] T004 [US1] **S-002 Step 2**: Add `std::enable_shared_from_this<SceneSlot>` base class to `SceneSlot` in src/slot.hpp
- [x] T005 [US1] **S-002 Step 3**: Create `HotkeyHandle` struct with `std::weak_ptr<SceneSlot>` in src/slot.hpp, allocate in `register_hotkeys()` and free in `unregister_hotkeys()` in src/slot.cpp
- [x] T006 [US1] **S-002 Step 4**: Convert `on_record_hotkey` and `on_save_hotkey` callbacks to lock `weak_ptr` from `HotkeyHandle` in src/slot.cpp
- [x] T007 [US1] **S-002 Step 5**: Convert signal handlers (`on_rec_output_stop`, `on_replay_output_stop`, `on_replay_saved`) to lock `weak_ptr` from `HotkeyHandle` in src/slot.cpp
- [x] T008 Verify clean build after S-002 migration — all callers of `slots_` compile correctly

**Checkpoint**: shared_ptr migration complete — user story phases can proceed

---

## Phase 3: User Story 1 — Stability & Safety Fixes (Priority: P1) 🎯 MVP

**Goal**: Apply remaining 8 stability/safety fixes (S-001, S-003 through S-009) plus C-001 (pulled forward as S-004 prerequisite) to eliminate crashes, data races, deadlocks, and undefined behaviour.

**Independent Test**: Each fix verified by code review against audit report, compilation, and manual testing of affected code path (shutdown, concurrent start/stop, hotkeys, replay save).

### Implementation

- [x] T009 [P] [US1] **S-001**: Connect `g_dock` to `QObject::destroyed` signal to null pointer on Qt destruction in src/plugin-main.cpp
- [x] T010 [US1] **S-003**: Change `start_all` and `stop_all` to snapshot `shared_ptr` copies instead of raw pointers, release `mtx_` before iterating in src/manager.cpp
- [x] T011 [P] [US1] **S-005**: Restructure `teardown()` to release `slot_mtx_` before `wait_for_output_stop` — three-phase approach (disconnect+stop under lock, wait without lock, release under lock) in src/slot.cpp
- [x] T012 [P] [US1] **S-006**: Snapshot `cfg_.name` and `replay_out_` under `slot_mtx_` at entry of `log_replay_saved()` in src/slot.cpp
- [x] T013 [P] [US1] **S-007**: Defer `stop()` call in `on_rec_output_stop` and `on_replay_output_stop` to UI thread via `obs_queue_task(OBS_TASK_UI, ...)` in src/slot.cpp
- [x] T014 [P] [US1] **S-008**: Replace macOS `sysctlbyname("hw.memsize")` with Mach `host_statistics64` VM stats for available (not total) memory in src/slot.cpp
- [x] T015 [P] [US1] **S-009**: Take `slot_mtx_` briefly to snapshot `cfg_` identity fields before calling into `SlotManager` in `start()` in src/slot.cpp
- [x] T025 [US3] **C-001**: Rewrite `build_output_filename` to embed 6-hex-char ID suffix and apply `sanitize_for_filename` to slot name in src/slot.cpp (also resolves S-004) *(US3 task pulled into Phase 3 because C-001 subsumes S-004)*
- [x] T016 [P] [US1] **S-004**: Verify that C-001's `build_output_filename` rewrite (T025) includes `sanitize_for_filename` — no additional code change expected

**Checkpoint**: All 9 stability/safety fixes applied — plugin safe for concurrent operations and clean shutdown

---

## Phase 4: User Story 2 — Performance Fixes (Priority: P2)

**Goal**: Apply 7 performance fixes (P-001 through P-007) to reduce lock contention, eliminate redundant syscalls, and optimize hot paths.

**Independent Test**: Each fix verified by code review, compilation, and observing reduced lock acquisitions or eliminated redundant calls.

### Implementation

- [x] T017 [US2] **P-001 Step 1**: Add `SlotSnapshot` struct (using `std::shared_ptr<SceneSlot>`) and `snapshot_slots()` method to src/manager.hpp and src/manager.cpp
- [x] T018 [US2] **P-001 Step 2**: Refactor `refresh()` and `refresh_stats()` in src/ui-dock.cpp to use `snapshot_slots()` instead of per-slot `slot_count()`/`slot_at()` calls
- [x] T019 [P] [US2] **P-002**: Reduce `slot_mtx_` scope in `stats()` — snapshot `rec_out_`, `replay_out_`, `encoder_fallback_` under lock, query OBS output functions without lock in src/slot.cpp
- [x] T020 [P] [US2] **P-003**: Cache `s->config()` reference once at top of `refresh_stats` loop body in src/ui-dock.cpp
- [x] T021 [P] [US2] **P-004**: Add `cached_avail_mb_` member to `SlotEditor` in src/ui-slot-editor.hpp, initialize in constructor, use in `on_replay_max_size_inputs_changed` in src/ui-slot-editor.cpp
- [x] T022 [P] [US2] **P-005**: Add `std::unordered_map<std::string, size_t> id_index_` to `SlotManager` in src/manager.hpp, rebuild in `load_from`/`add_slot`/`remove_slot`, use in `config_by_slot_id` and `slot_name_by_id` in src/manager.cpp
- [x] T023 [P] [US2] **P-006**: Replace manual `popcount32` loop with `__popcnt` (MSVC) / `__builtin_popcount` (GCC/Clang) intrinsics in src/slot.cpp
- [x] T024 [US2] **P-007**: Hoist single `obs_get_encoder_properties` call in `on_encoder_changed`, pass `obs_properties_t*` to `populate_rate_control_combo`, `update_rc_value_field`, and `update_encoder_specific_ui` in src/ui-slot-editor.hpp and src/ui-slot-editor.cpp

**Checkpoint**: All 7 performance fixes applied — reduced lock contention and eliminated redundant work

---

## Phase 5: User Story 3 — Correctness Fixes (Priority: P3)

**Goal**: Apply 4 remaining correctness fixes (C-002 through C-005) to validate loaded data, fix ID ambiguity, and correct platform-specific memory queries. (C-001 moved to Phase 3 as S-004 prerequisite.)

**Independent Test**: Each fix verified by code review, compilation, and testing the specific trigger condition (corrupted save, Linux memory reporting).

### Implementation

- [x] T026 [P] [US3] **C-002**: Add range clamping for `fps_num` (0–240000) and `fps_den` (0–1001) after loading in `slot_from_data` in src/manager.cpp
- [x] T027 [P] [US3] **C-003**: Add separator in `generate_slot_id` format string — change `"%llx%x"` to `"%llx-%x"` in src/slot.cpp
- [x] T028 [P] [US3] **C-004**: Replace Linux `sysinfo` `freeram` with `/proc/meminfo` `MemAvailable` (fallback to `sysinfo`) in `available_physical_mb()` in src/slot.cpp
- [x] T029 [P] [US3] **C-005**: Set `preview.replay_enabled = replay_check_->isChecked()` in `on_replay_max_size_inputs_changed` in src/ui-slot-editor.cpp

**Checkpoint**: All 4 remaining correctness fixes applied — data validated, memory queries accurate (C-001 already applied in Phase 3)

---

## Phase 6: User Story 4 — Code Quality Fixes (Priority: P4)

**Goal**: Apply 6 code quality fixes (Q-001 through Q-006) to clean up build configuration, version reporting, magic numbers, formatting, and code style.

**Independent Test**: Each fix verified by code review, compilation, and inspecting affected output (log messages, UI formatting, CMake configuration).

### Implementation

- [x] T030 [US4] **Q-001**: Delete duplicate `find_package`/`target_link_libraries` block (lines 51–63) in CMakeLists.txt
- [x] T031 [US4] **Q-002**: Add `src/plugin-main.hpp` to `target_sources` in CMakeLists.txt
- [x] T032 [US4] **Q-003**: Add `PLUGIN_VERSION` compile definition from `CMAKE_PROJECT_VERSION` in CMakeLists.txt, replace hardcoded `"1.0.0"` with `PLUGIN_VERSION` in src/plugin-main.cpp
- [x] T033 [P] [US4] **Q-004**: Replace magic numbers in `wait_for_output_stop` with named constants `kStopTimeoutMs` and `kStopPollMs` in src/slot.cpp
- [x] T034 [P] [US4] **Q-005**: Update `fmt_bytes_rate` in src/ui-dock.cpp to add 1-decimal tier for 10–99 kbps range, eliminating visual jump at kbps→Mbps boundary
- [x] T035 [P] [US4] **Q-006**: Wrap `static` helper functions in anonymous namespace and remove `static` keyword in src/slot.cpp (functions: `generate_slot_id`, `fetch_scene_source`, `build_output_filename`, `set_quality_value`, `apply_family_presets`, `apply_encoder_settings`)

**Checkpoint**: All 6 code quality fixes applied — clean build config, accurate version logging, consistent style

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Final verification across all 27 fixes

- [x] T036 Full clean build on primary dev platform after all fixes
- [ ] T037 Run quickstart.md verification sequence: smoke test, recording test, replay test, hotkey test, shutdown test, concurrent test
- [x] T038 Review all fixes against audit report — confirm each finding is addressed and any deviations are documented
- [x] T039 [P] Update CHANGELOG.md with entries for all user-visible bug fixes applied in this feature (constitution MUST: every bug fix adds an entry before merge)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — can start immediately
- **Foundational (Phase 2)**: S-002 shared_ptr migration — BLOCKS S-003, P-001, and signal handler safety
- **US1 Stability (Phase 3)**: Depends on Phase 2 (S-002). Most fixes within are independent [P].
- **US2 Performance (Phase 4)**: Depends on Phase 2 (P-001 needs shared_ptr). Independent of US1.
- **US3 Correctness (Phase 5)**: Independent of Phases 2–4. Can start after Phase 1. C-001 (T025) moved to Phase 3 as S-004 prerequisite.
- **US4 Code Quality (Phase 6)**: Independent of Phases 2–5. Can start after Phase 1. Soft dependency: Q-001 before Q-002/Q-003 (same file, avoids editing deleted lines).
- **Polish (Phase 7)**: Depends on all phases complete.

### Hard Dependencies (within phases)

```
T003 → T004 → T005 → T006 → T007 → T008   (S-002 steps are sequential)
T008 → T010                                  (S-003 needs S-002 complete)
T008 → T017 → T018                           (P-001 needs S-002; step 2 needs step 1)
T025 → T016                                  (S-004 verification depends on C-001; both now in Phase 3)
T030 → T031, T032                            (Q-002/Q-003 should follow Q-001 deletion)
```

### Parallel Opportunities

- **Within Phase 3**: T009, T011, T012, T013, T014, T015 are all [P]; T025 → T016 is sequential (C-001 before S-004 verify)
- **Within US2 (Phase 4)**: T019, T020, T021, T022, T023 are all [P] — different files
- **Within US3 (Phase 5)**: T026, T027, T028, T029 are all [P] — different files/functions
- **Within US4 (Phase 6)**: T033, T034, T035 are all [P] — different files
- **Cross-story**: US3 and US4 can start immediately (no Phase 2 dependency); US1 and US2 can run in parallel once Phase 2 completes

---

## Parallel Example: Stability Fixes (Phase 3)

```text
# After S-002 (Phase 2) completes, launch all independent stability fixes:
T009: S-001 — g_dock destroyed signal          (src/plugin-main.cpp)
T011: S-005 — teardown lock restructure         (src/slot.cpp — teardown)
T012: S-006 — log_replay_saved snapshot         (src/slot.cpp — log_replay_saved)
T013: S-007 — deferred stop via obs_queue_task  (src/slot.cpp — signal handlers)
T014: S-008 — macOS available memory            (src/slot.cpp — available_physical_mb)
T015: S-009 — start() pre-lock snapshot         (src/slot.cpp — start)
```

Note: T011–T015 touch different functions within src/slot.cpp and can be applied in any order without merge conflicts.

---

## Implementation Strategy

### MVP First (Phase 2 + Phase 3 — Stability)

1. Complete Phase 1: Read audit report, verify build
2. Complete Phase 2: S-002 shared_ptr migration (CRITICAL — blocks multiple fixes)
3. Complete Phase 3: Remaining stability fixes
4. **STOP and VALIDATE**: Clean build + shutdown test + concurrent start/stop test
5. This alone addresses the most dangerous class of issues

### Incremental Delivery

1. Phase 2 (S-002) → Foundation ready
2. Phase 3 (S-001, S-003–S-009) → Stability safe → Manual test
3. Phase 4 (P-001–P-007) → Performance improved → Manual test
4. Phase 5 (C-001–C-005) → Correctness fixed → Manual test
5. Phase 6 (Q-001–Q-006) → Quality polished → Manual test
6. Phase 7 → Full verification pass

### File-Grouped Alternative

For minimal context-switching, fixes can be applied file-by-file instead of by category:

| File | Tasks (in dependency-safe order) |
|------|----------------------------------|
| `CMakeLists.txt` | T030, T031, T032 |
| `src/plugin-main.cpp` | T009, T032 (consumer) |
| `src/slot.hpp` | T004, T005 (header), T035 |
| `src/manager.hpp` | T003, T017 (header), T022 (header) |
| `src/manager.cpp` | T003, T010, T017, T022, T026 |
| `src/slot.cpp` | T005–T007, T011–T016, T019, T023, T025, T027, T028, T033, T035 |
| `src/ui-dock.cpp` | T018, T020, T034 |
| `src/ui-slot-editor.hpp` | T021 (header), T024 (header) |
| `src/ui-slot-editor.cpp` | T021, T024, T029 |

---

## Notes

- [P] tasks = different files or non-overlapping functions, no dependencies
- [Story] label maps task to audit category for traceability (US1=Stability, US2=Performance, US3=Correctness, US4=Quality)
- Each audit finding preserves its original ID (S-001, P-003, etc.) for cross-reference to audit-report.md
- Commit after each task or logical group
- S-002 is the single most impactful prerequisite — apply first
- C-001 subsumes S-004 — when C-001 is applied, S-004 becomes verification only
