# Feature Specification: Performance parity with OBS native recording

**Feature Branch**: `003-perf-parity-audit`

**Created**: 2026-05-19

**Status**: Draft

**Input**: User description: "I received reports of fps drops in game while using this plugin compared to using OBS native recording. Investigate memory leaks and other performance related caveats in the code. Read OBS source code at `D:\Programs\Tools\obs-dev-kit\obs-studio` to find differences between OBS's own recording system and this plugin."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - In-game FPS parity with OBS native recording (Priority: P1)

A gamer running a 3D title at their normal settings turns on this plugin to record one slot at the same resolution/framerate/encoder they would use with OBS's built-in "Start Recording". Their in-game FPS reflects only the irreducible cost of the plugin's per-slot independent rendering — the cost of letting that one slot record its configured scene regardless of what OBS's program is currently showing. Every other source of overhead — anything plugin-specific that does NOT serve per-slot scene independence — has been closed.

**Why this priority**: This is the reported, user-felt symptom. The plugin's identity is **per-slot scene independence** — each slot records its configured scene regardless of program changes. That independence carries an inherent per-slot rendering cost (one extra GPU compositing pass per slot) that OBS native does NOT pay (OBS native records whatever is in program, so it reuses the one rendering pass already happening). This cost is the price of the plugin's feature and is **not** something we will trade away by hijacking the program's video output. What we can and must do is close every OTHER source of overhead — hardcoded format/scale/conversion settings in the per-slot pipeline that diverge from OBS's main config without reason, and anything else the audit identifies that doesn't serve per-slot independence.

**Independent Test**: Pick a stable, FPS-bound game scenario (locked benchmark, demo, replay) and run it 5+ times in each of three conditions: (a) no recording, (b) OBS native recording with identical encoder/resolution/framerate settings, (c) this plugin recording one slot with identical settings. Take the median across runs to suppress run-to-run noise. Two deltas inform the assessment:
- **Inherent per-slot cost**: median (b) − median (c). This is the extra rendering pass; expected to be non-zero and is accepted as the price of per-slot independence.
- **Avoidable overhead**: any FPS cost in (c) beyond what's explained by the inherent per-slot cost. The bar is "as close to zero avoidable overhead as the architecture allows, with every closeable item from the audit closed."

**Acceptance Scenarios**:

1. **Given** the audit has identified an architectural difference between this plugin's recording pipeline and OBS native's, **When** the user reviews its disposition, **Then** the difference is either (a) closed in this feature OR (b) accompanied by documented evidence that closure would sacrifice per-slot scene independence, conflict with another requirement, libobs API constraint, or has empirically zero contribution to overhead. Per-slot independence is the only feature that justifies a (b) disposition by itself; everything else requires positive evidence.
2. **Given** a single-slot recording with x264 CBR at matched settings, **When** the user runs the benchmark, **Then** the inherent per-slot cost (b − c) is measured and recorded as institutional memory for future regression detection — it is not pass/fail, but it must not GROW between this feature and any future change.
3. **Given** the same setup, **When** the user inspects the plugin's per-slot video pipeline configuration, **Then** every parameter that does not need to differ from OBS main (output format, scale type, GPU conversion, colorspace, range) matches the user's main video info exactly — i.e., the plugin imposes no avoidable extra conversions on the per-slot path.
4. **Given** any encoder family (x264 / NVENC / AMF / QSV), **When** the user runs the comparison, **Then** acceptance scenarios 2 and 3 hold across all four encoder paths.

---

### User Story 2 - Long recording sessions do not leak (Priority: P2)

A user records a multi-hour session (single slot, ordinary settings). The plugin's resident memory grows only with what's intrinsic to the recording — the live encoder + I/O buffers — and not with anything that accumulates per second or per frame. After the slot stops, memory drops back to the baseline plus any unavoidable cached objects.

**Why this priority**: Memory leaks rarely show up in short test sessions but compound over long recordings. They eventually crash OBS or force the user to restart between sessions. This is less acutely visible than FPS drops but no less corrosive to trust.

**Independent Test**: Start a single slot recording at the user's typical settings. Let it run for 4 hours, occasionally interacting with the slot (open the editor, refresh stats). Sample resident memory once per minute. Confirm: (a) memory plateaus within the first ~5 minutes, (b) does not grow more than ~50 MB above the plateau over the rest of the session, (c) stops cleanly and releases back near pre-record baseline.

**Acceptance Scenarios**:

1. **Given** one slot recording continuously for 4 hours, **When** the user samples plugin-attributable resident memory, **Then** memory does not grow by more than 50 MB above its first-5-minute plateau.
2. **Given** the slot is stopped after the 4-hour run, **When** the user samples memory 30 seconds later, **Then** the memory returns to within ~30 MB of the pre-recording baseline.
3. **Given** the user toggles a slot's recording start/stop 50 times in a row, **When** they sample memory after, **Then** memory does not exhibit a per-cycle leak (growth is no more than what one running encoder accounts for).

---

### User Story 3 - Plugin overhead is documented and audit-able (Priority: P3)

A maintainer (the project owner) has a written record of where this plugin's recording pipeline differs from OBS's native recording pipeline, and which of those differences contribute to overhead. Future code changes can be evaluated against this document — "did we just reintroduce a known divergence?" — and a future user-reported regression has a concrete starting place.

**Why this priority**: Solving the FPS-parity problem once without documenting the *why* leaves the next maintainer (or the next regression) without a map. This is institutional-memory work — important but not what the player feels in their game.

**Independent Test**: A `research.md` or comparable document exists under `specs/003-perf-parity-audit/` listing each implementation difference between this plugin's recording pipeline and OBS native (at `D:\Programs\Tools\obs-dev-kit\obs-studio`). For each difference, the document states: what OBS does, what this plugin does, whether it adds overhead/risk, and the disposition (fixed in this feature, deliberately kept, or accepted as known cost with rationale).

**Acceptance Scenarios**:

1. **Given** the feature has shipped, **When** a maintainer reads `research.md`, **Then** they can list every architectural difference between the plugin and OBS native recording without re-reading both source trees.
2. **Given** a future PR proposing a change to the plugin's recording pipeline, **When** the maintainer reviews it, **Then** the change can be cross-checked against this document to flag re-introduction of any closed gap.

---

### Edge Cases

- **Replay buffer enabled**: the recording pipeline plus the replay output is the user's worst-case workload for both FPS and memory. Both SC-001 (FPS) and SC-002 (memory) MUST hold in this configuration.
- **Multiple slots running simultaneously**: the plugin's value is recording multiple slots at once; if running two slots doubles overhead vs OBS native (which can only record one), that's the expected cost. Parity here is per-slot — i.e., one plugin slot should cost about the same as one OBS native recording.
- **Shared encoder context (one slot referencing another's encoder via `shared_encoder_slot_id`)**: a sharing slot reuses the owner's video pipeline. Its added overhead vs a single OBS native recording should be near zero (only an additional output + audio encoders, no second render pipeline). Verify this empirically.
- **Hotkey-initiated start vs dock-button start**: now that hotkey-triggered start posts a dock refresh (feature 002), confirm that path doesn't introduce additional per-second overhead. (One refresh per state transition only — not per frame.)
- **Scene-collection switch during a recording**: the existing flow stops all slots and unregisters hotkeys on `*_CHANGING`, then rebuilds on `*_CHANGED`. Memory MUST return to baseline after the round-trip, not accumulate per switch.
- **Encoder fallback (e.g., NVENC -> x264 CBR)**: the encoder-fallback path is a legitimate user-visible event and not part of normal-case performance. SC-001 is measured in the post-fallback steady state, not during the fallback transition.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The plugin's per-slot recording pipeline MUST be free of *avoidable* overhead. "Avoidable" means: any parameter in the per-slot pipeline configuration (video output format, scale type, GPU conversion, colorspace, range, FPS, dimensions, etc.) that does not need to differ from OBS's main video pipeline (`obs_get_video_info`) MUST match the main pipeline's value rather than being hardcoded to a different value. Per-slot independence — the requirement that a slot records its configured scene regardless of program — IS NOT considered avoidable; the per-slot rendering cost it implies is the price of the feature.
- **FR-002**: For every architectural difference between this plugin and OBS native enumerated by the audit (FR-008), the disposition MUST be either "closed in this feature" OR "accepted as the price of per-slot scene independence / libobs constraint / conflicting requirement / measured zero contribution." No undocumented divergence.
- **FR-003**: FR-001 MUST hold across software (x264), NVIDIA (NVENC), AMD (AMF), and Intel (QSV) encoder paths — i.e., no encoder family gets a special divergence the audit didn't catch.
- **FR-004**: With one slot recording continuously for 4 hours at default settings, the plugin's resident memory MUST NOT grow by more than 50 MB above its first-5-minute steady-state plateau.
- **FR-005**: After stopping a long-running recording, memory MUST return to within ~30 MB of the pre-recording baseline within 30 seconds.
- **FR-006**: 50 consecutive start/stop cycles on a single slot MUST NOT exhibit a per-cycle memory leak. The post-50-cycle resident memory MUST be no more than one-running-encoder above the pre-cycle baseline.
- **FR-007**: Scene-collection switch round-trips (away → back) MUST NOT accumulate per-switch memory. After two round-trips, memory MUST be within ~20 MB of the pre-first-switch baseline.
- **FR-008**: The audit MUST identify every architectural difference between this plugin's recording pipeline and OBS native's recording pipeline (as found in `D:\Programs\Tools\obs-dev-kit\obs-studio`). The minimum surface area covered: scene → view/canvas → video output → video encoder → output muxer; the audio mix path; encoder settings application; output start/stop lifecycle; signal handling.
- **FR-009**: For each identified difference, the audit MUST classify it as one of: (a) **closed in this feature** — the plugin's pipeline is brought into alignment with OBS native, OR (b) **accepted as the price of per-slot scene independence** — the divergence is required to give each slot its own independent scene rendering, OR (c) **accepted as irreducible for another documented reason** — a cited public libobs API constraint that blocks the alignment, a conflicting feature requirement, or empirical measurement showing the difference contributes zero overhead. Disposition (b) applies specifically to the per-group `obs_view_t + video_t` pipeline that exists so the slot can render its configured scene independent of OBS's program. Disposition (c) requires positive evidence beyond per-slot independence.
- **FR-010**: No regression to existing user-visible behavior. All four prior-feature outcomes (per-slot record/save replay, hotkey-driven toggle, dock-state-on-hotkey, settings-grouped hotkeys) MUST continue to pass their existing quickstart procedures after this feature ships.

### Key Entities

- **Recording pipeline**: the chain of OBS components that turn a scene into a muxed file on disk — scene source → view/canvas → video output → video encoder → muxer. The plugin instantiates its own variant of this chain per shared-encoder group; OBS native uses the main mix's pipeline. The differences between the two are the audit's primary subject.
- **Overhead**: the FPS delta between "recording with the plugin" and "recording with OBS native at matched settings" — the share of frame-time that is the plugin's responsibility, not the encoder's or the game's.
- **Steady-state memory**: the resident memory level reached after the first ~5 minutes of a continuous recording, used as the leak-detection baseline.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every audit finding catalogued under FR-008 has an FR-009 disposition. Every finding with disposition (a) ("closed in this feature") is fixed in this feature's commits. Every (b) and (c) carries documented evidence. No finding remains in an undocumented state.
- **SC-002**: A 4-hour continuous recording adds no more than 50 MB above the 5-minute steady-state plateau, on default settings.
- **SC-003**: Post-stop memory recovery returns to within 30 MB of the pre-recording baseline within 30 seconds.
- **SC-004**: 50 consecutive start/stop cycles do not show a per-cycle memory leak (no growth attributable to the cycles themselves).
- **SC-005**: The audit document lists every plugin↔OBS-native recording-pipeline difference with a disposition (fixed / deliberately kept / accepted) and a one-sentence rationale.
- **SC-006**: Zero regressions in the quickstart tests of prior shipped features (001 hotkey registration, 002 dock UI sync).
- **SC-007**: Zero plugin crashes attributable to recording-pipeline changes across a representative test workload (single slot 4-hour run, multi-slot 30-minute run, 50-cycle start/stop, three scene-collection round-trips, full hotkey lifecycle).

## Assumptions

- The user's "FPS drops" report concerns in-game framerate while a slot is actively recording — not OBS UI responsiveness, encoder kbps, or replay-buffer latency.
- "OBS native recording" means the OBS Studio built-in "Start Recording" function configured with the same encoder, resolution, framerate, bitrate, and audio tracks as the plugin's slot under test.
- **Per-slot scene independence is non-negotiable.** Each slot records its configured scene regardless of what OBS's program is currently showing. This is the plugin's identity and the reason it exists; sacrificing it to chase OBS-native FPS parity defeats the purpose. The per-group `obs_view_t + video_t` pipeline that delivers this independence has an inherent per-slot rendering cost that OBS native does not pay (OBS native records what's already in the program render). This cost is **accepted as irreducible** under FR-009 disposition (b).
- **What we DO close**: every OTHER source of overhead identified by the audit — hardcoded video-info parameters that diverge from OBS's main without reason, redundant inc_showing patterns when conditions allow them to be skipped, any other plugin-specific cost that does not serve per-slot independence.
- **FPS measurements are institutional memory, not a pass/fail gate.** The benchmark in [quickstart.md](#) compares plugin-with-recording to OBS-native-with-recording so a future regression can be detected ("did the delta grow?"), but absolute OBS-native parity in a single-slot benchmark is not the bar (it would require giving up the plugin's identity).
- The 4-hour leak-test duration is chosen as "typical long session." 24-hour soak-testing would be more conclusive but is impractical to run repeatedly during development; if a leak slips through 4 hours, a follow-up spec can extend the bar.
- The 50 MB / 30 MB memory tolerances are defaults; tighter targets are easy to re-spec later if early measurements show the plugin is already well-inside them.
- The reference OBS source tree at `D:\Programs\Tools\obs-dev-kit\obs-studio` is at the same OBS version the plugin targets (31.1.1 per `buildspec.json`). If the user later upgrades the OBS target, the audit document should be re-validated against the new source.
- The audit's "differences" are bounded to the recording pipeline (FR-008's enumerated surface area). Unrelated subsystems — Qt UI rendering, settings serialization, encoder selection UI — are out of scope unless they are demonstrated to contribute to FPS overhead or memory growth.
- Constitution Principle V (Encoder Robustness & Graceful Fallback) is not relaxed — the x264/CBR fallback path remains intact and is not part of the overhead audit (it's a correctness path, not a perf path).
- Findings that contribute to overhead but cannot be safely fixed (e.g., libobs API constraints) are documented and accepted; SC-005's "disposition" field is the channel for that.

## Resolved Clarifications

- **Audit scope**: bounded to the recording pipeline (scene → muxer + audio mix + encoder lifecycle) per FR-008. Out-of-scope subsystems documented in Assumptions.
- **Per-slot scene independence is non-negotiable**: this is the plugin's identity. The per-group `obs_view_t + video_t` pipeline that delivers it carries an inherent per-slot rendering cost that OBS native does not pay. This cost is **accepted as irreducible** under FR-009 disposition (b) — closing it would require deleting the feature.
- **Fix scope**: every plugin↔OBS-native difference that contributes overhead AND can be closed without sacrificing per-slot independence MUST be closed. The known close candidates are the hardcoded video-info parameters in the per-slot pipeline (output format, scale type, GPU conversion) — currently set to fixed values rather than matching the user's OBS main configuration. Anything else the audit surfaces that meets this criterion is also in scope.
- **Bar**: process-driven, not number-driven. Every audit finding has a documented disposition. Every (a)-disposition finding is fixed. Memory bounds (50 MB / 30 MB over 4 hours) hold. FPS measurements are institutional memory for future regression detection, not pass/fail acceptance.
