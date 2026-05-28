# Data Model: Remove Dead obs_output_set_mixers Calls

**Branch**: `009-remove-dead-mixer-call` | **Date**: 2026-05-28

## Entities

No new entities, fields, or relationships are introduced or modified by this change.

## Affected State

| State | Owner | Change |
|-------|-------|--------|
| `rec_out_` (`obs_output_t*`) | `SceneSlot` | No change — the `obs_output_set_mixers` call was a no-op on this encoded output |
| `replay_out_` (`obs_output_t*`) | `SceneSlot` | No change — same as above |
| `cfg_.audio_tracks` (bitmask) | `SlotConfig` | Read-only; no longer passed to a dead call |
| `aencs_` (audio encoder vector) | `SceneSlot` | Unchanged — continues to be attached via `obs_output_set_audio_encoder` |
| `selected_tracks_` (track index vector) | `SceneSlot` | Unchanged — continues to drive encoder-to-output mapping |

## Invariants Preserved

- Each audio encoder is attached to its output at the track index matching the encoder's mixer, via `obs_output_set_audio_encoder(out, aencs_[i], selected_tracks_[i])`.
- No mixer bitmask is set on encoded outputs (this was already the effective state, since OBS rejected the call).
