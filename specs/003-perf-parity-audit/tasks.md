---

description: "Task list for the Performance parity with OBS native recording feature"
---

# Tasks: Performance parity with OBS native recording

**Input**: Design documents from `specs/003-perf-parity-audit/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [quickstart.md](./quickstart.md)

**Tests**: This project has no automated test harness for plugin behavior — verification is manual via the OBS GUI and an FPS-overlay tool. Test tasks reference the manual procedures in `quickstart.md` rather than code under `tests/`. No TDD tasks are generated.

**Organization**: The audit (US3) is the substantive deliverable — `research.md` already exists and catalogues all 11 architectural differences with dispositions. The remaining code work is three one-line edits in `SharedEncoder::build` (US1) and a 4-hour live memory test (US2). Phases 3 and 4 verify behavior against the patched build.

**Note on scope shift**: the original audit identified the per-group `obs_view_t + video_t` (D1) as the dominant FPS cost. After user direction (2026-05-19) that per-slot scene independence is non-negotiable, D1 was reclassified from CLOSE to ACCEPT (irreducible, FR-009 (b)) — D1 IS the mechanism that delivers per-slot independence; closing it would tie slots to OBS's program output and silently capture program-content in slots configured for other scenes. The remaining closeable items (D2/D3/D4) are smaller — hardcoded video-info parameters in the per-slot pipeline.

## Format: `[ID] [P?] [Story?] Description`

- **[P]**: Can run in parallel
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Each task includes the exact file path and (where relevant) the current line range it edits

---

## Phase 1: Setup

**Purpose**: Confirm working tree and branch.

- [X] T001 Verify the current branch is `003-perf-parity-audit` and that `src/slot.cpp` is tracked. No code change in this task — read-only environment check.

---

## Phase 2: Foundational (Closeable Audit Items)

**Purpose**: Apply the three one-line edits that close D2 / D3 / D4 in `SharedEncoder::build`. These are the only code changes for this feature; everything else is verification.

**⚠️ CRITICAL**: All three edits live in the same function and the same struct; they MUST be made in a single editing pass to avoid intermediate states where some fields are aligned and others aren't.

- [X] T002 [US1] In `src/slot.cpp` `SharedEncoder::build` at the `obs_video_info ovi = {};` block (currently lines 269-283), align three fields to `main_ovi` (which is already populated from `obs_get_video_info` at line 278):
  - Change `ovi.output_format = VIDEO_FORMAT_NV12;` (line 276) → `ovi.output_format = main_ovi.output_format;` — closes D2.
  - Change `ovi.gpu_conversion = true;` (line 281) → `ovi.gpu_conversion = main_ovi.gpu_conversion;` — closes D4.
  - Change `ovi.scale_type = OBS_SCALE_BICUBIC;` (line 282) → `ovi.scale_type = main_ovi.scale_type;` — closes D3.

  Note: `main_ovi.colorspace` and `main_ovi.range` are already used (lines 279-280). After T002, the per-group `ovi` is fully aligned with `main_ovi` except for the dimensions/fps fields (which legitimately differ per slot config) and the format/scale/gpu_conversion fields (which now match main). No other lines in `SharedEncoder::build` need changing — the per-group `obs_view_create` / `obs_view_add2` path is the irreducible cost of per-slot independence and stays.

- [X] T003 Build the plugin against OBS Studio 31.1.1+ on the local platform. Confirm zero compilation errors and zero new warnings in `src/slot.cpp`, and that the plugin loads in OBS without missing-symbol errors. Depends on T002.

**Checkpoint**: The closeable audit items are closed. Phases 3, 4, 5 verify behavior against the patched build.

---

## Phase 3: User Story 1 — No avoidable plugin-specific overhead (Priority: P1)

**Goal**: confirm the per-slot pipeline now imposes no avoidable extra conversions beyond what `obs_get_video_info` reports for OBS main. Measure the residual `delta` (plugin overhead beyond OBS native) as institutional memory.

**Independent Test**: Quickstart Tests 1–4 (overhead measurement across encoder families) and Test 5 (non-program-scene per-group path still correct).

- [ ] T004 [P] [US1] Execute quickstart Test 1 (x264 overhead measurement, 5 runs of each baseline/OBS-native/plugin condition, median delta recorded) from `specs/003-perf-parity-audit/quickstart.md`. Record the delta as institutional memory; no pass/fail threshold. Depends on T003.

- [ ] T005 [P] [US1] Execute quickstart Test 2 (NVENC overhead measurement). Skip if no NVIDIA GPU. Depends on T003.

- [ ] T006 [P] [US1] Execute quickstart Test 3 (AMF overhead measurement). Skip if no AMD GPU. Depends on T003.

- [ ] T007 [P] [US1] Execute quickstart Test 4 (QSV overhead measurement). Skip if no Intel encoder. Depends on T003.

- [ ] T008 [P] [US1] Execute quickstart Test 5 (non-program-scene recording — slot captures its configured scene throughout, including during a mid-recording OBS program switch). This is the FR-009 (b) verification: the per-slot scene independence guarantee holds. Depends on T003.

**Checkpoint**: User Story 1 verified — the closeable items are closed; the delta is documented; per-slot independence still works.

---

## Phase 4: User Story 2 — Memory stability (Priority: P2)

**Goal**: confirm FR-004 / FR-005 / FR-006 / FR-007. The code change in this feature does not touch memory lifecycle, but the live tests are how we know the existing lifecycle was already correct (per the by-inspection audit in research.md).

- [ ] T009 [US2] Execute quickstart Test 6 (4-hour memory stability — single slot recording, ≤50 MB growth above 5-minute plateau, ≤30 MB post-stop delta from baseline) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [ ] T010 [US2] Execute quickstart Test 7 (50 consecutive start/stop cycles, no per-cycle leak) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [ ] T011 [US2] Execute quickstart Test 8 (two scene-collection round-trips, ≤20 MB growth from baseline) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

**Checkpoint**: User Story 2 verified — memory is stable across the documented workloads.

---

## Phase 5: User Story 3 — Audit document (Priority: P3)

**Goal**: confirm the audit deliverable (research.md) is complete, every architectural difference has a documented disposition per FR-009, and the document is suitable for future regression triage.

- [ ] T012 [US3] Read through `specs/003-perf-parity-audit/research.md` and confirm: (a) it enumerates the recording-pipeline surface from FR-008 (scene → view/canvas → video output → video encoder → muxer; audio mix path; encoder settings application; output start/stop lifecycle; signal handling); (b) each of D1–D11 has a disposition (CLOSE / ACCEPT / KEEP) and a cited rationale; (c) the user direction that established D1's irreducible disposition is captured verbatim or paraphrased clearly. No code change in this task — review only.

**Checkpoint**: User Story 3 verified — the audit document is the institutional-memory artifact.

---

## Phase 6: Polish & Cross-Cutting Concerns

- [ ] T013 [P] Execute quickstart Test 9 (replay-buffer variant — same delta measurement; replay clip save still works) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [ ] T014 [P] Execute quickstart Test 10 (sharing-slot variant — combined two-slot delta close to single-slot delta) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [ ] T015 [P] Execute quickstart Test 11 (regression checks — features 001 and 002 quickstart procedures all still pass) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [ ] T016 [P] Execute quickstart Test 12 (crash check — no plugin crashes attributable to recording-pipeline changes across the full test suite) from `specs/003-perf-parity-audit/quickstart.md`. Depends on T003.

- [X] T017 Apply the project's clang-format gate to `src/slot.cpp` (the only file touched). Depends on T002.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — read-only environment check.
- **Foundational (Phase 2)**: T002 stands alone. T003 (build) depends on T002.
- **Phases 3, 4, 5 (User Stories)**: All depend on T003 (Phase 3 + Phase 4); Phase 5 (T012) depends only on the spec/research being in place — can technically run any time, but listed after the code work for ordering clarity.
- **Phase 6 (Polish)**: T013–T016 depend on T003. T017 depends only on T002.

### User Story Dependencies

- **US1 (P1)**: depends on T003. Verification tasks T004–T008 are all independent of each other.
- **US2 (P2)**: depends on T003. T009 is the long-running one (4 hours); T010 and T011 are short and can interleave between US1 tests.
- **US3 (P3)**: depends only on the audit document existing. Independent of code changes.

### Parallel Opportunities

- All US1 verification tasks (T004–T008) are mutually parallel — they're independent benchmark runs.
- US1 and US2 verification phases can run in parallel against the same build (different testers / sessions).
- Polish tasks T013–T016 are mutually parallel.

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Complete Phase 1: Setup.
2. Complete Phase 2: Foundational (T002 + T003). Three one-line edits.
3. Complete Phase 3: User Story 1 verification (T004–T008). Records the delta as institutional memory.
4. **STOP and VALIDATE**: the closeable audit items are closed; the per-slot independence guarantee still works; we have a baseline FPS overhead number to defend against future regressions.

### Incremental Delivery

The code change is small and atomic; "incremental" here is about verification depth. After the MVP:
- Add US2 (memory stability) — confirms no leak.
- Add US3 (audit document) — confirms the institutional-memory deliverable is in shape.
- Add Polish — broader edge cases and regression checks.

### Parallel Team Strategy

For a multi-tester pass: one tester drives T002 + T003. Once T003 is green, three testers can split (US1 / US2 / Polish) since the verifications are independent. T009 (4-hour memory test) should be started as early as possible since it's the long pole.

---

## Notes

- The audit (US3) is the institutional-memory deliverable. Future changes to the recording pipeline should be cross-checked against research.md to flag any reintroduction of closed gaps OR any change to the per-slot independence guarantee.
- Quickstart Tests 1–4, 9, 10 record `delta` numbers — these are forward-looking artifacts, not pass/fail. If a future feature pushes any of these deltas up materially, that's a regression signal.
- The 4-hour memory test (T009) is the longest individual task. Schedule it for an unattended overnight or background run.
- T017 (clang-format) is small but easy to forget — make sure to run it before committing.
