---
description: "Task list for feature 006-cqp-mismatch"
---

# Tasks: CQP value coherence across editor, log, and shared-encoder consumer

**Input**: Design documents from `specs/006-cqp-mismatch/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md), [data-model.md](./data-model.md), [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md), [quickstart.md](./quickstart.md)

**Tests**: This project has no automated test harness; tests are NOT generated. Verification is the 12-test manual procedure in [quickstart.md](./quickstart.md), executed in the Polish phase.

**Organization**: Tasks are grouped by the three user stories from spec.md (US1 P1, US2 P1, US3 P2). The Foundational phase delivers the shared infrastructure (resolution helper + factored quality-key list) every story depends on.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: User story (US1, US2, US3) — only on story-phase tasks
- Each task lists the exact file (and where relevant, the existing line range) it touches

## Path Conventions

Single-project OBS plugin. All source lives under `src/`; specs / docs under `specs/006-cqp-mismatch/`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Anchor the zero-regression baseline before any code change. There are no new build dependencies, no new translation units, no CMake changes.

- [X] T001 Build baseline: run the project's existing CMake configure + build on the current branch (`006-cqp-mismatch`) and confirm a clean build (no warnings beyond the project's existing baseline). Record commit SHA so the Polish phase can diff against it.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Deliver the two pieces every user story depends on — the resolution helper `SlotManager::effective_rate_control` (US1, US2, US3 all read through it directly or transitively) and the shared quality-key list `rc_util::quality_keys()` / `quality_split_keys()` (US3 wires existing call sites to it; T012 in US2 uses it for load-time clamp range introspection).

**Reference**: [research.md](./research.md) Decisions 1 + 7; [data-model.md](./data-model.md) § Entities; [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md) § Internal contract: `SlotManager::effective_rate_control`.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T002 [P] Add `struct EffectiveRC { std::string mode; uint32_t value; bool fallback; std::string owner_slot_name; };` and the member declaration `EffectiveRC effective_rate_control(const SceneSlot::Config& c) const;` to `SlotManager` in `src/manager.hpp`. Place the struct adjacent to `SharedEncoder`; place the member declaration in the public section near `config_by_slot_id`. Document threading (takes `mtx_` then releases; takes `shared_mtx_` then releases; never nested).
- [X] T003 [P] Declare `const char* const* quality_keys();` and `const char* const* quality_split_keys();` in `src/slot.hpp` under `namespace rc_util`, alongside the existing `is_bitrate_based` / `is_lossless` helpers. Document that both return null-terminated arrays and that the order in `quality_keys` is the canonical match order (first present key wins for both editor range and build-path write).
- [X] T004 Implement `rc_util::quality_keys()` and `rc_util::quality_split_keys()` in `src/slot.cpp` as `static constexpr const char* kQualityKeys[] = { "crf", "cqp", "cq_level", "qp", "icq_quality", "global_quality", nullptr };` and `kQualitySplitKeys[] = { "qpi", "qpp", "qpb", nullptr };` returned by accessor functions. Place near the existing `rc_util` namespace block at the top of the file. Depends on T003.
- [X] T005 Implement `SlotManager::effective_rate_control` in `src/manager.cpp`. (a) If `c.shared_encoder_slot_id.empty()`, return `{c.rate_control, c.rc_value, false, ""}`. (b) Else call existing `config_by_slot_id(c.shared_encoder_slot_id, owner)` (takes `mtx_` briefly). If lookup fails, return `{"CBR", 6000, false, ""}` as the safe last resort. (c) Else briefly take `shared_mtx_`, look up `shared_.find(c.shared_encoder_slot_id)`; if present and `encoder_fallback_ == true`, override `mode = "CBR"`, `value = rc_util::is_bitrate_based(owner.rate_control) ? owner.rc_value : 6000`, `fallback = true`. (d) Return `{<resolved mode>, <resolved value>, <fallback>, owner.name}`. Depends on T002.

**Checkpoint**: Foundation ready — `effective_rate_control` and the shared quality-key list are usable from any caller. Build must still be clean (no callers yet, only the helper itself).

---

## Phase 3: User Story 1 — Consumer slot reports the encoder it actually uses (Priority: P1) 🎯 MVP

**Goal**: when a consumer slot starts, the slot start log line reflects the owner's effective rate-control mode/value (or fallback values), not the consumer's stale fields. Replay-buffer memory-cap estimate uses the same effective values.

**Independent Test**: Test 1 (consumer reports owner's encoder), Test 2 (fallback values reported), Test 9 (Lossless rendering), Test 11 (standalone regression) from [quickstart.md](./quickstart.md).

**Reference**: [research.md](./research.md) Decisions 5 + 8; [data-model.md](./data-model.md) § Slot start — `SceneSlot::start()` log line; [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md) § Log line format.

### Implementation for User Story 1

- [X] T006 [US1] Reformat the slot start log line in `src/slot.cpp` (existing `blog(LOG_INFO, "[multi-scene-rec] '%s' started (...)")` at lines 562-566) to call `SlotManager::instance().effective_rate_control(cfg_)`. Build the rate-control segment with: `Lossless` when `rc_util::is_lossless(eff.mode)`, else `<eff.mode>/<eff.value>`. Prepend `[CBR fallback] ` when `eff.fallback`. Append ` inherited from '<eff.owner_slot_name>'` when `!eff.owner_slot_name.empty()`. Depends on T005.
- [X] T007 [US1] Redirect the replay-buffer memory-cap estimate in `src/slot.cpp` line 748 (`uint32_t est_kbps = rc_util::is_bitrate_based(cfg_.rate_control) ? cfg_.rc_value : 12000;`) to call `SlotManager::instance().effective_rate_control(cfg_)` and use `eff.mode` / `eff.value` instead of `cfg_.rate_control` / `cfg_.rc_value`. Note: `setup_outputs` runs under `slot_mtx_`; the helper takes `mtx_` and `shared_mtx_` independently and never `slot_mtx_`, so lock-order `mtx_ → slot_mtx_` is respected only if `mtx_` is taken first; here `slot_mtx_` is already held, so the helper's internal `mtx_` acquisition would invert the order — instead, **resolve the effective values BEFORE `setup_outputs` is called** (in `start()`, after the owner-Config resolution that already happens, pass the resolved effective values to `setup_outputs` via a local). Adjust the `setup_outputs` signature to accept the effective mode/value (or stash them on the slot). Depends on T005.

**Checkpoint**: US1 fully functional. Verify with quickstart Tests 1, 2, 9, 11 (no automation; manual run before moving on).

---

## Phase 4: User Story 2 — Editor does not present stale rate-control values for consumer slots (Priority: P1)

**Goal**: editor for a consumer slot shows the inherited mode/value read-only and labeled by the owner name; saving never leaves a slot with stale standalone fields; loading any pre-006 save normalizes the consumer's stale fields and warns once for out-of-range standalone values or unknown standalone modes.

**Independent Test**: Tests 3 (sentinel write on switch), 4 (editor inherited rows), 5 (load-time migration), 6 (clamp + warn), 7 (substitute + warn) from [quickstart.md](./quickstart.md).

**Reference**: [research.md](./research.md) Decisions 2, 3, 4, 6; [data-model.md](./data-model.md) § Load — `slot_from_data` and § Editor accept / Editor display; [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md) § Internal contract: load-time normalization and § Internal contract: editor inherited rows.

### Implementation for User Story 2

- [X] T008 [P] [US2] In `src/ui-slot-editor.cpp::on_accept` (the `if (venc_data.startsWith("shared:"))` branch at lines 693-700), add `cfg_.rate_control = "<inherited>";` and `cfg_.rc_value = 0;` immediately after `cfg_.video_encoder_id.clear();`. This is the symmetric save-side write for Decision 2: the user switching a slot to shared-encoder mode never leaves it with stale standalone values on disk.
- [X] T009 [US2] In `src/ui-slot-editor.cpp::update_shared_encoder_visibility` (lines 987-1024), replace the two `set_form_row_visible(form_, rc_combo_, !is_shared);` and `set_form_row_visible(form_, rc_value_spin_, !is_shared);` calls (lines 1003-1004) with the read-only inherited rendering: when `is_shared`, call `SlotManager::instance().effective_rate_control(cfg_)`, clear and add a single disabled combo item with `eff.mode`, set the value spinbox range to `[eff.value, eff.value]` (or `[0,0]` + `setSpecialValueText("— (lossless)")` when `rc_util::is_lossless(eff.mode)`), disable both widgets, and set the labels via the existing form using **one of two shapes**: (a) when `!eff.owner_slot_name.empty()` (normal case, owner Config resolved), labels read `Rate control (inherited from <eff.owner_slot_name>)` and `Value (inherited from <eff.owner_slot_name>)`; (b) when `eff.owner_slot_name.empty()` (the orphan-consumer case: `shared_encoder_slot_id` set but resolution failed, i.e. T005(b)'s safe last-resort branch returned `CBR / 6000 / fallback=false / owner_slot_name=""`), labels read `Rate control (inherited — owner missing)` and `Value (inherited — owner missing)` per the spec edge case "Owner slot deleted while a consumer survives (orphan consumer)". Append ` [CBR fallback]` to whichever label shape was chosen **only when `eff.fallback == true`** (the helper returns `fallback == false` when no `SharedEncoder` row exists yet, and also for orphans, in which cases no fallback suffix is added — per FR-005). When `!is_shared`, restore the standalone-edit state (re-enable, restore label text via `populate_rate_control_combo` + `update_rc_value_field` which already handle this). Depends on T005, T008.
- [X] T009b [US2] In `src/ui-slot-editor.cpp`, in the video-encoder combo's change handler (the slot that calls `update_shared_encoder_visibility`), detect the transition from `is_shared == true` to `is_shared == false` within an open editor session (i.e., the prior selection was `shared:` and the new selection is a real encoder; the persisted `cfg_.rate_control == "<inherited>"` with `rc_value == 0` is the typical precondition but the seeding rule applies regardless of the prior persisted shape). When detected: introspect the newly-selected encoder via `obs_get_encoder_properties` (same pattern as `populate_rate_control_combo` at lines 535-549) to get the first listed mode; set `cfg_.rate_control` to that mode; introspect the value range for that mode via the existing `introspect_quality_range` (for quality modes) or the `bitrate` property (for bitrate modes); set `cfg_.rc_value` to the midpoint of `[min, max]` clamped to that range (or `0` for `rc_util::is_lossless(cfg_.rate_control)`). When the introspection returns no range, set `cfg_.rate_control` to the seeded mode and leave `cfg_.rc_value` at the spinbox's current minimum — the user can edit before clicking Save. Re-run `populate_rate_control_combo` and `update_rc_value_field` so the rows reflect the seeded values. Implements FR-016. Depends on T004 (uses `rc_util::quality_keys`) and T008 (sentinel write defines the typical precondition shape). Note: T009b calls `introspect_quality_range` (existing); whether T009b is implemented before or after T014 (which factors `introspect_quality_range` to walk `rc_util::quality_keys`), the call site produces identical key ordering — the literal dependency is on T004, not T014.
- [X] T010 [P] [US2] In `src/manager.cpp::slot_from_data` (existing function at lines 337-447), after the existing back-compat defaults block (ends ~line 376), add the consumer-clear normalization with **explicit if/else branching** to make the standalone-only path of T011/T012 unambiguous: `if (!c.shared_encoder_slot_id.empty()) { c.rate_control = "<inherited>"; c.rc_value = 0; /* T010b orphan-warn runs in this branch; T011/T012 do NOT — consumer's own value is irrelevant (helper resolves to owner). */ } else { /* T011 + T012 run in this else branch for standalone slots only. */ }`. Implements Decision 2 / FR-006 / FR-012 at load time.
- [X] T010b [US2] In `src/manager.cpp` — at the site that has access to the full loaded slot-Config map (typically a second pass at the end of `slot_from_data`'s caller, after every slot's `slot_from_data` has run so forward references resolve correctly) — walk every Config with non-empty `shared_encoder_slot_id` and call the existing `config_by_slot_id` lookup to confirm the reference resolves. For each Config whose reference does NOT resolve (orphan consumer), emit a single warning: `blog(LOG_WARNING, "[multi-scene-rec] '%s': shared_encoder_slot_id '%s' does not resolve — orphan consumer; reads will return safe last-resort values until the user re-points the slot or deletes it", c.name.c_str(), c.shared_encoder_slot_id.c_str());`. The orphan state is otherwise non-fatal: T005(b)'s helper already returns safe last-resort values for orphans, and T009's editor label `(inherited — owner missing)` clarifies the dangling reference in the UI. Implements the spec edge case "Owner slot deleted while a consumer survives (orphan consumer)" SHOULD requirement. Depends on T010 (the sentinel-clear runs unconditionally for any non-empty `shared_encoder_slot_id`, including orphans; T010b only adds the warning for the subset whose reference does not resolve).
- [X] T011 [US2] In `src/manager.cpp::slot_from_data`, **inside T010's `else` branch (standalone-slot path: `c.shared_encoder_slot_id.empty()`)**, add the mode-substitute-and-warn step: introspect the encoder's `rate_control` property list (use `obs_get_encoder_properties(c.video_encoder_id.c_str())` + walk `obs_property_list_item_string` on the `rate_control` property — same pattern as `populate_rate_control_combo` at `ui-slot-editor.cpp:535-549`). If `c.rate_control` is not in the list and the list is non-empty, substitute the first listed mode and emit `blog(LOG_WARNING, "[multi-scene-rec] '%s': rate-control '%s' not supported by %s; substituted '%s'", c.name.c_str(), original_rc.c_str(), c.video_encoder_id.c_str(), c.rate_control.c_str());`. Implements Decision 4 / FR-015. Depends on T010.
- [X] T012 [US2] In `src/manager.cpp::slot_from_data`, **inside T010's `else` branch (standalone-slot path), after T011's substitute**, add the value-clamp-and-warn step. Introspect the relevant int range from the same `obs_properties_t*`: for bitrate-based modes (`rc_util::is_bitrate_based(c.rate_control)`) use the `bitrate` key; for lossless skip; for quality-based modes walk `rc_util::quality_keys()` and `rc_util::quality_split_keys()` (first present key wins, matching `set_quality_value`'s order). If a range is found and `c.rc_value` is outside `[min, max]`, clamp to the nearest endpoint and emit `blog(LOG_WARNING, "[multi-scene-rec] '%s': rc value %u out of range for %s on %s [%d, %d]; clamped to %u", c.name.c_str(), original_value, c.rate_control.c_str(), c.video_encoder_id.c_str(), r.min, r.max, c.rc_value);`. Free the properties handle. Implements Decision 3 / FR-013. Depends on T004, T011.

**Independent Test**: Tests 3 (sentinel write on switch), 3b (consumer → standalone return trip seeds valid defaults), 4 (editor inherited rows), 5 (load-time migration), 6 (quality-mode clamp + warn), 6b (bitrate-mode clamp + warn), 7 (substitute + warn) from [quickstart.md](./quickstart.md).

**Checkpoint**: US2 fully functional. Verify with quickstart Tests 3, 3b, 4, 5, 6, 6b, 7.

---

## Phase 5: User Story 3 — Owner's quality-mode value reaches the encoder under the same key the editor wrote (Priority: P2)

**Goal**: defend FR-007 by construction: the editor's introspected quality range and the encoder-build path's write target are derived from the same `rc_util::quality_keys()` / `quality_split_keys()` arrays. A future PR that adds a key to one side without the other becomes a compile-time follow-through to the single source.

**Independent Test**: Test 10 from [quickstart.md](./quickstart.md) (per-encoder verification of typed value reaching the encoder).

**Reference**: [research.md](./research.md) Decision 7.

### Implementation for User Story 3

- [X] T013 [P] [US3] In `src/slot.cpp::set_quality_value` (existing function at lines 91-117), replace the hard-coded `const char* single_keys[] = {...}` and `const char* split_keys[] = {...}` arrays with iteration over `rc_util::quality_keys()` and `rc_util::quality_split_keys()` (null-terminated). Preserve current behaviour exactly: first present single key wins (set + return); else set every present split key as a unit; else last-resort `obs_data_set_int(settings, "cqp", value)`. Depends on T004.
- [X] T014 [P] [US3] In `src/ui-slot-editor.cpp::introspect_quality_range` (anonymous-namespace helper at lines 52-62), replace the hard-coded `const char* keys[] = { "crf", "cqp", "cq_level", "qp", "icq_quality", "global_quality", "qpi" }` with iteration over `rc_util::quality_keys()` followed by iteration over `rc_util::quality_split_keys()` (in that order, returning the first found range). Now the editor's range source walks the exact same list (plus split keys) as `set_quality_value`'s write target. Depends on T004.

**Checkpoint**: All three user stories independently functional. Verify with quickstart Test 10 across the encoders available on the test machine.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: documentation, full quickstart run, regression coverage.

- [X] T015 [P] Add an entry to `CHANGELOG.md` at the repository root for this feature, under a new version section (per constitution § Development Workflow — Patch notes). User-visible change description: "consumer slots now report the owner's effective rate-control mode and value everywhere (editor, log, replay-buffer estimate), with read-only inherited labels in the editor; load-time validation clamps out-of-range values and substitutes unknown rate-control modes against the encoder's introspected lists, emitting a single warning per affected slot."
- [ ] T016 Execute all 15 tests in [quickstart.md](./quickstart.md) on **Windows (the only platform the maintainer can manually verify)**. Record results under `specs/006-cqp-mismatch/results/` per the quickstart template (folder is gitignored institutional memory). Tests 1, 2, 9, 11 verify US1; tests 3, 3b, 4, 5, 6, 6b, 7 verify US2; test 10 verifies US3 on the encoders available on the Windows test machine (typically NVENC + `obs_x264`; AMF / QSV where the hardware exists); test 8 verifies FR-009; test 12 is the constitution-VII regression check; test 13 verifies FR-010 (consumer starts while owner stopped, no prior `SharedEncoder` row). Any test outside PASS is a blocker for the feature. **Cross-platform manual coverage is out of scope** for this feature: macOS VideoToolbox and Ubuntu `obs_x264 + CRF` are NOT manually verified by the maintainer (Windows-only test environment). FR-007 / SC-004 coverage on those encoders rests on the structural guarantee from T013 + T014 (both the editor's range introspection and the encoder-build write path walk the same `rc_util::quality_keys()` / `quality_split_keys()` array by construction — so any encoder whose keys are in that list is covered without per-encoder manual testing) and on future community reports.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1, T001)**: No code dependencies. Start immediately.
- **Foundational (Phase 2, T002-T005)**: Depends on Setup. **Blocks all user stories.**
- **User Story 1 (Phase 3, T006-T007)**: Depends on Foundational (T005).
- **User Story 2 (Phase 4, T008-T012 incl. T009b and T010b)**: Depends on Foundational (T005 for T009, T004 for T012). T008/T010 do not depend on the helper — they only write the sentinel — and could in principle ship before Foundational, but they belong with their story for coherence. T010b depends on T010 (the sentinel-clear runs unconditionally for any consumer; T010b only adds the orphan-warning for the subset whose reference does not resolve).
- **User Story 3 (Phase 5, T013-T014)**: Depends on Foundational (T004).
- **Polish (Phase 6, T015-T016)**: Depends on all desired user stories. T016 needs every story checkpoint passing.

### User Story Dependencies

US1, US2, and US3 are **independent** of each other once Foundational is complete. They touch different functions in different files (US1: `slot.cpp` log + replay-buffer; US2: `ui-slot-editor.cpp` editor + `manager.cpp` load-time normalization; US3: `slot.cpp::set_quality_value` + `ui-slot-editor.cpp::introspect_quality_range`). T013 in US3 and T007 in US1 both edit `slot.cpp` but distinct functions; this is "same file, different functions" — sequential edit recommended, no logical dependency.

### Within Each Story

- US1: T006 → T007 (same file, sequential; both depend on T005).
- US2: T008 || T010 (different files); T009 depends on T008 (same file `ui-slot-editor.cpp`); **T009b depends on T008 (same file, same widget, but distinct handler — sequential edit recommended)**; T010b depends on T010 (same file `manager.cpp`, runs in T010's `if` branch — sequential edit); T011 depends on T010, T012 depends on T011 (all inside T010's `else` branch in `manager.cpp::slot_from_data`, three sequential edits to one function).
- US3: T013 || T014 (different files).

### Parallel Opportunities

- Phase 2 Foundational: **T002 || T003** (different files); then **T004 || T005** (different files, T004 depends on T003, T005 depends on T002).
- Phase 4 US2: **T008 || T010** (different files).
- Phase 5 US3: **T013 || T014** (different files).

Cross-story: once Foundational ships, US1 / US2 / US3 can be developed in parallel by different agents — they share no files except `slot.cpp` (US1 line 562 + line 748; US3 lines 91-117 — three disjoint hunks).

---

## Parallel Example: Phase 2 Foundational

```text
# Round 1 (declarations, different files):
T002: Add EffectiveRC + helper decl to src/manager.hpp
T003: Declare rc_util::quality_keys/quality_split_keys in src/slot.hpp

# Round 2 (implementations, different files; each depends on its matching declaration):
T004: Implement rc_util::quality_keys/quality_split_keys in src/slot.cpp
T005: Implement SlotManager::effective_rate_control in src/manager.cpp
```

## Parallel Example: US2 in flight

```text
T008: Sentinel write in ui-slot-editor.cpp::on_accept
T010: Consumer-clear in manager.cpp::slot_from_data
# (both safe to run in parallel — different files)

# Then sequentially in their files:
T009  (after T008): Read-only inherited rows in ui-slot-editor.cpp::update_shared_encoder_visibility
T009b (after T008): Consumer → standalone encoder-combo seed handler in ui-slot-editor.cpp
T010b (after T010): Orphan-consumer warning in manager.cpp (post-load second pass)
T011  (after T010): Mode-substitute-and-warn in manager.cpp::slot_from_data (inside else branch)
T012  (after T011): Value-clamp-and-warn in manager.cpp::slot_from_data (inside else branch)
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Phase 1 Setup (T001) — baseline.
2. Phase 2 Foundational (T002-T005) — helper + shared key list.
3. Phase 3 US1 (T006-T007) — log line + replay-buffer estimate.
4. **STOP and VALIDATE**: Run quickstart Tests 1, 2, 9, 11. The headline bug ("consumer log line reports CBR/6000 instead of CQP/23") is fixed at this checkpoint, even though saves still carry stale fields and the editor still hides rows.

This is a shippable intermediate state: the user sees correct values in the log immediately; the persisted-config-still-stale story is incomplete but not regressing anything.

### Incremental Delivery

1. MVP (US1) → demo.
2. Add US2 (T008, T009, T009b, T010, T010b, T011, T012) → editor inherited rows + consumer→standalone seed + on-disk normalization + orphan-consumer warning → demo. Now Story 2 ACC#1-#3 all pass; old saves migrate at load; orphan-consumer dangling references are surfaced to the user without crashing.
3. Add US3 (T013-T014) → owner-side range/key invariant defended → demo. Tests 10 across encoders.
4. Polish (T015-T016): CHANGELOG entry + full quickstart run.

### Parallel Team Strategy

With multiple agents post-Foundational:

- Agent A: US1 (T006-T007 in `slot.cpp`).
- Agent B: US2 (T008-T012 split between `ui-slot-editor.cpp` and `manager.cpp`).
- Agent C: US3 (T013-T014).

Then a single agent runs Polish (T015-T016).

---

## Notes

- This project has no automated test harness; verification is the manual [quickstart.md](./quickstart.md) procedure executed at T016.
- The constitution's NON-NEGOTIABLE principles (III Thread Safety, VII Recording & Replay Buffer Correctness) are preserved by construction; the helper takes `mtx_` and `shared_mtx_` independently (never nested), and no replay-buffer length / save semantics change — only the memory-cap estimate redirects to the owner's effective values.
- The on-disk format shape is unchanged. Pre-006 saves with stale consumer fields migrate silently at load (T010); the next save (T015 quickstart Test 5) persists the normalized form.
- Each task includes the exact file and where relevant, the existing line range, so the implementer can locate the edit site without rediscovering call sites.
- Commit after each task or logical group; the constitution's "each logical change is its own commit" rule applies.
