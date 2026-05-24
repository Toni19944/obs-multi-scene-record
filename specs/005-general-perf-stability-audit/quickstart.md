# Quickstart: verifying the general performance and stability audit

This document is the manual verification procedure for the FRs and SCs of feature 005. The plugin has no automated test harness; verification is manual via the OBS GUI, Task Manager / Resource Monitor (CPU / memory), a frame-time tool (for hitch detection), and OBS logs (for handle-leak / callback-after-unload warnings).

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with at least one scene.
- Task Manager OR Resource Monitor open and watching the OBS process.
- (Optional) PresentMon, RTSS, or any frame-time tool to detect UI hitches.
- OBS logs window open (Help → Log Files → View Current Log) so leak warnings surface immediately.

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

Save reports under `specs/005-general-perf-stability-audit/results/` (gitignored — institutional memory for future regression triage).

## Test 1 — F-S1: redundant `running_.store(true)` removal (regression check)

**Goal**: confirm F-S1's one-line removal does not regress the start/stop lifecycle.

1. Configure 1 slot. Start it via the per-row state toggle.
2. Confirm the dock shows "REC" (or "RPL" if replay-only).
3. Stop it via the per-row toggle. Confirm "off".
4. Repeat 10 times.
5. Press the slot's Toggle Recording hotkey 10 times (5 start / 5 stop).

**Pass criteria**: every transition reflects in the dock; no slot stuck in a half-state; OBS does not log any "encoder leaked" / "output leaked" warning.

## Test 2 — F-M1: `stop_all()` does not hold `mtx_` for the wait duration

**Goal**: confirm the snapshot refactor lets the dock remain responsive during a multi-slot stop.

1. Configure 5 slots, all recording.
2. While they are recording, click "Stop All".
3. **During the stop sequence** (which can take 1 – 5 s per slot for outputs to flush), click the dock's "Add" button.

**Pre-fix expectation**: the Add button click is queued behind `mtx_` and only opens the editor after every slot has flushed. UI feels frozen.

**Post-fix expectation**: the Add button opens the editor immediately; the stop sequence proceeds in the background. Manager's `mtx_` is held only briefly (microseconds) during the snapshot.

**Pass criteria**: clicking Add (or any dock control) during Stop All does not visibly freeze.

## Test 3 — F-UD1: dock refresh latency (≤100 ms target)

**Goal**: verify SC-007 (~100 ms dock action latency) holds with 10 slots and is improved by the QTableWidgetItem reuse.

For each of the following, count seconds from click to row visibly updating:

1. Click "Add Slot," fill in the editor, click OK.
2. Double-click a slot to open the editor, change the name, click OK.
3. Click the per-row state toggle to start a slot.
4. Click the per-row state toggle to stop the slot.
5. Click "Start All."
6. Click "Stop All."
7. Click "Remove" on a slot (confirm the prompt).

**Pre-fix expectation**: 8 QTableWidgetItem allocations per row per refresh — visible but small cost on 10-slot tables.

**Post-fix expectation**: existing items reused (mutate-in-place); only a `setText` per cell. Should feel snappier than pre-fix, especially on Start All / Stop All which fully refresh the table.

**Pass criteria**: every action visibly updates within ~100 ms. No regression from pre-fix.

## Test 4 — F-USE1: editor open latency (≤200 ms target)

**Goal**: verify SC-007 (editor populates within ~200 ms).

1. Configure a slot with an NVENC encoder selected (NVENC has the most encoder-specific introspection — preset / profile / tune / multipass all populate).
2. Time from "Edit" (or double-click row) → dialog fully populated and visible.
3. Repeat with x264, AMF, QSV in turn.
4. While the editor is open, change the encoder family in the Video Encoder combo. Time from selection-change → encoder-specific widgets re-populated.

**Pre-fix expectation**: `update_encoder_specific_ui` calls `obs_get_encoder_properties` 4+ times per change — measurable on slow systems.

**Post-fix expectation**: one call per change. Should be faster, especially on encoder-family switch.

**Pass criteria**: editor open within ~200 ms; encoder-family switch within ~100 ms; no visible stall on any combo repopulation.

## Test 5 — Idle CPU baseline (carry-forward of feature 004)

**Goal**: verify SC-005 (≈0 % plugin-attributable idle CPU with no slots) and SC-006 (no per-stopped-slot tax) — same bar as feature 004, no regression allowed.

1. Cold-start OBS, plugin loaded, zero slots. Leave idle 60 s. Sample OBS CPU% every 5 s. Take the median.
2. Configure 10 stopped slots (no recording). Leave idle 60 s. Sample again.
3. Compare medians.

**Pass criteria**: both medians at or near 0 %; medians indistinguishable within measurement noise; the audit does not introduce any new always-on work.

## Test 6 — Save callback latency (~16 ms typical case)

**Goal**: verify FR-007 (save callback completes well under one frame at 60 fps for ≤10 slots).

1. Configure 10 slots (mix of running and idle).
2. Trigger OBS save (Ctrl+S) 20 times in rapid succession.
3. Observe OBS UI throughout.

**Pass criteria**: no visible UI freeze on any save. Dock buttons remain clickable between saves.

## Test 7 — Memory baseline (FR-013 carry-forward)

**Goal**: capture institutional-memory values for the plugin's static memory footprint after this feature's edits.

1. Cold-start OBS. Note resident memory (baseline). `mem_obs_only`.
2. Wait 30 s for steady state.
3. Plugin loaded + zero slots: `mem_plugin_no_slots`. Plugin-attributable = `mem_plugin_no_slots − mem_obs_only`.
4. Configure 10 stopped slots: `mem_10_stopped`. Per-stopped-slot cost = `(mem_10_stopped − mem_plugin_no_slots) / 10`.

**Record values**, not pass/fail. Expected: comparable to feature 004 baseline (no regression). Material deviation in a future build is the regression signal.

## Test 8 — 50-cycle leak check (SC-008)

**Goal**: verify SC-008 (no per-cycle leak across 50 start/stop cycles).

1. Configure 1 slot.
2. Note resident memory (`mem_pre`).
3. Start / stop the slot 50 times via the per-row toggle (or hotkey).
4. Note resident memory after the 50th stop (`mem_post_50`).

**Pass criteria**: `mem_post_50 − mem_pre` is no more than one running-encoder above baseline (typically < 50 MB). No per-cycle growth visible.

## Test 9 — Scene-collection round-trip (no handle leak)

**Goal**: verify SC-009 (no leaked-handle log lines) on a scene-collection round-trip.

1. With 5 slots configured (some recording), switch to another scene collection.
2. Switch back to the original collection.
3. Repeat 5 times.
4. Inspect OBS logs after each round-trip.

**Pass criteria**: no visible UI freeze on any switch. No `[multi-scene-rec]` log line mentioning a leaked handle. Slots are stopped on switch-out and the dock reflects the round-trip cleanly.

## Test 10 — OBS shutdown leak check (SC-009)

**Goal**: verify SC-009 (no callback-fired-after-unload or leaked-handle log lines at shutdown).

1. Configure 3 slots, start them.
2. Close OBS while they are still recording.
3. Reopen OBS, plugin loaded.
4. Inspect the previous-session log (Help → Log Files → View Previous Log).

**Pass criteria**: no `leaked shared encoder context for group …` line, no "callback fired after unload" / dangling-callback warning. The post-reopen state matches pre-close (same slots, hotkey bindings restored).

## Test 11 — F-S2 latent race (informational; expected unreachable)

**Goal**: confirm the documented (b)-disposition latent race in `SceneSlot::start()` / `stop()` is not reachable on the observed thread model.

1. Configure 1 slot.
2. Bind its Toggle Recording hotkey to a key.
3. Hold the key down to spam-fire start/stop (autorepeat triggers many OBS hotkey events).
4. Simultaneously click the per-row state toggle in the dock.
5. Watch for any state where the dock shows "REC" but the stats columns show "--" indefinitely (the symptom of `running_=true` with no outputs).

**Pass criteria**: dock state and recording state remain consistent. If you do observe the symptom, **escalate** — the (b) disposition documented in research.md must be revisited.

## Test 12 — Encoder fallback regression check (Principle V)

1. Configure a slot with NVENC (or another hardware encoder) on a system where it IS available.
2. Start it. Confirm normal recording.
3. Stop it.
4. Edit the slot to use a non-existent encoder ID (workaround: pick an encoder you don't have, or unplug the GPU — not actually feasible, so just confirm by inspection that the fallback code path in `SharedEncoder::build` is untouched in this feature's diff).

**Pass criteria**: the `[CBR fallback]` indicator still appears in the dock encoder column when the configured encoder is unavailable. Feature 005 must not have touched this path.

## Test 13 — All-feature regression sweep (SC-010)

Run the quickstart procedures from:

- `specs/001-fix-hotkey-registration/quickstart.md`
- `specs/002-fix-dock-hotkey-sync/quickstart.md`
- `specs/003-perf-parity-audit/quickstart.md`
- `specs/004-general-perf-pass/quickstart.md`

**Pass criteria**: every prior-feature test still passes.

## Test 14 — Crash check (SC-004)

While performing Tests 1 – 13, monitor for any plugin crash, OBS crash, or hang.

**Pass criteria**: zero crashes attributable to feature 005 changes across the full sweep.

## Regression spot-checks (must still pass)

- Per-slot recording writes to configured directories.
- Dock state column reflects hotkey-initiated transitions (feature 002).
- Settings > Hotkeys shows `Multi-Scene Record: <slot name>` groups (feature 001).
- `[CBR fallback]` indicator appears on x264 fallback (Principle V).
- Shared-encoder slots still record correctly (Principle II).
- Replay buffer saves clips on hotkey or button press.
- 1 Hz stats QTimer remains gated by `mgr.any_running()` (feature 004 F1) — pauses when no slots run.
- Cell-widget state-toggle button is reused per row, not recreated (feature 004 F2).
