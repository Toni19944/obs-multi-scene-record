# Feature Specification: General performance and stability audit across all source files

**Feature Branch**: `005-general-perf-stability-audit`

**Created**: 2026-05-20

**Status**: Draft

**Input**: User description: "General performance and stability audit and optimization across all source files: `manager.cpp`, `manager.hpp`, `plugin-main.cpp`, `plugin-main.hpp`, `slot.cpp`, `slot.hpp`, `ui-dock.cpp`, `ui-dock.hpp`, `ui-slot-editor.cpp`, `ui-slot-editor.hpp`."

## Clarifications

### Session 2026-05-20

- Q: What's the posture of this audit — fix observed bugs, or polish a working plugin? → A: The plugin already works fine in normal use. This audit is **nitpicky cleanup and optimization**, not bug-fixing. The bar is "every closeable polish item is closed" rather than "every observed defect is fixed." It is still high-importance work — the goal is to leave the code in a state where no preventable performance overhead, latent stability risk, or dead code remains in the listed files.
- Q: Is the audit a single pass through each file, or does it iterate? → A: **Multi-pass.** One pass = visiting every one of the ten files exactly once. After a pass, if any file in that pass produced an edit that changes how other files call into it, the auditor MUST run another pass so that every file is examined again against the new contract. Repeat until a pass produces no triggering edits.
- Q: Does the file-visit order within a pass matter? → A: **No. The auditor chooses the order.** The order in the user's original input was alphabetical happenstance, not a constraint. Each pass MUST still visit all ten files exactly once, but the auditor may pick whatever within-pass order best fits the work (e.g., lowest-level / most-depended-on first). Convergence is defined per-pass, not per-position-within-a-pass, so order does not affect correctness.
- Q: What counts as an "edit" for triggering another pass? → A: **Contract-affecting edits only.** A pass is "non-empty" (triggers another pass) when it produces at least one contract-affecting edit — defined as a removed or renamed exported symbol, a narrowed function signature, a removed public branch / public condition, or a tightened invariant. Internal renames of private helpers, comment-only corrections, and pure-whitespace / formatting changes do NOT count as contract-affecting and do NOT, by themselves, trigger another pass. Cosmetic edits may still happen during any pass; they simply do not block convergence.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Dock and slot controls feel instant, and stay instant after the cleanup (Priority: P1)

A user clicks Add Slot, Remove Slot, the per-row state toggle, Start All, Stop All, or Save Replay, or double-clicks a row to open the editor, or saves edits and closes it. Each interaction visibly reflects the change within ~100 ms — never appears stuck, busy, or laggy. With a stats-enabled dock and ten or so configured slots, the 1 Hz stats refresh does not stutter the UI and stays paused when no slots are recording. The audit MUST NOT introduce any new visible latency, and it MUST close any nitpicky source of avoidable UI work it finds along the way.

**Why this priority**: UI responsiveness is the most visible kind of polish. It is also where the largest closeable wins typically live (over-eager full-table rebuilds in refresh paths, redundant work in stats refresh, costly work on the UI thread that could be skipped). This story extends feature 004's scope to include the editor dialog (`src/ui-slot-editor.cpp`) — opening it on a slot with many encoder options should not visibly stall, and its many encoder-specific show/hide cascades are a likely source of small redundant work to clean up.

**Independent Test**: With 10 slots configured (mix of running and idle), click each dock control in turn, then double-click each row to open and close the editor. Each visible effect appears within ~100 ms; opening the editor populates within ~200 ms. With no slots recording, the dock's stats timer remains paused. Compare a representative timing trace before and after the audit; latencies do not regress.

**Acceptance Scenarios**:

1. **Given** 10 slots configured, **When** the user clicks any dock control (Add, Remove, state toggle, Start All, Stop All, Save Replay), **Then** the visible effect appears within ~100 ms with no UI freeze.
2. **Given** the same setup, **When** the user double-clicks a row, **Then** the slot editor opens and populates (scene combo, encoder combo, all encoder-specific widgets) within ~200 ms.
3. **Given** the editor is open, **When** the user changes the video-encoder selection, **Then** the encoder-specific widgets reconfigure without a visible stall and without redundant cascades through change-signal slots.
4. **Given** the dock is hidden behind another panel or its stats checkbox is off, **When** any slot is recording, **Then** stats refresh does not spend UI-thread time on widgets the user cannot see.

---

### User Story 2 - Plugin has near-zero cost when idle, and the audit does not erode that (Priority: P1)

A user has the plugin loaded with N slots configured but none currently recording. OBS is open and otherwise idle. The plugin contributes essentially nothing to OBS's process cost in this state — no measurable plugin-attributable CPU, no background work running that does not need to run when nothing is being recorded. Adding more configured-but-stopped slots does not increase idle cost. This story carries forward feature 004's idle-cost bar (FR-002 / FR-003 / SC-002 / SC-003) and the audit MUST NOT regress it.

**Why this priority**: Many users keep the plugin installed for occasional use. If having it loaded adds a per-second tax even when nothing is recording, the user pays that tax for every hour OBS is open. A nitpicky audit is exactly the kind of change that could re-introduce a small always-on cost (e.g., a new timer, an idle-state check that scans the slot list every tick) — the bar from 004 must hold after this feature ships.

**Independent Test**: Cold-start OBS with the plugin loaded, no slots configured. Measure idle process CPU over 60 seconds via Task Manager / Resource Monitor. Add 10 stopped slots and measure again. The two should be indistinguishable within measurement noise — same as feature 004.

**Acceptance Scenarios**:

1. **Given** OBS open with the plugin loaded and no slots, **When** the user observes plugin-attributable CPU over 60 seconds of idle, **Then** it is at or near 0 %.
2. **Given** OBS open with the plugin loaded and 10 stopped slots, **When** the user observes plugin-attributable CPU over 60 seconds of idle, **Then** it is essentially identical to the no-slots baseline.
3. **Given** OBS open with the plugin loaded and 1 slot recording, **When** the user observes plugin-attributable polling, **Then** any per-second work (stats refresh, dock paint) is scaled to running slots only, not the total configured count.

---

### User Story 3 - Latent stability risks are closed even though no user has ever hit them (Priority: P1)

The plugin works fine in normal use today. The audit's job on the stability axis is to find the *risks* that have not surfaced yet — code paths that would race, deadlock, leak, or use-after-free under the right unlucky timing or unusual session — and close them. This is the "nitpicky" side of stability: not "fix the crash" (there isn't one to fix) but "remove the conditions under which a crash could happen later."

**Why this priority**: The plugin touches libobs callbacks on worker threads, Qt widgets on the UI thread, and frontend events around scene-collection / load / save / unload — exactly the surface where lifetime, lock-order, and use-after-free defects hide latent for years until an unlucky user hits the timing. Closing those now, while the relevant code is fresh in mind, is much cheaper than diagnosing a future crash report.

**Independent Test**: Read the audit's stability findings list in `research.md`. For each finding, the disposition is either "closed" (with the commit visible in this feature's history) or "accepted with documented rationale" (e.g., libobs constraint, intentional design choice). No "TODO: revisit later" entries. Then run a 60-minute representative session — start/stop, scene-collection round-trip, Ctrl+S during recording, dock open/close, editor open on every row, OBS close-and-reopen — and confirm no regression.

**Acceptance Scenarios**:

1. **Given** the lock order documented in Constitution Principle III, **When** the auditor inspects every code path that acquires more than one of the four plugin mutexes, **Then** every path acquires them in the documented order, and any deviation is closed in this feature or carries a documented rationale.
2. **Given** every site that acquires an OBS handle (source / view / video / encoder / output / hotkey id / data array / signal connection / frontend callback), **When** the auditor inspects each acquire, **Then** every reachable exit path — including failure / early-return paths — releases the handle exactly once, OR the site carries a documented (b)-disposition.
3. **Given** every worker-thread → UI interaction (output stop signal, frontend event), **When** the auditor inspects each, **Then** none calls into Qt widgets directly; all route through `SlotManager` and `QMetaObject::invokeMethod` (or equivalent).
4. **Given** the representative 60-minute session, **When** the user runs it after the audit ships, **Then** OBS does not crash, hang, or display a "leaked handle" / "callback fired after unload" log line attributable to the plugin.

---

### User Story 4 - Multi-pass methodology leaves no dead code or stale call site behind (Priority: P2)

The audit walks through every one of the ten in-scope files in each pass (auditor chooses the within-pass order). After a pass, if any file's edit changed a contract other files depended on — a removed function, narrowed signature, removed branch, renamed exported entity, an invariant tightened — the auditor MUST run another pass so every file is re-examined against the new contract. Now-dead code, stale comments, and now-redundant call sites are removed in the revisit. Passes repeat until one produces no contract-affecting edits. The result is a code state with no orphaned plumbing tying to changes made elsewhere in the audit.

**Why this priority**: A single-pass audit on N coupled files almost always leaves dead code at the seams. When `slot.cpp` changes what it exposes, `manager.cpp` and `ui-dock.cpp` may still be calling the old shape (or guarding against a condition that can no longer occur). One sweep is not enough; two or three passes typically settle things. This is exactly the failure mode "nitpicky cleanup" is meant to prevent, so it gets its own user story.

**Independent Test**: Read the audit log (a brief per-pass log in `research.md`). It records each pass — the within-pass order the auditor chose, what was edited per file, and which files were revisited as a result of contract changes. The final pass produced no contract-affecting edits. There is no `// TODO`, no dead branch matching a removed condition, and no stale comment referencing a no-longer-present concept in any of the ten files.

**Acceptance Scenarios**:

1. **Given** the audit has shipped, **When** a maintainer reads the per-pass log in `research.md`, **Then** they can see each pass produced fewer or equal contract-affecting edits than the prior pass, and the last pass produced zero.
2. **Given** any file edited in any pass, **When** the maintainer grep-checks the other nine files for references to symbols removed or renamed by that edit, **Then** there are zero stale references and zero now-orphaned branches.
3. **Given** any file that was edited because another file produced a contract change, **When** the maintainer inspects the relevant call sites, **Then** they are updated consistently and any guard for a now-impossible condition is removed.

---

### User Story 5 - The audit document captures every finding with disposition (Priority: P3)

A maintainer reads `research.md` for this feature and sees, for each source file in scope, every finding the audit produced — both performance and stability — and what disposition was assigned to each (closed in this feature, or accepted with documented rationale). The document also includes the per-pass log from US4. Same institutional-memory pattern as features 003 and 004.

**Why this priority**: Closing items once is good; the document is what stops them coming back. Lower priority because it does not change runtime behavior; higher priority than "nice to have" because every prior perf/stability feature in this project has paid off by having one.

**Independent Test**: Read `specs/005-general-perf-stability-audit/research.md`. For each of the ten files in scope, the document has a section listing findings with dispositions. A separate section logs each audit pass with file-by-file edits.

**Acceptance Scenarios**:

1. **Given** the feature has shipped, **When** a maintainer opens `research.md`, **Then** every file in scope has a dedicated section enumerating findings with one of the dispositions defined in FR-008.
2. **Given** the same document, **When** the maintainer reads the per-pass log, **Then** each pass is enumerated with what was edited per file and which prior files were revisited as a result.
3. **Given** a future PR proposing a change in any of those files, **When** the maintainer reviews it, **Then** the change can be cross-checked against the document to flag re-introduction of a closed finding.

---

### Edge Cases

- **Many slots configured (~20)**: dock should still feel responsive; any UI work that scales worse than O(N) with slot count is a finding to investigate.
- **All slots running simultaneously**: stats polling, save callback, and start/stop-all paths must not cause UI hitches or lock contention.
- **Scene-collection switch while recording**: the existing teardown / rebuild flow is the typical place for handle-release bugs; verified leak-free post-audit.
- **OBS shutdown while recording**: the module-unload sequence must stop slots cleanly before the dock and SlotManager are destroyed; no dangling callback fires after unload.
- **Module reload during development**: removing the plugin DLL/SO and reloading must not leave orphaned OBS handles or fire stale callbacks.
- **Encoder fallback (NVENC → x264 CBR)**: a slot that falls back must not leak the original encoder context; the `[CBR fallback]` indicator must continue to surface for owner and every sharer.
- **Shared encoder context (a slot referencing another's encoder via `shared_encoder_slot_id`)**: stopping the owner must not disturb a sharer; releasing the last consumer must tear the context down in encoder → view → scene order with no leak.
- **Disk-full / encoder-failure mid-recording**: the worker-thread stop signal must not crash, deadlock, or leave the slot row in an inconsistent state.
- **Slot editor opened on a slot whose backing config is being updated by another thread**: dialog must populate from a coherent snapshot, not a torn read.
- **Multi-pass churn**: if a late-pass edit causes broad earlier-file revisits, the audit MUST converge — passes producing zero edits MUST be reachable within a small finite number of passes. If passes appear to ping-pong (file A → file B → file A → file B …), that's itself a finding worth documenting.
- **OBS background-tab / window-minimized**: stats refresh should not run at full rate when the dock is not visible.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The audit MUST be conducted as a sequence of full passes. **One pass = visiting every one of the ten in-scope files exactly once.** The auditor chooses the within-pass order (e.g., lowest-level / most-depended-on first); the order is not pinned by the spec and may differ between passes. When any file in a pass produces a **contract-affecting edit** — defined as a removed or renamed exported symbol, a narrowed function signature, a removed public branch / public condition, or a tightened invariant — the auditor MUST run another full pass so every file is examined against the new contract. Internal renames of private helpers, comment-only corrections, and pure formatting / whitespace changes do NOT count as contract-affecting and do NOT, by themselves, trigger another pass; they may still be applied during any pass. Passes MUST repeat until one produces zero contract-affecting edits. The pass log — including the within-pass order the auditor chose for each pass — is recorded in `research.md`.
- **FR-002**: Every code path that acquires more than one of the four plugin mutexes (`SlotManager::mtx_`, `SceneSlot::slot_mtx_`, `SceneSlot::stats_mtx_`, `SlotManager::shared_mtx_`) MUST acquire them in the order documented in Constitution Principle III. Any deviation surfaced by the audit MUST be either closed or documented as a deliberate, safe exception with rationale.
- **FR-003**: No plugin code path MUST call into Qt widgets from a libobs worker thread directly. Worker-thread → UI interactions MUST route through `get_dock()` + `QMetaObject::invokeMethod` (or equivalent). Findings to the contrary MUST be closed.
- **FR-004**: Every OBS handle the plugin acquires (sources, views, video outputs, encoders, outputs, hotkey ids, data arrays, signal-handler connections, frontend event callbacks) MUST be released exactly once across the relevant lifecycle event. The audit MUST inspect every acquire site for a matching release on every reachable exit path, including failure / early-return paths.
- **FR-005**: All user-initiated dock actions (Add, Remove, state toggle, Start All, Stop All, Save Replay, Edit) MUST produce their visible effect within ~100 ms under typical workloads (≤10 slots). Slot-editor dialog open MUST populate within ~200 ms. UI work that visibly stalls in this range MUST be either optimized or documented as irreducible. The audit MUST NOT regress this latency.
- **FR-006**: Background plugin work that runs while no slot is recording MUST be reduced to the minimum required for correctness. The 1 Hz stats QTimer MUST remain gated such that it does not consume per-tick work when no slots are running (carry-forward of feature 004 FR-002). The auditor MUST confirm the gate still holds across this feature's changes and identify any new always-on work this audit introduces or surfaces.
- **FR-007**: Per-slot work in idle state (slot exists but is not recording) MUST be O(1), not O(N) or worse, with respect to the slot count. The audit MUST NOT introduce any per-stopped-slot tax (carry-forward of feature 004 FR-003).
- **FR-008**: For every audit finding (performance or stability or dead-code/cleanup), the disposition MUST be one of: (a) **closed in this feature**, OR (b) **accepted with documented evidence** — a cited libobs / Qt API constraint, a conflicting feature requirement, or a measured / argued zero contribution. No undocumented finding. Same disposition scheme as features 003 / 004.
- **FR-009**: The audit MUST cover every source file listed in the user input. Each file MUST have its own section in `research.md` enumerating findings with dispositions.
- **FR-010**: The audit's scope per file MUST include three axes — performance (CPU cost, idle cost, hitch risk, scaling), stability (thread safety, lifetime / ownership, exception safety, error-path correctness, latent race / deadlock / leak / use-after-free risk), and **cleanup** (dead code, stale comments, redundant work, now-unreachable branches, naming or shape that mis-states current behavior). Not just one or two.
- **FR-011**: The audit's posture is **nitpicky cleanup and optimization**, not bug-fixing. Findings the auditor would otherwise skip ("not a bug — works fine") are still in scope when closing them removes preventable overhead, latent risk, or dead code. The bar is "every closeable polish item is closed," not "every observed defect is fixed."
- **FR-012**: After every pass that produces at least one contract-affecting edit, the next pass MUST verify that no file references the changed contract in a stale way (e.g., a guard for a now-impossible condition, a call to a removed helper, a comment that names a removed concept). Stale references found MUST be closed in that next pass — regardless of whether the file containing the stale reference was visited earlier or later than the file that produced the contract change in the originating pass.
- **FR-013**: Memory baseline (plugin loaded with no slots) MUST be a measurable static value and recorded in `quickstart.md` for future regression detection (carry-forward of feature 004 FR-008).
- **FR-014**: No regression in features 001 – 004. Hotkey registration, dock UI sync, recording-pipeline behavior, and the non-recording perf gains from feature 004 MUST continue to pass their existing quickstart procedures.
- **FR-015**: Findings in `ui-dock.cpp` / `ui-slot-editor.cpp` MUST preserve Constitution Principle IV (UI / Logic Separation). No fix may introduce a direct libobs or `obs-frontend-api` call from widget code.
- **FR-016**: Findings in `slot.cpp` / `manager.cpp` MUST preserve Constitution Principle II (Clear Ownership & Minimal Shared State). No fix may give a slot direct access to another slot's pipeline; cross-slot data flow MUST continue to route through `SlotManager`. The `SharedEncoder` symmetric-consumer semantics MUST remain intact.

### Key Entities

- **Audit finding**: a specific behavior, code shape, or comment in one of the ten in-scope files that diverges from the "minimum required for correctness and clarity" ideal. Findings fall on three axes: performance, stability (latent risk), or cleanup (dead / stale code). Each finding has a disposition under FR-008.
- **Pass**: one ordered walk through the ten in-scope files (file #1 → file #10). A pass produces a set of edits (possibly empty). The audit comprises at least one pass; multi-pass audits continue until a pass produces zero edits (FR-001).
- **Revisit**: an instance where a file is re-examined in a subsequent pass because some file's edit in the prior pass changed the contract this file relied on. Revisits are logged per pass in `research.md`. ("Earlier" and "later" here refer to pass numbers, not within-pass visit order — the within-pass order is the auditor's choice and not pinned.)
- **Disposition**: one of (a) closed in this feature, or (b) accepted with documented evidence. Same scheme as features 003 / 004.
- **Plugin handle**: any OBS C-API object the plugin acquires (`obs_source_t`, `obs_view_t`, `video_t`, `obs_encoder_t`, `obs_output_t`, `obs_hotkey_id`, `obs_data_array_t`, signal-handler connection, frontend event callback) whose lifetime the plugin is responsible for.
- **Idle plugin**: the plugin loaded in OBS with no slots currently recording. Its CPU/memory cost is the baseline measured against (carry-forward from feature 004).

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every finding catalogued in the audit document has an FR-008 disposition. No undocumented divergence between "what the plugin does" and "the minimum required for correctness, performance, and clarity" in any of the ten files in scope.
- **SC-002**: The audit ran as a multi-pass sequence and the **final pass produced zero edits**. The pass log in `research.md` shows this convergence.
- **SC-003**: No file is left with a stale reference (comment, branch, or call site) to a contract that another file's edit changed. Grep-checked finding-by-finding.
- **SC-004**: Zero plugin crashes, deadlocks, or visible UI freezes attributable to changes made in this feature across the representative test workload (the 60-minute session described in US3).
- **SC-005**: With the plugin loaded and no slots running, plugin-attributable idle CPU over 60 seconds is at or near 0 % (within measurement noise) — no regression vs. feature 004 baseline.
- **SC-006**: With 10 stopped slots configured, plugin-attributable idle CPU is essentially identical to the no-slots baseline (no per-stopped-slot tax).
- **SC-007**: Dock UI actions complete (visible effect) within ~100 ms in typical-workload tests; editor open within ~200 ms.
- **SC-008**: After stop-all on a long session, plugin-attributable memory returns to within ~30 MB of the pre-recording baseline within 30 seconds.
- **SC-009**: OBS module-unload completes without any "callback fired after unload" or "leaked handle" log line attributable to the plugin.
- **SC-010**: Zero regressions in the quickstart procedures of features 001 (hotkey registration), 002 (dock UI sync), 003 (recording pipeline), 004 (non-recording perf).

## Assumptions

- **Scope** is bounded to the ten source files explicitly listed in the user input. Other parts of the repository (CMake, locale, buildspec) are out of scope unless a finding inside one of the ten files directly depends on them.
- **The plugin works fine today.** This audit is a nitpicky polish pass — fixing latent risks, closing small inefficiencies, and removing dead code — not a response to a user-reported crash, hang, or perf complaint. The bar is closeable polish, not bug triage.
- **Three-axis audit per file**: performance, stability (latent risk), cleanup (dead / stale code). Findings on each axis share one disposition scheme (FR-008) but are documented under separate axis headings within the file's section in `research.md`.
- **Multi-pass methodology** is mandatory (FR-001). Single-pass audits typically leave stale call sites and dead code at file boundaries; the audit converges when a pass produces zero edits. If convergence is not reached within a small finite number of passes, that itself is a finding worth documenting.
- **Process-oriented bar, not numeric**: same posture as features 003 and 004 — every finding has a documented disposition; every (a)-disposition finding is closed; the final pass is empty. Numeric thresholds (~100 ms UI, ~200 ms editor open, ~16 ms save, ≈0 % idle CPU, 50 MB / 30 MB memory bounds) are institutional memory recorded in `quickstart.md`, not strict pass/fail gates.
- **No automated test harness exists**: verification is manual via the OBS GUI, FPS overlay tools (hitch detection), Task Manager / Resource Monitor (CPU / memory), OBS logs (handle leaks, callback-fired-after-unload warnings), and code review (lock-order audit, dead-code scan). Same constraint as features 001 – 004.
- **Constitution principles are non-negotiable.** Principles I (Native OBS API Compliance), II (Clear Ownership & Minimal Shared State), III (Thread Safety), IV (UI / Logic Separation), and V (Encoder Robustness & Graceful Fallback) MUST be preserved across every finding's fix. A fix that would weaken any of them is not acceptable and MUST be re-scoped or replaced with a documented (b)-disposition.
- **Per-slot independence** is preserved unconditionally — same constraint as features 003 and 004.
- **Feature 003 and 004 closures are not re-audited.** Prior dispositions stand. This feature widens the lens (every listed file, three axes, multi-pass methodology) and picks up everything they did not address.
- **Stability findings the audit cannot reproduce on demand** (theoretical race windows, hypothetical use-after-free under specific timing) are still in scope: a credible code-review-level argument for the risk is sufficient to require a disposition.
- **`research.md` is the deliverable for the audit body**: per-file findings with dispositions, plus a per-pass log showing convergence. Numeric measurements live in `quickstart.md` as the regression-detection reference.

## Resolved Clarifications

- **Posture**: nitpicky cleanup and optimization of a plugin that already works fine. Latent risks and small inefficiencies are in scope; bug triage is not the framing. (See Clarifications, 2026-05-20.)
- **Methodology**: multi-pass — one pass = all ten files visited exactly once in an auditor-chosen order. When any pass produces a contract-affecting edit, every file is revisited in the next pass against the new contract. Passes repeat until one produces zero contract-affecting edits. The within-pass order is not pinned and may differ between passes. (See Clarifications, 2026-05-20.)
- **Scope**: bounded to the ten files listed in the user input. Three axes (performance, stability, cleanup) per file.
- **Bar**: process-oriented (every finding has a disposition; every closeable finding is closed; final pass is empty) plus an absolute no-regression bar (SC-004, SC-010). Numeric latency / CPU / memory targets are institutional memory.
- **What counts as a "finding"**: (i) avoidable performance overhead (CPU, idle work, hitch risk, super-linear scaling) attributable to plugin code, OR (ii) a credibly-identified latent stability risk (lock-order, lifetime, race, leak, use-after-free, error-path) — observability not required, code-review evidence suffices, OR (iii) dead code, stale comment, redundant work, or now-unreachable branch in one of the ten files.
- **Relationship to prior features**: this feature does NOT re-open closures from features 003 / 004. It widens scope (every listed file, three axes, multi-pass methodology).
