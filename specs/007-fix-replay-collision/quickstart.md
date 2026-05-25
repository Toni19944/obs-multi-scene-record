# Quickstart — Manual Verification

**Feature**: Replay file uniqueness across slots sharing an output directory + truthful replay-save logging

**Branch**: `007-fix-replay-collision` | **Date**: 2026-05-25

This file lists the manual verification procedure for the 007 fix. Manual coverage is **Windows-only** (the maintainer's test environment). macOS / Ubuntu builds get the same code path; correctness on those platforms rests on the platform-agnostic nature of the filename construction and on future community reports — same scoping as feature 006.

Each test references the FRs / SCs / User Stories it exercises (from [spec.md](./spec.md)) and the design decisions it validates (from [research.md](./research.md)). Pre-condition for all tests: build the plugin from this branch (`007-fix-replay-collision`) against OBS 31.1.1 (`buildspec.json`), and load OBS with the plugin enabled.

---

## Test 1 — US1 headline bug: two slots, same dir, same-second save

**Validates**: US1, FR-001, FR-002, FR-010, SC-001 | **References**: research D1, D2, D3, D5

1. Create slot **CamA**: source a scene (any scene works for the test; a static color source is fine), set Output Path to `D:/tmp/replay-007/`, container `mkv`, enable replay buffer, replay length 5 s. Bind the save-replay hotkey to F8.
2. Create slot **CamB**: same scene, same output path `D:/tmp/replay-007/`, same container `mkv`, replay enabled, 5 s buffer. Bind save-replay hotkey to F9.
3. Start both slots from the dock.
4. Wait ~6 seconds for the replay buffers to fill.
5. Press F8 and F9 in rapid succession (within ~100 ms; press one immediately after the other).
6. **Expected**: in `D:/tmp/replay-007/`, two distinct files are present. Filenames are of the shape `CamA_<id6a>_Replay_<ts>.mkv` and `CamB_<id6b>_Replay_<ts>.mkv` where `<id6a> != <id6b>` and the `<ts>` portions may be identical (down to the second) or differ by one second.
7. **Pre-fix regression check**: confirm the bug. Build the previous-revision binary (`git checkout main`, rebuild), repeat steps 1-5. Observe that only one file is in the directory after both hotkeys fire in the same second — confirming the bug. Switch back to `007-fix-replay-collision` and rebuild before continuing.

---

## Test 2 — US1 generalization: three+ slots, same dir, same-second save

**Validates**: US1 Acceptance #2, FR-002, SC-003 | **References**: research D2

1. Repeat Test 1's setup with three slots CamA / CamB / CamC, all pointing at `D:/tmp/replay-007/`, all `mkv`, hotkeys F8 / F9 / F10.
2. Start all three.
3. Wait ~6 seconds.
4. Press F8, F9, F10 in rapid succession.
5. **Expected**: three distinct files in the directory, one per slot. The slot identity components in the filenames differ; the timestamps may all be the same second.

---

## Test 3 — US1 different container, same dir

**Validates**: US1 Acceptance #3, FR-002 | **References**: research D4 (sanitization neutrality), D5

1. Two slots same dir `D:/tmp/replay-007/`, but CamA uses `mp4` and CamB uses `mkv`.
2. Start both, fill the buffer, simultaneous saves.
3. **Expected**: two files, one with `.mp4` extension and one with `.mkv` extension. Pre-007 already supported this case (the extension difference was sufficient); the fix MUST NOT regress it. Confirm both files are present.

---

## Test 4 — US2 attribution by name

**Validates**: US2 Acceptance #1, FR-003, FR-005, SC-002 | **References**: research D2, D5

1. Two slots named "Front Camera" and "Side Camera", same dir, same container.
2. Trigger one Save Replay on each (different seconds is fine; this test is about filename readability, not collision).
3. **Expected**: open the directory in Windows Explorer. The user can identify by reading filenames alone which file is "Front Camera" and which is "Side Camera". Filenames look like `Front Camera_<id6>_Replay_<ts>.mkv` and `Side Camera_<id6>_Replay_<ts>.mkv` — both readable; the recording's continuous filename (if any) for the same slot is `Front Camera_<ts>.mkv` and sorts adjacent to the replay file in the directory listing.

---

## Test 5 — US2 same-name-same-dir collision

**Validates**: US2 Acceptance #3, FR-004 | **References**: research D2, D3

1. Two slots **both named "CamA"** (slot names are not enforced unique). Same output dir, same container, replay enabled on both, both started, both buffers filled.
2. Trigger Save Replay on each within the same second.
3. **Expected**: two distinct files. Filenames `CamA_<id6_first>_Replay_<ts>.mkv` and `CamA_<id6_second>_Replay_<ts>.mkv` differ in the `<id6>` component. Neither file is overwritten.

---

## Test 6 — US2 empty-name fallback

**Validates**: US2 Acceptance #2, FR-003 fallback rule, FR-004a | **References**: research D4

1. One slot with `cfg.name` set to the empty string (clear the Name field in the editor; click Save).
2. Trigger Save Replay.
3. **Expected**: filename is `slot_<id6>_Replay_<ts>.<ext>` — the `"slot"` literal fallback is in place (matching the existing recording-filename fallback at `slot.cpp:99`), and the `<id6>` component still uniquely identifies the slot.

---

## Test 7 — US3 truthful log on success

**Validates**: US3 Acceptance #2, FR-011, FR-012, FR-013, SC-006, SC-007 | **References**: research D1, D7, D8

1. One slot, output dir writable, replay enabled, started, buffer filled.
2. Trigger Save Replay.
3. **Expected**: the OBS log shows two plugin lines in order:
   - `[multi-scene-rec] 'CamA' replay save requested` (LOG_INFO)
   - `[multi-scene-rec] 'CamA' replay save wrote 'D:/tmp/replay-007/CamA_<id6>_Replay_<ts>.mkv'` (LOG_INFO)
4. The OBS muxer's own `[ffmpeg muxer: 'replay_out_<id>'] Wrote replay buffer to '<same-path>'` line appears between the two (or right around them, depending on log flush ordering). The plugin's `wrote` path matches the OBS line's path exactly.
5. Verify by inspecting the directory: the file at the reported path exists.

---

## Test 8 — US3 truthful log on failure (output dir unwritable)

**Validates**: US3 Acceptance #1, US3 Acceptance #3, FR-011, SC-006 | **References**: research D1, D7

1. One slot, output dir `D:/tmp/replay-007-unwritable/`. Create the directory, then revoke write permission to it (Windows Properties → Security → deny Write to your user — or, simpler: configure to `D:/this/path/does/not/exist/`).
2. Replay enabled, started, buffer filled.
3. Trigger Save Replay.
4. **Expected**: the plugin log shows ONLY the `request` line — no `wrote` follow-up line. Specifically:
   - `[multi-scene-rec] 'CamA' replay save requested` (LOG_INFO) appears.
   - No `[multi-scene-rec] 'CamA' replay save wrote ...` line appears anywhere in the subsequent log output for this save.
5. The OBS muxer's own log shows a `Failed to create process pipe` or `Could not write headers for file ...` warning (depending on which mux-thread step fails first). This is the OBS-side signal that the write failed.
6. **Critical**: the plugin log does NOT contain a line matching `replay save OK` or `replay save succeeded` for this save. (This is the entire point of the US3 fix.)

---

## Test 9 — Edge case: slot name contains path-illegal characters

**Validates**: FR-004a, FR-008 | **References**: research D4

1. Slot name set to `Cam<>*A` (contains `<`, `>`, `*`). Output dir writable. Replay enabled, started, buffer filled.
2. Trigger Save Replay.
3. **Expected**:
   - Filename produced is `Cam_A_<id6>_Replay_<ts>.<ext>` (the illegal characters replaced with `_` and the run collapsed).
   - The `request` and `wrote` log lines appear (the save succeeded because the sanitized filename is portable).
   - The slot's stored `cfg.name` on disk is **still** `Cam<>*A` — sanitization is filename-only, not persisted. Verify by reopening the slot editor; the Name field still shows the original characters.
   - **Also**: the continuous recording for this same slot, if started and stopped, would still fail at write time (the recording filename construction at `slot.cpp:96-104` does NOT sanitize per FR-006 / FR-008). This asymmetry is intentional and noted in spec Assumptions; not a regression of this feature.

---

## Test 10 — Regression: pre-fix files co-exist in directory

**Validates**: FR-007, SC-005 | **References**: spec Edge Cases bullet on pre-fix files

1. Manually drop a few files named `Replay_2026-01-15_10-23-45.mkv` (any content, including empty) into the output directory, simulating pre-007 saves.
2. Start a slot pointed at that directory, fill the buffer, trigger Save Replay.
3. **Expected**:
   - The pre-fix files remain untouched (no rename, move, or delete).
   - The new post-007 file is created alongside them with the new format `CamA_<id6>_Replay_<ts>.mkv`.
   - No errors, warnings, or special log lines appear at slot start, slot stop, or save time about the pre-fix files.
   - The directory listing now shows both pre-fix and post-fix shapes; both are valid filenames.

---

## Test 11 — Regression: slot teardown during in-flight save (lock-order / signal-disconnect ordering)

**Validates**: contract § Threading, Constitution Principle III | **References**: research D6

This is the lock-order regression check. It is awkward to reproduce manually but worth confirming for any change to teardown ordering.

1. One slot, replay enabled, started, buffer filled. Output dir writable.
2. Trigger Save Replay (F8). The mux thread starts writing.
3. While the mux is in flight (the muxer takes hundreds of ms to seconds depending on buffer size and disk speed), immediately stop the slot from the dock.
4. **Expected**: no deadlock, no crash, no hang. The stop proceeds normally. Either:
   - **Case A** (mux completes before disconnect): the `saved` signal fires, the `wrote` log line is emitted, then teardown's disconnect returns harmlessly.
   - **Case B** (disconnect returns before mux completes): the disconnect runs synchronously with any in-flight callback (libobs's `signal_handler_disconnect` blocks on the signal mutex until dispatch completes). If the mux is still writing, `obs_output_stop` → `wait_for_output_stop` will let it finish (or terminate it cleanly). Either way, the plugin does not deadlock and does not crash.
5. If a deadlock is observed (Visual Studio debugger shows the main thread waiting on a mutex held by a worker thread that is itself blocked in `signal_handler_disconnect`), the contract has been violated and the implementation needs review.

---

## Verification matrix

| Test | US | FRs validated | SCs validated | Edge cases validated |
|---|---|---|---|---|
| T1 | US1 #1 | FR-001, FR-002, FR-010 | SC-001 | Same-second cross-slot |
| T2 | US1 #2 | FR-002 | SC-001, SC-003 | Three-slot same-second |
| T3 | US1 #3 | FR-002 | SC-001 | Different-container same-dir |
| T4 | US2 #1 | FR-003, FR-005 | SC-002 | Attribution by name |
| T5 | US2 #3 | FR-001, FR-004 | SC-001, SC-002 | Same-name slots |
| T6 | US2 #2 | FR-003 (fallback) | SC-002 | Empty user-facing name |
| T7 | US3 #2 | FR-011, FR-012, FR-013 | SC-006, SC-007 | Success path truthful log |
| T8 | US3 #1, #3 | FR-011 | SC-006 | Failure path no false-success |
| T9 | — | FR-004a, FR-008 | — | Path-illegal characters in name |
| T10 | — | FR-007 | SC-005 | Pre-fix file co-existence |
| T11 | — | — | — | Lock-order / teardown-during-save |

All FRs and SCs are covered. Edge cases listed in [spec.md](./spec.md) § Edge Cases are covered by T1-T11 except for the ones that are platform-specific OS races (e.g., "Windows share-violation race") — those are observed-but-not-easily-reproducible in a manual test environment; the truthful-log invariant (T8) ensures any future occurrence is detectable from the log.
