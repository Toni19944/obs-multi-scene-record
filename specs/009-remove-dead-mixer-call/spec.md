# Feature Specification: Remove Dead obs_output_set_mixers Calls

**Feature Branch**: `009-remove-dead-mixer-call`

**Created**: 2026-05-28

**Status**: Draft

**Input**: User description: "obs_output_set_mixers called on encoded outputs produces OBS warning and is dead code"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Clean Slot-Start Logs (Priority: P1)

A plugin user starts one or more recording/replay slots. The OBS log for each slot start contains only genuine diagnostic information — no spurious warnings from rejected API calls. When the user later needs to debug an audio-routing or replay-buffer problem, every warning in the log represents a real issue.

**Why this priority**: The warning fires on every slot start for every configured slot, creating persistent log noise that actively misleads users and support helpers triaging audio issues. Removing it is the highest-value outcome.

**Independent Test**: Start any slot (recording-only, replay-only, or both) and verify the OBS log contains no line matching `Tried to use obs_output_set_mixers on an encoded output` for that slot's outputs.

**Acceptance Scenarios**:

1. **Given** a slot configured for recording with multi-track audio, **When** the slot starts, **Then** the OBS log does not contain `Output 'rec_out_<id>': Tried to use obs_output_set_mixers on an encoded output`.
2. **Given** a slot configured with replay buffer enabled, **When** the slot starts, **Then** the OBS log does not contain `Output 'replay_out_<id>': Tried to use obs_output_set_mixers on an encoded output`.
3. **Given** a slot configured for both recording and replay, **When** the slot starts, **Then** neither warning appears.

---

### User Story 2 - Audio Track Routing Preserved (Priority: P1)

A plugin user records with a multi-track audio configuration (e.g., tracks 1 and 3 selected). After the dead-code removal, the recorded file contains exactly the same audio tracks, in the same layout, as before the change. The replay buffer likewise captures the correct audio.

**Why this priority**: Equal priority to US-1 because correctness preservation is the prerequisite for the removal being safe. The change must be proven side-effect-free.

**Independent Test**: Record a short clip with a non-contiguous track selection (e.g., tracks 1 and 3). Open the output file in a media inspector and verify it contains exactly two audio streams mapped to the expected tracks.

**Acceptance Scenarios**:

1. **Given** a slot selecting audio tracks {1, 3}, **When** a recording completes, **Then** the output file contains audio streams on tracks 1 and 3 with correct content.
2. **Given** a slot selecting audio tracks {1, 3} with replay enabled, **When** a replay is saved, **Then** the replay file contains audio streams on tracks 1 and 3 with correct content.
3. **Given** a slot selecting a single audio track {1}, **When** a recording completes, **Then** the output file contains one audio stream on track 1.

---

### Edge Cases

- What happens when all six audio tracks are selected? The `obs_output_set_audio_encoder` loop handles all tracks; removal of the mixer call has no effect.
- What happens with replay-only mode (no recording output)? Only the replay output path is exercised; the recording-side removal is not reached. The replay-side removal alone must be clean.
- What happens with recording-only mode (replay disabled)? Only the recording output path is exercised; the replay-side removal is not reached.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The plugin MUST NOT call `obs_output_set_mixers` on any encoded output (`ffmpeg_muxer` or `replay_buffer`).
- **FR-002**: The plugin MUST continue to call `obs_output_set_audio_encoder` for each selected audio track on both the recording and replay-buffer outputs, preserving the existing per-track routing logic.
- **FR-003**: No call site for `obs_output_set_mixers` may remain anywhere in the plugin source after this change (the two identified sites are the only sites; no new sites should be introduced).
- **FR-004**: The change MUST be confined to `src/slot.cpp` — no header, build-system, or other source file changes required.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Zero instances of the string `Tried to use obs_output_set_mixers on an encoded output` appear in OBS logs during any slot start sequence.
- **SC-002**: Audio track layout in recorded and replayed files is byte-identical to pre-change behavior for any track selection configuration.
- **SC-003**: Zero remaining call sites for `obs_output_set_mixers` in the plugin source tree after the change.

## Assumptions

- The two call sites at `src/slot.cpp` (recording output setup and replay-buffer output setup) are the only locations in the plugin that invoke `obs_output_set_mixers`. This has been confirmed via grep.
- The `obs_output_set_audio_encoder` loop immediately preceding each removed call is the sole and correct mechanism for audio track routing on encoded outputs, as established by the OBS API contract.
- This change is a leftover from a pre-track-aware era; the track-aware `obs_output_set_audio_encoder` loop introduced in feature 003 made the mixer call redundant but did not remove it at the time.
- No downstream consumer (settings serialization, UI, test harness) reads or depends on the mixer mask set via `obs_output_set_mixers` on these outputs — OBS itself discards the call with a warning and performs no state change.
