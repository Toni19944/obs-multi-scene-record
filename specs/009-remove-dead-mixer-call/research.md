# Research: Remove Dead obs_output_set_mixers Calls

**Branch**: `009-remove-dead-mixer-call` | **Date**: 2026-05-28

## R-001: OBS API Contract for obs_output_set_mixers

**Decision**: `obs_output_set_mixers` is exclusively for raw (non-encoded) outputs. Calling it on an encoded output is a no-op that logs a warning.

**Rationale**: OBS `output.c` explicitly checks `(output->info.flags & OBS_OUTPUT_ENCODED)` and early-returns with a warning when the flag is set. Both `ffmpeg_muxer` and `replay_buffer` are encoded output types. The function sets a mixer bitmask used only by raw outputs to select which audio mixers to pull PCM data from — encoded outputs receive pre-encoded audio via `obs_output_set_audio_encoder` instead.

**Alternatives considered**: None — the API contract is unambiguous.

## R-002: Call Site Inventory

**Decision**: Exactly two call sites exist in the plugin, both in `src/slot.cpp`:
- Line 983: `obs_output_set_mixers(rec_out_, cfg_.audio_tracks);` — recording output
- Line 1072: `obs_output_set_mixers(replay_out_, cfg_.audio_tracks);` — replay buffer output

**Rationale**: Confirmed via `rg obs_output_set_mixers` across the entire repository. No other source files, headers, or build scripts reference this function.

**Alternatives considered**: N/A — exhaustive search.

## R-003: Audio Track Routing Correctness Without the Mixer Call

**Decision**: Audio track routing is fully handled by the `obs_output_set_audio_encoder` loop that immediately precedes each removed call. No additional routing mechanism is needed.

**Rationale**: Each audio encoder is bound to its mixer at construction time via `obs_encoder_set_audio(aenc, main_audio)`. The `obs_output_set_audio_encoder(out, aencs_[i], selected_tracks_[i])` call attaches each encoder at the correct output index. This is the standard and complete mechanism for encoded outputs — the mixer bitmask set by `obs_output_set_mixers` is never consulted by the encoded output path.

**Alternatives considered**: Investigated whether `obs_output_set_mixers` might serve as a fallback or secondary routing path for encoded outputs. Confirmed it does not — OBS discards the call entirely.

## R-004: Historical Provenance

**Decision**: The calls are a leftover from the pre-track-aware era, predating feature 003's audio-routing fix.

**Rationale**: Feature 003 (`006-cqp-mismatch`) introduced the track-aware `obs_output_set_audio_encoder` loop with explicit `selected_tracks_[i]` indexing. The original code likely called `obs_output_set_mixers` as a naive attempt at track routing (mimicking raw output patterns). When the correct encoder-based routing was added, the now-redundant mixer call was not cleaned up.

**Alternatives considered**: N/A — historical investigation only.

## R-005: Side Effect Analysis

**Decision**: Removal is side-effect-free. No state change occurs from either call today.

**Rationale**: OBS `output.c` returns immediately after logging the warning when `OBS_OUTPUT_ENCODED` is set. The `output->mixer_mask` field is never written. No downstream code path in the plugin or in OBS reads the mixer mask for encoded outputs. No settings serialization, UI display, or test infrastructure depends on this value.

**Alternatives considered**: Checked whether any OBS callback or signal might reference the mixer mask on encoded outputs — none do.
