# Quickstart: manually verifying the hotkey registration fix

This is a manual test procedure. The plugin has no automated GUI tests; correctness is verified by exercising each user story end-to-end against a fresh OBS install with the patched plugin loaded.

## Prerequisites

- OBS Studio 31.1.1 (or whatever the current `buildspec.json` pin is) installed.
- This plugin built from the `001-fix-hotkey-registration` branch and installed into OBS's plugin directory.
- A scene collection with at least one scene named (e.g.) `Test Scene` containing any visible source — a Color Source is fine.

## Test 1 — User Story 1: Toggle Recording (P1)

1. Open OBS. Open the Multi-Scene Record dock.
2. Add a slot. Set: name = `Slot A`, scene = `Test Scene`, path = a writable directory.
3. Open **Settings > Hotkeys**. Verify a group **`Multi-Scene Record: Slot A`** exists with two rows: `Toggle Recording: Slot A`, `Save Replay: Slot A`.
4. Bind `Toggle Recording: Slot A` to a key combo (e.g., `Ctrl+Shift+R`). Click OK.
5. Press `Ctrl+Shift+R`. Confirm:
   - Dock shows Slot A as active (recording indicator on).
   - A file is created at the configured path.
6. Press `Ctrl+Shift+R` again. Confirm:
   - Dock shows Slot A as idle.
   - The file is finalized (mtime stops advancing, size stable, plays back).

**Pass** if both transitions happen from the hotkey alone, with zero dock interaction.

## Test 2 — User Story 1: per-slot isolation

1. Add a second slot `Slot B` with a different scene and a different file path.
2. Bind `Toggle Recording: Slot B` to a different key combo (e.g., `Ctrl+Shift+S`).
3. Press `Ctrl+Shift+R`. Verify only Slot A starts recording.
4. Press `Ctrl+Shift+S`. Verify only Slot B starts (Slot A still running).
5. Press both keys again to stop. Verify each stops independently.

**Pass** if each hotkey affects only its own slot.

## Test 3 — User Story 2: Save Replay (P1)

1. Edit Slot A. Enable replay buffer; set replay seconds = 30.
2. Save and start Slot A (via dock or hotkey).
3. Bind `Save Replay: Slot A` to `Ctrl+Shift+P`.
4. Wait ~10 seconds. Press `Ctrl+Shift+P`. Confirm:
   - A replay clip file appears at the configured path.
   - No error dialog. No crash.

**Pass** if the clip is written.

## Test 4 — User Story 2: replay-disabled silent no-op

1. Edit Slot A. **Disable** replay buffer. Save.
2. Press `Ctrl+Shift+P` (the previously bound Save Replay key for Slot A).
3. Confirm:
   - No file appears.
   - No error dialog.
   - OBS log shows no `[error]`-level message related to the hotkey (a `[multi-scene-rec] '...' replay save FAILED` info-line is acceptable per current code, but no crash and no user-visible error).

**Pass** if pressing the hotkey is a silent no-op.

## Test 5 — User Story 3: persistence across OBS restart

1. With both slots configured and both Toggle Recording hotkeys bound, close OBS cleanly.
2. Reopen OBS.
3. Open **Settings > Hotkeys**. Verify:
   - Both `Multi-Scene Record: Slot A` and `Multi-Scene Record: Slot B` groups still exist.
   - The previously bound key combos are still attached.
4. Press the Slot A hotkey. Confirm it starts recording Slot A.

**Pass** if every previously assigned binding survives the restart.

## Test 6 — User Story 3: persistence across slot rename

1. With Slot A's Toggle Recording bound to `Ctrl+Shift+R`, rename Slot A → `Game Capture` in the dock editor and save.
2. Open **Settings > Hotkeys**. Verify:
   - The group label is now `Multi-Scene Record: Game Capture` (no stale `Multi-Scene Record: Slot A` group remains).
   - The two rows inside read `Toggle Recording: Game Capture` and `Save Replay: Game Capture`.
   - `Ctrl+Shift+R` is still attached to the Toggle Recording row.
3. Close Settings, press `Ctrl+Shift+R`. Confirm it starts recording the renamed slot.

**Pass** if the group label updates AND the binding is preserved.

## Test 7 — User Story 3: persistence across scene collection switch

1. Note the bindings on Slot A and Slot B.
2. Scene Collection menu → switch to a different collection (creating one with no slots is fine).
3. Open **Settings > Hotkeys**. Verify the `Multi-Scene Record: …` groups are gone (no slots in the new collection).
4. Switch back to the original collection.
5. Open **Settings > Hotkeys**. Verify the groups and bindings are back as in step 1.

**Pass** if all bindings survive the round-trip.

## Test 8 — User Story 4: grouped Settings layout

1. With three slots configured (`Slot A`, `Slot B`, `Slot C`) open **Settings > Hotkeys**.
2. Verify three distinct groups exist, each labelled `Multi-Scene Record: <slot name>`, each containing exactly two rows.
3. Verify the groups are not interleaved with unrelated entries from the "Front-End" list.

**Pass** if the layout matches FR-008 and FR-010.

## Test 9 — Edge case: add a slot at runtime

1. With OBS running and Settings closed, click "Add Slot" in the dock for a new slot `Slot D`.
2. Open **Settings > Hotkeys**. Verify `Multi-Scene Record: Slot D` appears with both rows.
3. Bind keys; verify they fire.

**Pass** if the new slot's hotkeys are usable without an OBS restart.

## Test 10 — Edge case: remove a slot at runtime

1. With Slot D's Toggle Recording bound to a key, remove Slot D from the dock.
2. Open **Settings > Hotkeys**. Verify `Multi-Scene Record: Slot D` is gone.
3. Press the previously bound key. Confirm nothing happens (no crash, no orphan recording).

**Pass** if removal cleanly retires the hotkeys.

## Test 11 — Edge case: encoder-failure during hotkey-initiated start

1. Pick an encoder that will fail on the current system (e.g., set Slot A to use an NVENC encoder on a machine with no NVIDIA GPU). Save.
2. Press the Slot A Toggle Recording hotkey.
3. Confirm the dock surfaces the failure (the same way it would for a click-based start), the slot returns to idle, and OBS does not crash.

**Pass** if hotkey-initiated and dock-initiated failure paths behave the same.

## Test 12 — Stress: full slot lifecycle

Run this in order with a single slot (`Slot A`):

```
add → rename → start (via hotkey) → stop (via hotkey)
    → enable replay → start → save replay (via hotkey) → stop
    → switch scene collection away and back
    → restart OBS
    → remove slot
```

**Pass** if every step succeeds and OBS does not crash at any point (SC-006).

## Regression checks (must still pass)

These exercise behavior outside the changed code path; they should remain unaffected.

- Per-slot recording paths still write to the configured directories.
- The dock's start/stop buttons still work.
- The `[CBR fallback]` indicator still appears when x264 falls back from a hardware encoder.
- Shared-encoder slots (one slot referencing another's encoder via `shared_encoder_slot_id`) still record correctly.
