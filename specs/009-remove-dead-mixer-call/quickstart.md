# Quickstart: Remove Dead obs_output_set_mixers Calls

**Branch**: `009-remove-dead-mixer-call` | **Date**: 2026-05-28

## What to Change

Delete two lines in `src/slot.cpp` within `SceneSlot::setup_outputs()`:

1. **Line 983** (recording output setup): `obs_output_set_mixers(rec_out_, cfg_.audio_tracks);`
2. **Line 1072** (replay buffer setup): `obs_output_set_mixers(replay_out_, cfg_.audio_tracks);`

No other files need changes.

## How to Verify

1. Build the plugin.
2. Configure a slot with multi-track audio (e.g., tracks 1 and 3) and replay enabled.
3. Start the slot.
4. Check the OBS log — confirm no `Tried to use obs_output_set_mixers on an encoded output` warning appears.
5. Record a short clip and save a replay. Open both files in a media inspector and confirm the expected audio tracks are present.

## Build Commands

```powershell
# Windows (from repo root)
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

## CHANGELOG Entry

Add under the next release heading in `CHANGELOG.md`:

```
- fix: remove dead `obs_output_set_mixers` calls on encoded outputs that produced OBS warnings on every slot start
```
