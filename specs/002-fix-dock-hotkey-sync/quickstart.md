# Quickstart: manually verifying the dock-UI sync fix

The plugin has no automated GUI test harness — verification is by exercising the dock against a live OBS instance. Run these checks against the patched build.

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with at least one scene containing any visible source.
- The Multi-Scene Record dock open and visible.

## Test 1 — User Story 1: dock follows hotkey-initiated start (P1)

1. Open the dock. Add a slot named `Slot A` with a working scene and output path.
2. Bind `Toggle Recording: Slot A` to a key combo (e.g. `Ctrl+Shift+R`) in **Settings > Hotkeys**.
3. Confirm the dock shows Slot A as idle.
4. Press `Ctrl+Shift+R`.
5. **Expected (within 1 s)**: the dock state column for Slot A switches to the active indicator (button label / dot / filename — everything that's state-driven). No click required.

**Pass** if the visual transition happens within 1 second of the keypress, in 100% of attempts.

## Test 2 — User Story 1: dock follows hotkey-initiated stop (P1)

1. With Slot A actively recording (from Test 1), press `Ctrl+Shift+R` again.
2. **Expected (within 1 s)**: the state column switches to idle without any dock interaction.

**Pass** if the visual transition happens within 1 second of the keypress.

## Test 3 — Cross-slot isolation

1. Add `Slot B` and bind its `Toggle Recording` to a different key (`Ctrl+Shift+S`).
2. Click anywhere in Slot B's row to make it the currently selected row in the table.
3. Press `Ctrl+Shift+R` (Slot A's hotkey).
4. **Expected**: Slot A's row updates to active; Slot B's row is unaffected (still idle, still the selected row).

**Pass** if only Slot A's row changes.

## Test 4 — User Story 2: failed start does not leave a fake "active" (P2)

1. Edit Slot A and clear its output path (or set it to a non-existent directory if your build validates paths only at start).
2. Confirm the dock shows Slot A as idle.
3. Press `Ctrl+Shift+R`.
4. **Expected**: the dock stays at idle. No transient "active" flicker. (If your build pops a "Missing output directory" dialog on hotkey-initiated start, that's an even better outcome — but the minimum required behavior is "no fake active.")

**Pass** if the state column never shows active for the failed slot.

## Test 5 — Edge case: hotkey while dock is closed

1. Close the dock (View > Docks > Multi-Scene Record uncheck).
2. Press `Ctrl+Shift+R` (Slot A's hotkey).
3. Confirm Slot A actually starts recording (a file appears at its output path).
4. Reopen the dock.
5. **Expected**: the dock comes back showing Slot A as active. No crash.

**Pass** if (a) recording started despite the dock being closed and (b) the dock displays the correct state when re-shown.

## Test 6 — Edge case: rapid repeated hotkey presses

1. With Slot A idle, press `Ctrl+Shift+R` rapidly 10× in under 3 seconds.
2. Wait ~2 seconds after the last press.
3. **Expected**: the dock state column matches the slot's actual `is_running()` final state. No stale refresh "wins" and shows the wrong indicator. (If you press an even number of times from idle, end state is idle; odd number, end state is active.)

**Pass** if the dock state and the actual recording state (file present and growing vs. not) agree.

## Test 7 — Edge case: external/error stop still refreshes (regression check)

This verifies the existing external-stop refresh path (in `on_rec_output_stop`) wasn't broken.

1. Start Slot A via hotkey or dock click.
2. Fill the output disk, kill the encoder, or otherwise force an external failure (the simplest in dev: run another tool that locks the output file).
3. **Expected**: the dock state column switches from active to idle on its own within a second or two of the external stop.

**Pass** if the dock detects the external stop, as it did before this change.

## Test 8 — Edge case: hotkey across slot removal

1. Bind Slot A's hotkey. Press it once to start recording.
2. While still recording, remove Slot A from the dock (the existing remove flow stops the slot first).
3. After the slot is gone, press its (now-unattached) hotkey.
4. **Expected**: no crash. Either nothing happens (the hotkey was unregistered with the slot) or — if the binding somehow lingers — a refresh is queued but harmlessly finds no such slot.

**Pass** if no crash and no stale row reappears in the dock.

## Test 9 — Save Replay hotkey does NOT trigger state refresh (FR-008 / regression)

1. With Slot A recording and replay buffer enabled, bind its `Save Replay` hotkey.
2. Note the dock's exact rendering.
3. Press the Save Replay hotkey several times.
4. **Expected**: a replay clip is written each press, but the dock state column does NOT flicker or change rendering. (It's already in the active state; that state is correct and remains correct.)

**Pass** if Save Replay produces clips without redrawing the dock state column.

## Regression spot-checks (must still pass)

- Dock-button click: clicking the per-slot state cell starts/stops the slot AND refreshes the dock (this was already correct; verify it remains correct).
- Start All / Stop All: starts/stops every slot AND refreshes the dock.
- Stats column auto-refresh (the 1 Hz timer): bitrates/frames update once per second while a slot is recording.
- Slot edit / rename: dock reflects the new name on save.
