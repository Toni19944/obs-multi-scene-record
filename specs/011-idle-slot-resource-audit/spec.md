# Feature Specification: Idle-State Background Resource Audit for Configured-but-Not-Running Slots

**Feature Branch**: `011-idle-slot-resource-audit`

**Created**: 2026-05-29

**Status**: Draft

**Input**: User description: "Audit idle-state background resource usage for configured-but-not-running slots"

## Clarifications

### Session 2026-05-29

- Q: Should this audit apply fixes in this feature for idle over-retention it finds, or only report? → A: **Hybrid** — fix **confirmed correctness leaks** (a resource that should be released but isn't) in this feature; **document but defer** "acceptable-but-reducible" costs (e.g., stats-timer interval tuning, making the refresh rate configurable) to a follow-up feature.
- Q: What is the pass/fail bar for plugin work at true idle (no slot running)? → A: **Zero scheduled work** — at true idle the plugin performs no periodic work (the stats refresh timer stays paused); the audit confirms the feature-004 gate still holds and treats any always-on idle tick as a finding.

## User Scenarios & Testing *(mandatory)*

A user of this plugin typically configures several recording slots (one per scene/camera angle/quality preset) but only records with a subset of them at any moment. The remaining slots sit **configured but not running**: they exist in the slot list, their hotkeys work, but they are not recording. This audit verifies that such idle slots impose the smallest possible standing cost on the host machine, and closes any case where an idle slot quietly keeps resources alive that belong only to a running recording.

This is the next entry in the plugin's audit series (003 recording-pipeline parity, 004 idle-CPU pass, 005 general perf/stability). It is scoped specifically to **per-slot resource retention while a slot is not running**.

### User Story 1 - Idle slots impose near-zero standing cost (Priority: P1)

A user configures many slots (e.g., ten covering different scenes and quality presets) but records with only one or two at a time. The slots that are configured but not running must not consume meaningful CPU, memory, GPU/render time, file handles, or OBS pipeline objects, and must not keep their scenes or the sources inside them active in the background.

**Why this priority**: This is the entire point of the feature. Users keep many slots ready precisely so they can start any of them instantly; if each idle slot carried recording-weight cost, configuring more than a couple of slots would tax the machine for the whole time OBS is open even when nothing is recording. A silent over-retention here (e.g., a stopped slot leaving its scene "shown" so its cameras keep running) is exactly the kind of cost users cannot see but pay for continuously.

**Independent Test**: Configure ten slots covering distinct scenes, start none of them, and observe the plugin's standing resource footprint. The footprint must reflect only lightweight per-slot bookkeeping (configuration + input bindings + the per-slot Settings-grouping object) and must not include any recording-pipeline resources; it must not scale with recording-weight cost as more idle slots are added.

**Acceptance Scenarios**:

1. **Given** ten slots are configured and none is running, **When** the host's resource usage attributable to the plugin is measured over time, **Then** it shows no per-second background work and no recording-pipeline memory, GPU surfaces, encoders, outputs, or replay buffers.
2. **Given** a slot whose scene contains an active capture source (camera/window/display), **When** that slot is configured but not running, **Then** the plugin does not keep that scene's sources active/rendering on the slot's behalf.
3. **Given** the plugin is loaded with zero slots, **When** the same plugin is loaded with ten configured-but-not-running slots, **Then** the difference in standing CPU is indistinguishable and the difference in memory is a small, bounded per-slot constant.

---

### User Story 2 - Stopping a slot fully releases its runtime resources (Priority: P2)

A user starts a slot, records, then stops it. After stopping, the slot returns to the idle baseline: every resource acquired for recording is released, leaving only the lightweight idle bookkeeping. In particular, the scene the slot was recording is no longer kept active on the plugin's behalf.

**Why this priority**: A configured-but-not-running slot is most commonly one that *was* running and has been stopped. If a stop does not fully release what start acquired, "idle" slots would carry residual recording cost — the worst form of the problem because it only appears after real use. This is the bridge between "running" and "idle"; without it, User Story 1's guarantee cannot hold in practice.

**Independent Test**: Record the idle baseline for a single configured slot, start it, let it record briefly, stop it, then re-measure after a short settle period. Every measured resource class returns to its pre-start value.

**Acceptance Scenarios**:

1. **Given** a slot was started and is recording, **When** it is stopped, **Then** within a bounded settle time its scene source is no longer kept active, its video/audio encoders and outputs are gone, and its replay-buffer memory (if any) is freed.
2. **Given** a slot that shares an encoder group with another slot, **When** the sharing slot stops but a group peer is still running, **Then** the shared pipeline remains alive for the running peer and is not torn down by the stopped slot; the stopped slot itself retains none of it.
3. **Given** a slot that shares an encoder group, **When** the last running member of the group stops, **Then** the shared pipeline (scene showing, view, video, encoder) is fully released and no member keeps it alive while idle.

---

### User Story 3 - Repeated cycles do not accumulate idle cost (Priority: P3)

A user repeatedly starts and stops slots, edits them, and renames them over a long OBS session. The idle footprint after each cycle must return to baseline; nothing may accumulate cycle over cycle.

**Why this priority**: Leaks that only manifest after many cycles are subtle and erode the P1 guarantee over a long session. Renames are called out specifically because the per-slot Settings-grouping object is destroyed and recreated on rename, a classic place for a per-rename handle leak.

**Independent Test**: Run many start/stop cycles and several rename/edit cycles on a configured slot, returning it to idle each time, and confirm the idle footprint (handles, memory, OBS object counts) does not grow monotonically.

**Acceptance Scenarios**:

1. **Given** a configured slot, **When** it is started and stopped many times in succession, **Then** the idle footprint after the last cycle matches the idle footprint after the first.
2. **Given** a configured slot, **When** it is renamed and re-edited several times while idle, **Then** no OBS objects (including the Settings-grouping object) accumulate and input bindings are preserved.
3. **Given** a scene-collection switch that rebuilds the slot list, **When** the previous slots are torn down, **Then** their idle resources are released and not orphaned.

---

### Edge Cases

- **Never-started slot**: A slot loaded from saved data that the user has never started must hold only idle bookkeeping — no pipeline resources are created merely by existing.
- **Replay-only slot at idle**: A slot configured as replay-only and not running must not hold any replay-buffer memory or output while idle.
- **Idle sharer with a running owner (and vice versa)**: The shared pipeline cost belongs to the group while at least one member runs; an idle member of a live group must not be charged a *second* copy, and an idle slot whose group has no running member must hold none of it.
- **Scene deleted while the slot is idle**: A configured slot whose referenced scene no longer exists must not leak or keep a phantom scene/source active.
- **Slot added or edited during another slot's active recording**: Configuring a new idle slot while others record must not perturb or duplicate running pipelines, and the new slot must hold only idle bookkeeping.
- **Stop triggered externally (disk full, encoder failure)**: An error-driven stop must release resources to the same idle baseline as a user-initiated stop.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The audit MUST enumerate every class of background resource a configured-but-not-running slot can hold while idle, covering at minimum: memory, scheduled/periodic work and threads, GPU/render activity, OBS pipeline objects (scene source and its "showing" state, view, video output, video encoder, audio encoders, recording/replay outputs, replay-buffer memory), file/OS handles, input (hotkey) registrations, host per-frame/compositing callbacks fired against registered-but-inactive outputs, and platform-specific device idle wakeups (e.g., GPU/D3D11 on Windows).
- **FR-002**: A configured-but-not-running slot MUST NOT keep its scene source in a "showing"/active state while idle, so that the scene's video sources (cameras, captures) are not kept rendering on the slot's behalf when it is not recording.
- **FR-003**: A configured-but-not-running slot MUST NOT hold a video encoder, view, or video output while idle. These belong to the refcounted shared encoder context, which MUST exist only while at least one consumer in its group is running and MUST be released when the last consumer stops.
- **FR-004**: A configured-but-not-running slot MUST NOT hold recording or replay outputs, audio encoders, or replay-buffer memory while idle.
- **FR-005**: At true idle (no slot running) the plugin MUST perform zero scheduled/periodic work. No per-slot timer or background thread may run while a slot is idle, and the global stats refresh timer MUST be paused when no slot is running (carry-forward of feature 004 FR-002 / feature 005 FR-006). The audit MUST confirm this gate still holds and MUST treat any always-on idle tick as a finding (a confirmed leak to fix).
- **FR-006**: The standing per-slot idle footprint MUST be bounded and MUST NOT grow with recording-weight cost as the number of configured slots increases. The audit MUST identify the minimal set of objects an idle slot legitimately retains (its configuration, its registered hotkeys, and at most one inert per-slot Settings-grouping object) and MUST justify each retained object as necessary for correctness or close it.
- **FR-007**: Stopping a running slot MUST return that slot's footprint to the pre-start idle baseline, releasing every resource that start acquired. No recording-pipeline resource class may remain retained by the slot after it stops.
- **FR-008**: Repeated start/stop, edit, rename, and scene-collection-rebuild cycles MUST NOT accumulate idle resources. The destroy-and-recreate of the per-slot Settings-grouping object on rename MUST NOT leak OBS objects or input bindings.
- **FR-009**: The idle guarantees MUST hold across all slot variants and lifecycle states: a slot never started, a slot started then stopped, a replay-only slot, and a slot that owns or shares an encoder group.
- **FR-010**: The audit MUST produce findings documented with concrete code references, each carrying exactly one of three dispositions: **CLOSE** (a confirmed correctness leak — a resource that should be released but isn't — fixed in this feature), **ACCEPT** (a retention that is the minimum necessary for correctness, with a stated reason), or **DEFER** (an acceptable-but-reducible cost documented with a recommendation and handed to a follow-up feature). No identified idle resource class may be left with an "unknown" disposition.
- **FR-011**: Any fix applied as a result of a finding MUST preserve existing correctness guarantees — including thread-safety/lock-ordering and native OBS API compliance — and MUST NOT regress the running-slot recording path or the existing idle-CPU behavior established by features 004 and 005.
- **FR-012**: The audit MUST determine whether the host fires any per-frame or compositing callback against a slot's registered-but-inactive outputs (or its inert Settings-grouping object), and MUST confirm that a configured-but-not-running slot engages none of the host's compositing/encode pipeline. Any inactive output found incurring per-frame work is a confirmed leak to CLOSE.
- **FR-013**: The audit MUST record the dock stats timer's refresh interval (currently fixed at 1 Hz) and how its per-tick cost scales with configured slot count, and MUST assess whether the interval should be made configurable or lowered. Because the timer is paused at true idle (FR-005), this is an acceptable-but-reducible cost: it is documented with a recommendation and DEFERRED to a follow-up — unless the audit finds the timer running while no slot is active, which is a confirmed leak to CLOSE.
- **FR-014**: The audit MUST cross-reference platform-specific idle costs attributable to held-but-unused video pipelines (e.g., GPU/D3D11 device idle wakeups on Windows). If a configured-but-not-running slot is found holding a video pipeline that produces such wakeups, that retention is a confirmed leak to CLOSE (per FR-003 and FR-007).

### Key Entities *(include if feature involves data)*

- **Configured-but-not-running slot (idle slot)**: A slot present in the slot manager whose running state is false — either never started or previously started and stopped. The subject of this audit.
- **Idle resource footprint**: The complete set of resources (memory, scheduled work, GPU/render activity, OBS objects, handles, input bindings) attributable to a slot while it is not running. The audit's unit of measurement.
- **Idle baseline**: The minimal acceptable idle footprint — per-slot configuration, registered hotkeys, and the inert per-slot Settings-grouping object — against which "before start" and "after stop" states are compared. The dock's stats refresh timer is **not** part of this baseline: it is paused at true idle and runs only while at least one slot is active.
- **Shared encoder context**: The refcounted recording pipeline (scene showing, view, video output, video encoder) owned per encoder group, built on the first running consumer and destroyed on the last. Must contribute zero footprint to any slot that is not running unless a group peer is running.
- **Settings-grouping object**: The inert, never-started per-slot object that exists solely to group the slot's hotkeys under its name in the host's Hotkeys settings. A candidate "legitimately retained at idle" object the audit must confirm is minimal and leak-free.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: With any number of configured-but-not-running slots and zero recording, the plugin contributes no per-second background work — its standing CPU is indistinguishable from the plugin loaded with zero slots.
- **SC-002**: With N configured-but-not-running slots, the plugin's standing memory grows by at most a small fixed per-slot constant and contains no recording-pipeline allocations (encoders, output buffers, replay buffers, render targets).
- **SC-003**: After starting then stopping a slot, every measured idle resource SHOULD return to its pre-start value within 5 seconds; up to 10 seconds is acceptable.
- **SC-004**: A capture source (camera/window/display) that appears only in an idle slot's scene is not kept active by the plugin while that slot is not running, verifiable via the source's active/showing state.
- **SC-005**: Performing 100 start/stop cycles on a slot leaves its idle footprint unchanged — no monotonic growth in OBS object counts, OS handles, or memory.
- **SC-006**: Every idle resource class enumerated by the audit (FR-001) carries an explicit CLOSE or ACCEPT disposition with a stated rationale; none is left unresolved.
- **SC-007**: The running-slot recording path and the features 004/005 idle-CPU behavior show no regression after any fixes from this audit (existing acceptance checks for those features still pass).

## Assumptions

- **Definition of "configured-but-not-running"**: A slot that exists in the slot manager (persisted and/or loaded) with its running state false — whether never started or stopped. This is the audit's subject; slots that are actively recording are out of scope except as the "before/after" reference for User Story 2.
- **Hybrid investigate-and-fix scope** (per 2026-05-29 clarification): This feature investigates idle retention, documents every finding with a CLOSE/ACCEPT/DEFER disposition, and fixes only **confirmed correctness leaks** (a resource that should be released but isn't). **Acceptable-but-reducible** costs — e.g., stats-timer interval tuning or making the refresh rate configurable — are documented with a recommendation and DEFERRED to a follow-up feature rather than changed here. It adds no user-facing functionality.
- **Minimal legitimate idle retention**: The expected idle baseline per slot is its configuration, its two registered hotkeys (record + replay-save, intentionally registered while stopped so the toggle works), and the single inert per-slot Settings-grouping object. The audit confirms this set is minimal and that each member is leak-free; if any member proves unnecessary it is closed.
- **Out of scope**: The active recording pipeline already covered by audit 003, and the general non-recording subsystems (slot editor, save/load, frontend callbacks) already covered by audits 004/005, except where they bear directly on per-slot idle retention.
- **Measurement approach**: Verification uses the host's own object/active-state introspection together with standard OS resource tooling (handle/memory counts). No specific third-party profiler is mandated; the criteria are tool-agnostic.
- **Platform**: Primary verification on the build/run platform (Windows); the guarantees are platform-independent in intent, and any platform-specific resource accounting is noted per finding.
