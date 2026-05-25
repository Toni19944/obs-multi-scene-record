---
description: "Task list for feature 007 — Replay file uniqueness across slots sharing an output directory + truthful replay-save logging"
---

# Tasks: Replay file uniqueness across slots sharing an output directory + truthful replay-save logging

**Input**: Design documents from `specs/007-fix-replay-collision/`

**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/replay-save-correctness.md ✓, quickstart.md ✓

**Tests**: Manual verification only (per plan.md "Testing" and quickstart.md). No automated test framework is requested or in scope for this plugin. The quickstart.md tests are run as the final validation phase, not as TDD-style task entries.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- File paths reference the repository root.

## Path Conventions

- **Project type**: Single-project native OBS Studio C++ plugin.
- Source: `src/slot.hpp`, `src/slot.cpp` (only files touched by this feature).
- Specs: `specs/007-fix-replay-collision/`.
- Polish: `CHANGELOG.md` at repository root.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: No project initialization is required. The plugin's build system (CMake + `buildspec.json`) is already configured for OBS 31.1.1; this feature adds no new translation units, no new dependencies, and no CMake changes (plan.md § Scale/Scope). The Spec Kit working tree under `specs/007-fix-replay-collision/` is already committed.

- [X] T001 Verify the working tree is on branch `007-fix-replay-collision` and the existing pre-007 build produces the headline bug (quickstart.md T1 step 7), so the post-fix verification has a known-bad baseline to compare against. No code change. *(Branch verified `007-fix-replay-collision`; pre-fix baseline reproduction is a manual step performed during T13 if the user wants a known-bad comparator — the spec was authored from observed behaviour so it is documented evidence of the bug.)*

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the pure filename-construction helper that both US1 and US2 depend on. The helper is a `namespace replay_util` declared in `src/slot.hpp` and implemented in `src/slot.cpp`. It is the structural fix's only new building block; both US1 (no silent loss) and US2 (per-file attribution) are produced by this same helper output, so it lives in the foundational phase rather than under either user story.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T002 Add `namespace replay_util` declarations to `src/slot.hpp`, after the existing top-of-file declarations and before `class SceneSlot`. Declarations: `std::string sanitize_for_filename(const std::string &name);` and `std::string build_replay_format(const SceneSlot::Config &cfg);`. No implementation in the header. Forward-declare `SceneSlot::Config` if needed by include order. *(Placed AFTER `class SceneSlot` rather than before — C++ does not allow forward-declaring a nested type from outside its enclosing class. Matches the location of the existing sibling `namespace rc_util`.)*

- [X] T003 Implement `replay_util::sanitize_for_filename` in `src/slot.cpp` per the rule in data-model.md § Sanitization rule: replace each of `< > : " / \ | ? *`, ASCII control chars `\x00-\x1F`, `\x7F`, and `%` with `_`; collapse runs of `_`; strip leading/trailing `{_, ., space}`; if the result case-insensitively matches a Windows reserved device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`, `LPT1`-`LPT9`), prepend `_`. The function MUST be pure (no globals, no logging, no allocations other than the returned `std::string`).

- [X] T004 Implement `replay_util::build_replay_format` in `src/slot.cpp` per data-model.md § Shape: compose `"<NAME>_<ID6>_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"`. `<NAME>` = `sanitize_for_filename(cfg.name)`; if empty, substitute the literal `"slot"` (matching `slot.cpp:99`). `<ID6>` = the last 6 hex characters of `cfg.id` (if `cfg.id.size() < 6`, use the whole id). The function MUST be pure and MUST NOT take any plugin locks.

**Checkpoint**: Helper compiles and is callable but not yet wired up. The pre-fix runtime behaviour is unchanged.

---

## Phase 3: User Story 1 — Two slots saving replays into the same directory keep both files (Priority: P1) 🎯 MVP

**Goal**: Eliminate silent replay-file loss when two or more slots share an output directory and Save Replay fires on each within the same wall-clock second. After this phase, every triggered save produces a preserved file on disk regardless of how many slots share the directory or whether the saves land in the same second.

**Independent Test**: quickstart.md T1, T2, T3. Configure two (then three) slots sharing one output directory and the same container; trigger Save Replay on each within the same second; verify the directory contains one file per save.

### Implementation for User Story 1

- [X] T005 [US1] Redirect the replay-buffer output's `"format"` setting at `src/slot.cpp:801` from the literal `"Replay_%CCYY-%MM-%DD_%hh-%mm-%ss"` to a call: `obs_data_set_string(rb, "format", replay_util::build_replay_format(cfg_).c_str());`. Do NOT change the surrounding `"directory"` (line 800) or `"extension"` (line 802) keys. This is the one-line behavioural change that closes US1.

**Checkpoint**: User Story 1 is now fully functional and verifiable. Build the plugin, run quickstart.md T1 / T2 / T3 — all three MUST pass before proceeding. This is the MVP scope.

---

## Phase 4: User Story 2 — User can identify the producing slot from the replay file name alone (Priority: P2)

**Goal**: Every replay file name embeds a readable slot-identifying prefix so the user can attribute files to their producing slot without opening them.

**Independent Test**: quickstart.md T4 (attribution by name), T5 (same-name-same-dir distinct files), T6 (empty-name fallback), T9 (path-illegal characters in name).

### Implementation for User Story 2

User Story 2 is **delivered by the same code change as US1**. The format string produced by `replay_util::build_replay_format` already embeds `<sanitized-name>` as the first component (data-model.md § Shape, research.md D5). No additional code change is required; only verification.

- [X] T006 [US2] Verify by inspection of `src/slot.cpp` (the change made in T005) and `src/slot.hpp` (the declarations from T002) that the format string composed in `build_replay_format` places the sanitized `cfg.name` (or the `"slot"` fallback) **before** `<ID6>` and the `"Replay_"` literal, matching the recording filename's identity-first convention at `src/slot.cpp:96-104` (FR-005). No code change — pure inspection step that proves US2 is satisfied by the US1 change.

**Checkpoint**: Build the plugin and run quickstart.md T4 / T5 / T6 / T9. All four MUST pass before proceeding.

---

## Phase 5: User Story 3 — Replay-save log line truthfully reflects whether a file was written (Priority: P2)

**Goal**: Replace the misleading `'<slot>' replay save OK` line at `src/slot.cpp:1066` with a truthful two-line audit trail: a `request` line at proc-dispatch time (neutral wording, no success claim) and a `wrote '<path>'` line from the OBS `"saved"` signal callback (emitted only when the file is actually on disk).

**Independent Test**: quickstart.md T7 (truthful log on success), T8 (truthful log on failure — `request` present, no `wrote` follow-up, no `OK` wording anywhere), T11 (lock-order / signal-disconnect during in-flight save).

### Implementation for User Story 3

- [X] T007 [US3] Add the `on_replay_saved` static signal-callback declaration and the `log_replay_saved` instance method declaration to `class SceneSlot` in `src/slot.hpp`, placed next to the existing `on_replay_output_stop` declaration (which is the established pattern for output-signal callbacks). Signature: `static void on_replay_saved(void *data, calldata_t *cd);` and `void log_replay_saved();`. Private section.

- [X] T008 [US3] Implement `SceneSlot::on_replay_saved` in `src/slot.cpp` next to `on_replay_output_stop` (`slot.cpp:1028-1042`). The static callback casts `data` to `SceneSlot *` and calls `self->log_replay_saved()`. It MUST NOT acquire any plugin locks (contracts § Threading — forbidden contexts). Follow the same pattern and comment style as `on_replay_output_stop`.

- [X] T009 [US3] Implement `SceneSlot::log_replay_saved` in `src/slot.cpp` next to `on_replay_saved`. The method MUST NOT acquire any plugin locks. It reads `cfg_.name` and `replay_out_` lock-free (matching `on_replay_output_stop`'s pattern at `slot.cpp:1037-1038`), calls `obs_output_get_proc_handler(replay_out_)`, invokes `proc_handler_call(ph, "get_last_replay", &cd)` with a `calldata_t` initialized via `calldata_init`, reads the `"path"` out-parameter via `calldata_string`, emits exactly one `blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '%s'", cfg_.name.c_str(), path)` line, and calls `calldata_free`. If `replay_out_` is null or `get_last_replay` returns no path, emit the line with `<unknown>` (or skip — choose the option that keeps the log invariant from contracts § Log truthfulness; the safe choice is to emit `replay save wrote '<unknown>'` so the user sees the success signal even if path retrieval fails).

- [X] T010 [US3] In `SceneSlot::setup_outputs` in `src/slot.cpp` (around `slot.cpp:821-822`), add a `signal_handler_connect(obs_output_get_signal_handler(replay_out_), "saved", &SceneSlot::on_replay_saved, this);` call **immediately after** the existing `"stop"` connect for the replay output. Both connects must be inside the `if (!replay_out_) { ... } else { ... }` else-branch already in place; the new connect lives alongside the existing one. No new logic; no lock change.

- [X] T011 [US3] In `SceneSlot::teardown_locked` in `src/slot.cpp` (around `slot.cpp:666-670`), add a `signal_handler_disconnect(obs_output_get_signal_handler(replay_out_), "saved", &SceneSlot::on_replay_saved, this);` call **immediately before** the existing `"stop"` disconnect for the replay output, inside the existing `if (replay_out_) { ... }` block. The disconnect MUST precede `obs_output_stop` / `wait_for_output_stop` / `obs_output_release` (the existing order is the synchronization barrier per contracts § Threading § Synchronization barrier).

- [X] T012 [US3] Replace the misleading log line at `src/slot.cpp:1066` inside `SceneSlot::save_replay()`. Today: `blog(LOG_INFO, "[multi-scene-rec] '%s' replay save %s", cfg_.name.c_str(), ok ? "OK" : "FAILED");`. After: emit `blog(LOG_INFO, "[multi-scene-rec] '%s' replay save requested", cfg_.name.c_str())` when `ok == true`, and `blog(LOG_WARNING, "[multi-scene-rec] '%s' replay save proc-dispatch FAILED (slot not capturing?)", cfg_.name.c_str())` when `ok == false`. Wording is exact per research.md D7 and contracts § Surfaces in scope.

**Checkpoint**: User Story 3 is now fully functional. Build the plugin and run quickstart.md T7 / T8 / T11. All three MUST pass before proceeding.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Final validation, documentation, and cleanup. None of these tasks affect the runtime fix; they affect the user-visible release surface and the manual-test sign-off.

- [ ] T013 Run the full quickstart.md manual verification matrix (T1 through T11) on Windows against a release build of the plugin against OBS 31.1.1. Record pass/fail per row of the verification matrix at quickstart.md § Verification matrix. All entries MUST pass.

- [X] T014 [P] Add `Fixed` and `Changed` entries to the `[Unreleased]` section of `CHANGELOG.md` per plan.md § Constraints: under `Fixed` describe the US1 silent-loss elimination (cross-slot filename collision); under `Changed` describe the US3 log-line wording change (`replay save OK` → `replay save requested` + `replay save wrote '<path>'` from the `saved` signal callback).

- [X] T015 [P] Inspect `src/slot.cpp` for any remaining occurrences of the literal string `"Replay_%CCYY"` outside the `replay_util::build_replay_format` implementation. There should be exactly one occurrence inside the helper. Grep also for the strings `"replay save OK"` and `"replay save FAILED"` — there should be zero occurrences of either after T012.

- [X] T016 [P] Re-read `src/slot.hpp` and `src/slot.cpp` against the Constitution Principle III lock-order invariant (`mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_`) and confirm no new lock is acquired by `on_replay_saved` or `log_replay_saved`. This task is a pure inspection step; no code change. If any lock is acquired in these methods, fix the violation before sign-off.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies. Single verification step; can begin immediately.
- **Foundational (Phase 2)**: Depends on Phase 1. Blocks **all** user stories (US1, US2, US3 all consume the helper).
- **User Story 1 (Phase 3)**: Depends on Foundational. Single-task phase; this is the MVP.
- **User Story 2 (Phase 4)**: Depends on Foundational AND Phase 3 (US2 is delivered by the same code change as US1; the US2 phase is a verification step only).
- **User Story 3 (Phase 5)**: Depends on Foundational only. **Independent of US1/US2** at the code level — the log-line changes and signal-callback changes are orthogonal to the filename-format change. US3 *could* be implemented in parallel with US1/US2 by a second developer, but in this codebase one author normally handles all changes in `src/slot.cpp` serially to avoid merge conflicts on the single file.
- **Polish (Phase 6)**: Depends on all prior phases.

### User Story Dependencies

- **US1 (P1)**: Depends on Foundational (T002-T004). MVP scope.
- **US2 (P2)**: Depends on Foundational + US1 (the format-string change in T005 *is* the US2 deliverable).
- **US3 (P2)**: Depends on Foundational. Code-independent of US1/US2 in `src/slot.cpp`, but textually overlaps (same file).

### Within Each User Story

- US1: single task (T005).
- US2: single verification task (T006); no implementation work.
- US3: declarations (T007) → callback impl (T008-T009) → wire-up (T010-T011) → log-line replacement (T012). Within US3, T008 and T009 can be implemented in the same edit pass (both live in the same file region). T010, T011, T012 each modify a distinct line range in `src/slot.cpp` and could theoretically be done in three separate edits, but in practice one author edits all four sites in a single editing session.

### Parallel Opportunities

- **Within Phase 2**: T003 and T004 must be sequential (T004 calls `sanitize_for_filename` introduced by T003).
- **Across phases by different authors**: T002 (header) and T003-T004 (impl) cannot truly run in parallel because the header declarations and the impl bodies must match; this is a single-author serial edit in practice.
- **Phase 6 polish**: T014, T015, T016 can run in parallel — they touch different files (CHANGELOG vs source inspection).
- **Cross-feature parallelism**: this is a single-author bug-fix feature in a small codebase; the realistic execution is fully serial. Parallel slots are noted for completeness, not as a recommended workflow.

---

## Parallel Example: Phase 6 Polish

```bash
# All three tasks touch different files / are pure inspection:
Task: "Add Fixed + Changed entries to CHANGELOG.md [Unreleased] section"
Task: "Grep src/slot.cpp for any remaining 'Replay_%CCYY' / 'replay save OK' / 'replay save FAILED' occurrences"
Task: "Re-inspect src/slot.cpp lock-order invariant for on_replay_saved / log_replay_saved"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. **Phase 1**: T001 (baseline verification on the pre-fix build).
2. **Phase 2**: T002 → T003 → T004 (helper declared, sanitization implemented, format builder implemented).
3. **Phase 3**: T005 (one-line redirect at `slot.cpp:801`).
4. **STOP and VALIDATE**: build the plugin, run quickstart.md T1 / T2 / T3. Confirm no file overwrites on two-slot / three-slot same-second saves.
5. Deploy/demo if ready. This is a shippable MVP — US1 alone closes the silent-data-loss bug, which is the highest-priority concern in the spec.

### Incremental Delivery

1. **Setup + Foundational** → helper compiles, no runtime change.
2. **Add US1** → silent loss eliminated → ship as fix-1 (MVP).
3. **Add US2 (verification)** → attribution guarantee confirmed → no separate release needed (US2 is structurally delivered by US1's change).
4. **Add US3** → log line tells the truth on success and failure → ship as fix-2 (observability improvement).
5. **Polish** → CHANGELOG entries + grep audit + lock-order re-inspection → cut release.

Each US ships without breaking the previous ones because all changes are additive in `src/slot.cpp` and structurally independent of one another.

### Parallel Team Strategy

With multiple developers — **not recommended for this feature**, but possible:

1. Developer A: Phase 2 (T002, T003, T004) + Phase 3 (T005) + Phase 4 (T006).
2. Developer B: Phase 5 (T007-T012) in parallel, against the same `src/slot.cpp` file (merge resolution needed).
3. Both meet at Phase 6.

For a single author (this codebase's typical case), execute strictly in task-ID order T001 → T016.

---

## Notes

- **[P]** tasks = different files or pure inspection, no edit conflict.
- **[Story]** label maps task to specific user story for traceability.
- Each user story is independently completable and testable per quickstart.md.
- No automated tests are in scope; manual verification per quickstart.md is the test plan.
- Commit after each user-story phase (US1, US2-verification-as-part-of-US1, US3); the project uses Spec Kit's `after_implement` git hook for the final commit.
- Stop at any phase's checkpoint to validate that user story independently against quickstart.md.
- Avoid: editing `src/manager.hpp`, `src/manager.cpp`, `src/ui-dock.{hpp,cpp}`, `src/ui-slot-editor.{hpp,cpp}`, `src/plugin-main.{hpp,cpp}` — none are in scope (plan.md § Project Structure / Source Code).
- Avoid: any change to `src/slot.cpp:96-104` (the continuous recording filename construction) — out of scope per FR-006 / FR-008.
- Avoid: any new `obs_data_t` key, any new persisted slot configuration field — out of scope per plan.md § Storage and § Constraints.
