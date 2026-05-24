# Feature Specification: General performance pass (non-recording subsystems)

**Feature Branch**: `004-general-perf-pass`

**Created**: 2026-05-19

**Status**: Draft

**Input**: User description: "General performance optimization." Scoped at clarification time to: a broad audit of every subsystem feature 003 did NOT cover — UI responsiveness, slot lifecycle latency, idle CPU/memory cost, save/load performance, stats poll overhead. Fix what we find; document the rest.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Dock and slot controls feel instant (Priority: P1)

A user clicks "Add Slot," "Remove Slot," the per-row state toggle button, "Start All," or "Stop All" in the dock. The dock visibly reflects the change within the time it takes them to move their cursor to the next thing — i.e., it never appears stuck, busy, or laggy. The same is true when they double-click a row to open the editor and again when they save edits and close it.

**Why this priority**: Sluggish UI is the most visible kind of performance bug to a user. Even a well-functioning plugin feels broken if the dock takes half a second to respond to a click. This is also the area most likely to have low-hanging optimization wins (over-eager rebuilds, redundant work in refresh paths) since the recording pipeline (the more carefully tuned subsystem) was already audited in feature 003.

**Independent Test**: With 5 slots configured, click each dock control (Add, Remove, Start All, Stop All, per-row toggle, Edit). Each click should produce its visible effect within ~100ms — no visible "freeze" frame, no cursor change to busy/wait. Open and close the slot editor on a slot 10 times in a row; both transitions should be smooth.

**Acceptance Scenarios**:

1. **Given** 5 slots configured in the dock, **When** the user clicks the state-toggle button on any slot, **Then** the row's state column updates within ~100ms with no visible UI freeze.
2. **Given** the same setup, **When** the user clicks "Start All," **Then** every row reflects its post-start state within ~500ms (allowing time for the encoders to actually start; the UI itself should not be the bottleneck).
3. **Given** 5 slots, **When** the user clicks "Add Slot," **Then** a new row appears within ~100ms.
4. **Given** the editor is open, **When** the user clicks Save, **Then** the editor closes and the dock reflects the change within ~200ms.

---

### User Story 2 - Plugin has near-zero cost when idle (Priority: P1)

A user has the plugin loaded with N slots configured but none currently recording. OBS is open and idle (no streaming, no recording, no preview). The plugin should contribute essentially nothing to OBS's process cost in this state — no measurable CPU usage attributable to plugin code, no background work running that doesn't need to run when nothing is being recorded.

**Why this priority**: Many users keep the plugin installed for occasional use. If just *having it loaded* adds idle CPU cost (e.g., a timer firing every second to refresh stats for slots that aren't recording), the user pays a tax for the entire time OBS is open even when they're not using the plugin. This is also where a worst-case-baseline regression test fits cleanly: the idle plugin's cost should not grow with the number of configured slots.

**Independent Test**: Cold-start OBS with the plugin loaded but no slots configured. Measure idle process CPU over 60 seconds (Task Manager / Resource Monitor). Then add 10 stopped slots and measure again. The two should be essentially identical — adding slots-that-aren't-running should NOT add idle CPU.

**Acceptance Scenarios**:

1. **Given** OBS open with the plugin loaded and no slots, **When** the user observes plugin-attributable CPU over 60 seconds of idle, **Then** it is at or near 0%.
2. **Given** OBS open with the plugin loaded and 10 stopped slots, **When** the user observes plugin-attributable CPU over 60 seconds of idle, **Then** it is essentially identical to the no-slots baseline (no per-slot idle tax).
3. **Given** OBS open with the plugin loaded and 1 slot recording, **When** the user observes plugin-attributable CPU, **Then** any per-second polling (e.g., stats refresh) is scaled to the number of *running* slots, not the total slot count.

---

### User Story 3 - Slot lifecycle operations don't block the UI thread (Priority: P2)

When the user adds, removes, or updates a slot — or when the plugin's save callback fires during a normal OBS save — the UI thread does not freeze. Any cross-thread synchronization (lock acquisition, signal handler running) takes much less time than a frame at the user's framerate, so it never causes a visible hitch.

**Why this priority**: A blocking UI thread is what makes "the dock feels laggy" symptoms become "OBS freezes for a second when I save." This is qualitatively worse than a slow dock click — it affects the whole OBS UI, not just our plugin. This usually correlates with US1, but it's a stricter check (it cares about the worst case, not the typical case).

**Independent Test**: With 10 slots configured (mixture of running and idle), trigger OBS's save (Ctrl+S or File > Save). The save should complete without any visible OBS UI freeze. Repeat 20 times. No single save should produce a visible hitch.

**Acceptance Scenarios**:

1. **Given** 10 slots configured (5 running, 5 idle), **When** OBS's save callback fires, **Then** the OBS UI does not visibly freeze.
2. **Given** the same setup, **When** the user clicks "Stop All," **Then** the UI remains responsive throughout the stop sequence — buttons can be clicked, the slot editor can be opened, etc.
3. **Given** the same setup, **When** the user opens the slot editor on any row, **Then** the editor populates and displays within ~200ms with no visible UI freeze.

---

### User Story 4 - The audit document captures every finding (Priority: P3)

A maintainer reads `research.md` for this feature and can see, for each non-recording subsystem (UI dock, slot editor, slot lifecycle, save/load, stats poll, frontend event callbacks, idle behavior), what was found, what was fixed, and what was deliberately left as-is with rationale. Same institutional-memory pattern as feature 003.

**Why this priority**: Same reasoning as feature 003's US3. Closing a perf issue once is good; documenting *why* it was an issue and *what was changed* is what keeps the next regression from sneaking in.

**Independent Test**: Read `specs/004-general-perf-pass/research.md`. For each subsystem listed in FR-006, the document has a section enumerating findings with dispositions.

**Acceptance Scenarios**:

1. **Given** the feature has shipped, **When** a maintainer opens `research.md`, **Then** every subsystem in FR-006 has its own dedicated section with a list of findings and dispositions.

---

### Edge Cases

- **Many slots configured** (~20): the dock should still feel responsive; UI work that scales worse than O(N) with slot count is a finding to investigate.
- **All slots running simultaneously**: stats polling should not cause UI hitches.
- **Plugin loaded but OBS just-started**: cold-start cost should be low; the plugin shouldn't add measurable startup latency.
- **Scene collection with no slots**: should be a no-op — no timers, no callbacks doing meaningful work.
- **Frontend events at high frequency**: switching scenes rapidly, opening/closing Settings repeatedly, etc. — none of this should trigger plugin work proportional to the frequency.
- **OBS background-tab / window-minimized**: when OBS isn't in focus, the plugin should not be doing visible work either. (Mostly an OBS-level concern, but if our stats poll runs at full rate while the dock is hidden, that's a finding.)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: All user-initiated dock actions (Add, Remove, state toggle, Start All, Stop All, Save in editor) MUST produce their visible effect within ~100ms under typical workloads (≤10 slots). UI work that visibly stalls in this range MUST be either optimized or, if irreducible, documented with rationale.
- **FR-002**: Background plugin work (timers, callbacks) that runs while no slot is recording MUST be reduced to the minimum required for correctness. The 1Hz stats polling timer is the prime candidate: it MUST be paused when no slots are running, not run continuously.
- **FR-003**: Per-slot work in idle state (slot exists but is not recording) MUST be O(1), not O(N) or worse, with respect to the slot count. Having more configured-but-stopped slots MUST NOT increase idle CPU proportionally.
- **FR-004**: OBS's save callback (`save_cb`) MUST complete promptly — well under one display frame at 60fps (~16ms) for the typical case (≤10 slots). If a per-slot serialization step is expensive, it MUST be optimized or documented.
- **FR-005**: The dock refresh and stats-refresh paths MUST avoid unnecessary work: if no slots have changed, a stats refresh MUST NOT rebuild rows; if a stats refresh sees only one slot has changed state, the per-row update MUST NOT trigger a full table rebuild unless required for layout correctness.
- **FR-006**: The audit (the deliverable for User Story 4) MUST cover every non-recording subsystem in scope: UI dock (`src/ui-dock.cpp`), slot editor (`src/ui-slot-editor.cpp`), slot lifecycle (`SlotManager` and `SceneSlot` excluding the recording pipeline already audited in feature 003), save/load (`save_to` / `load_from` / `save_cb`), stats poll (the 1Hz QTimer), frontend event callbacks (`frontend_event_cb`), and idle behavior (anything that runs when nothing is recording).
- **FR-007**: For each audit finding, the disposition MUST be one of: (a) closed in this feature, OR (b) accepted with documented evidence (libobs / Qt API constraint, conflicting requirement, or empirical zero-contribution measurement). Same scheme as feature 003 FR-009.
- **FR-008**: Memory baseline of the plugin loaded with no slots MUST be a measurable static value (not growing over time) and recorded in `quickstart.md` as institutional memory for future regression detection.
- **FR-009**: No regression in features 001–003. Hotkey registration, dock UI sync, and recording-pipeline behavior MUST continue to work as those features specified.

### Key Entities

- **Idle plugin**: the plugin loaded in OBS with no slots currently recording. Its CPU/memory cost is the baseline we measure against.
- **Audit finding**: a specific behavior observed during the audit that diverges from the "minimum work required for correctness" ideal. Each finding has a disposition (closed / accepted with rationale).
- **UI hitch**: a visible pause in OBS's UI thread attributable to plugin code — a frame-time spike that the user notices, typically >50ms.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every finding catalogued in the audit document has an FR-007 disposition. No undocumented divergence between "what the plugin does" and "the minimum required for correctness" in the subsystems listed in FR-006.
- **SC-002**: With the plugin loaded and no slots running, plugin-attributable idle CPU over 60 seconds is at or near 0% (within measurement noise).
- **SC-003**: With 10 stopped slots configured, plugin-attributable idle CPU is essentially identical to the no-slots baseline (no per-stopped-slot tax).
- **SC-004**: Dock UI actions complete (visible effect) within ~100ms in typical-workload tests.
- **SC-005**: OBS UI does not visibly freeze during plugin operations (save, start/stop all, slot lifecycle, scene-collection switch).
- **SC-006**: Zero regressions in the quickstart procedures of features 001 (hotkey registration), 002 (dock UI sync), 003 (recording pipeline).
- **SC-007**: Zero plugin crashes attributable to changes made in this feature.

## Assumptions

- **"General performance optimization" scope**: clarified at spec time to mean "every subsystem feature 003 did NOT cover" — i.e., UI, slot lifecycle, save/load, idle behavior, and stats polling. The recording pipeline is explicitly out of scope (feature 003 owns that).
- **Process-oriented bar, not numeric**: same posture as feature 003 — the bar is "every audit finding has a documented disposition; every (a)-disposition finding is fixed." Numeric thresholds (~100ms UI, ~16ms save, ≈0% idle CPU) are guidance and are recorded as institutional memory; they're not strict pass/fail gates. The user can tighten any of these via `/speckit-clarify` once early measurements are in hand.
- **Audit scope is bounded** to plugin code under `src/`. Qt-internal cost, OBS-internal cost, and operating-system overhead are out of scope unless they are demonstrably triggered or amplified by plugin behavior.
- **No automated test harness exists**: verification is manual via the OBS GUI, FPS overlay tools (for hitch detection), and Task Manager / Resource Monitor (for CPU/memory). Same constraint as features 001–003.
- **Per-slot independence** is preserved unconditionally — same constraint as feature 003. No "shortcut" optimization (e.g., shared dock state, shared stats sampler) that would compromise the plugin's per-slot guarantee is acceptable.
- **Constitution Principle IV** (UI/Logic Separation) is preserved unconditionally — UI improvements must keep slot.cpp / manager.cpp free of direct Qt-widget calls. Any UI optimization that requires logic-layer changes routes through the existing thread-safe accessors (`get_dock()`, `QMetaObject::invokeMethod`, etc.).
- The 1Hz stats poll is the most likely first finding (running continuously regardless of recording state is a known idle-cost overshoot). Other findings will emerge from the audit; the spec doesn't prejudge them.

## Resolved Clarifications

- **Scope**: bounded to non-recording-pipeline subsystems (`src/ui-dock.cpp`, `src/ui-slot-editor.cpp`, `SlotManager` lifecycle, `SceneSlot` non-pipeline state, save/load, stats poll, frontend event callbacks, idle behavior).
- **Bar**: process-oriented (every audit finding has a documented disposition; every closeable finding is closed). Numeric targets are institutional memory.
- **What counts as a "finding"**: any plugin behavior that does measurable work beyond what's needed for correctness in its subsystem, OR any user-perceptible delay attributable to plugin code in the actions enumerated in FR-001.
