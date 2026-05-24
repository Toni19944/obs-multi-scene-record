# Quickstart: verifying the general performance pass

This document is the manual verification procedure for the FRs and SCs of feature 004. The plugin has no automated test harness; verification is manual via the OBS GUI, Task Manager / Resource Monitor (CPU/memory), and a frame-time tool if available (for hitch detection).

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with at least one scene.
- Task Manager OR Resource Monitor open and watching the OBS process.
- (Optional) PresentMon, RTSS, or any frame-time tool to detect UI hitches.

## Reporting template

For each test, record:

```
Test:           [test number and title]
Build:          [git rev-parse HEAD]
OBS version:    [OBS Help → About]
Hardware:       [CPU / GPU / RAM]
Pre-fix value:  [measurement before this feature]
Post-fix value: [measurement after this feature]
Verdict:        [PASS / FAIL] [+ notes]
```

Save reports under `specs/004-general-perf-pass/results/` (gitignored — institutional memory for future regression triage).

## Test 1 — User Story 2: idle CPU baseline, no slots (P1)

**Goal**: verify SC-002 (≈0% plugin-attributable idle CPU with no slots).

1. Cold-start OBS. Plugin loaded; zero slots configured (delete any existing slots first).
2. Leave OBS idle (no recording, no streaming, no preview, no other interaction) for 60 seconds.
3. Note OBS process CPU% sampled every 5 seconds. Take the median.

**Pre-fix expectation**: the 1 Hz stats timer fires but operates on an empty row set (no iterations); cost is the timer-delivery itself. Probably indistinguishable from zero, but measurable to a profiler.

**Post-fix expectation**: timer is paused (no slots are running → no timer); plugin contributes literally zero scheduled work. CPU is bounded by everything else OBS does at idle.

**Pass criteria**: post-fix median CPU is at or below pre-fix median CPU. No regression.

## Test 2 — User Story 2: idle CPU with stopped slots (P1)

**Goal**: verify SC-003 (no per-stopped-slot idle CPU tax).

1. Cold-start OBS. Configure **10 slots**, all stopped. Don't start any.
2. Leave OBS idle for 60 seconds.
3. Note OBS process CPU% sampled every 5 seconds. Take the median.

**Pre-fix expectation**: timer fires every second, iterates 10 rows, ~10 atomic loads + ~50 no-op `setText` calls per second. Small but non-zero.

**Post-fix expectation**: timer paused (none of the 10 slots are running); plugin contributes zero scheduled work. CPU should match Test 1.

**Pass criteria**: post-fix median CPU is essentially identical to Test 1's median (no per-stopped-slot tax). Within measurement noise.

## Test 3 — Stats timer runs when a slot starts and pauses when all stop

**Goal**: verify F1's fix end-to-end.

1. With 10 stopped slots from Test 2 still configured, ensure the "Show stats" checkbox is enabled in the dock.
2. Note: timer should NOT be running. (Verify, if you have profiling, that no QTimer timeout is firing.)
3. Start one slot via the dock state-toggle button.
4. **Expected**: stats columns (Frames, Dropped, Kbps) on that row begin updating once per second. Other rows continue to show "--".
5. Stop the slot.
6. **Expected**: stats columns return to "--" once. Then the timer pauses again.
7. Start two slots simultaneously via Start All.
8. **Expected**: stats columns on both rows update at 1 Hz.
9. Stop both via Stop All.
10. **Expected**: stats columns return to "--" and the timer pauses.

**Pass criteria**: timer starts on the first slot starting, pauses when the last slot stops, regardless of how many slots are configured.

## Test 4 — User Story 1: dock action latency (P1)

**Goal**: verify SC-004 (~100 ms for visible effect of dock actions) with 10 slots.

For each of the following actions, count seconds from click to the row visibly updating. (Use a frame-time tool if available, or just feel — they should all feel instant.)

1. Click "Add Slot," fill in the editor, click OK.
2. Double-click a slot to open the editor, change the name, click OK.
3. Click the per-row state toggle on a slot to start it.
4. Click the per-row state toggle on the running slot to stop it.
5. Click "Start All."
6. Click "Stop All."
7. Click "Remove" on a slot (and confirm the prompt).

**Pre-fix expectation**: actions feel responsive but with measurable button-rebuild cost (especially state-toggle and Start All / Stop All with 10 slots). 50-100 ms range.

**Post-fix expectation**: actions feel snappier — state-toggle in particular should be visibly instant since the per-row button isn't being destroyed and recreated. <50 ms ideal.

**Pass criteria**: every action visibly updates within ~100 ms with no perceptible UI freeze. No regression from pre-fix.

## Test 5 — User Story 3: no UI freeze on save (P2)

**Goal**: verify SC-005 (no visible OBS UI freeze during save).

1. Configure 10 slots (mixture of running and idle).
2. Trigger OBS save with Ctrl+S 20 times in rapid succession.
3. Observe OBS UI throughout.

**Pass criteria**: no visible freeze in OBS UI on any save. Dock buttons remain responsive (try clicking the per-row toggle between saves).

## Test 6 — User Story 3: no UI freeze on scene collection switch (P2)

1. With 5 slots configured and running, switch to another scene collection via Scene Collection menu.
2. Switch back to the original collection.
3. Repeat 5 times.

**Pass criteria**: no visible UI freeze during any switch. Plugin slots stop on collection switch (existing behavior, feature 003 quickstart) and the dock reflects the empty/restored state without hitches.

## Test 7 — Memory baseline (FR-008)

**Goal**: capture institutional-memory values for the plugin's static memory footprint.

1. Cold-start OBS. Note resident memory of OBS process (baseline). Call this `mem_obs_only`.
2. Wait 30 seconds for steady state.
3. With the plugin loaded and zero slots, note the resident memory. Call this `mem_plugin_no_slots`.
   - Plugin-attributable: `mem_plugin_no_slots - mem_obs_only`.
4. Configure 10 stopped slots. Note resident memory. Call this `mem_10_stopped`.
   - Per-stopped-slot cost: `(mem_10_stopped - mem_plugin_no_slots) / 10`.

**Record values**, not pass/fail. Expected order of magnitude per spec FR-007 / research F7: ~1-2 KB per stopped slot. Material deviation in a future build is the regression signal.

## Test 8 — Stats display when running (regression check)

1. Configure 1 slot and start it.
2. Watch the dock for 10 seconds.

**Pass criteria**: Frames, Dropped, and Kbps columns update once per second with non-zero values. Same behavior as pre-fix — F1's fix MUST NOT break the stats display when at least one slot is running.

## Test 9 — Stats checkbox toggle (regression check)

1. With 1 slot recording, toggle the "Show stats" checkbox off in the dock.

**Pass criteria**: stats columns hide, timer pauses, dock continues to show state correctly.

2. Toggle the checkbox back on.

**Pass criteria**: stats columns reappear and start updating within 1 second.

## Test 10 — Cross-thread refresh path (regression check)

1. Configure 1 slot, bind its Toggle Recording hotkey.
2. Press the hotkey several times rapidly (start/stop/start/stop).

**Pass criteria**: dock state column reflects every transition correctly (feature 002's guarantee). The cross-thread `QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection)` path still works correctly with F2's cell-widget reuse pattern.

## Test 11 — All-feature regression sweep

Run the quickstart procedures from:

- `specs/001-fix-hotkey-registration/quickstart.md` (12 tests)
- `specs/002-fix-dock-hotkey-sync/quickstart.md` (9 tests + regression spot-checks)
- `specs/003-perf-parity-audit/quickstart.md` (12 tests)

**Pass criteria**: all prior-feature tests still pass (FR-009 / SC-006).

## Test 12 — Crash check (SC-007)

While performing Tests 1-11, monitor for any plugin crash, OBS crash, or hang.

**Pass criteria**: zero crashes attributable to feature 004 changes.

## Regression spot-checks (must still pass)

- Per-slot recording paths still write to configured directories.
- Dock state column reflects hotkey-initiated transitions (feature 002 still works).
- Settings > Hotkeys shows `Multi-Scene Record: <slot name>` groups (feature 001 still works).
- `[CBR fallback]` indicator still appears on x264 fallback.
- Shared-encoder slots still record correctly.
- Replay buffer still saves clips on hotkey or button press.
