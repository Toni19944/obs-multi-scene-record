# Quickstart: benchmarking the FPS-parity fix and the memory bounds

This document is the manual verification procedure for FR-001 through FR-007 (FPS parity, memory stability) and SC-001 through SC-007 (success criteria) of feature 003.

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with at least one scene containing a representative video source (a game capture or a 1080p/60 video source works best for benchmarks).
- A **stable, FPS-bound** benchmark scenario. Recommended:
  - A locked benchmark (e.g., a game's built-in benchmark, 3DMark, Unigine Heaven/Superposition).
  - A demo replay that's CPU/GPU-bound at the test resolution/framerate.
  - **Avoid** menus, loading screens, or anything that produces non-stationary FPS distributions.
- An FPS overlay or capture tool that produces both average FPS and 1% low FPS over a sustained interval (RTSS, PresentMon, MSI Afterburner, CapFrameX).
- ≥ 5 minutes between benchmark runs to let GPU/CPU thermals settle.
- A clean OBS session (no other recordings/streams/projectors running) for each measurement.

## Test 1 — FPS overhead baseline measurement, x264 (informational)

**Goal**: capture the per-slot FPS overhead as institutional memory for future regression detection. This is NOT a pass/fail bar — single-slot parity with OBS native is explicitly impossible while preserving per-slot scene independence (see research.md D1 / D11). What we DO want: a number to compare against in future feature work to catch any regression that ADDS overhead beyond the current baseline.

For each of the three conditions below, run the benchmark **5 times**, recording average FPS and 1% low FPS per run. Take the **median** across runs (suppresses one-off jitter from background processes, GC ticks, encoder warmups).

| Run group | Configuration |
|---|---|
| **Baseline** | OBS open, no recording. Just the dock visible. |
| **OBS native** | OBS's built-in Start Recording. Encoder: x264. Rate control: CBR. Bitrate: 6000 kbps. Resolution: same as main canvas (1920x1080). Framerate: 60. Track 1 only. Container: mp4. |
| **Plugin** | This plugin, one slot recording. Same scene as OBS's current program scene. Same encoder/RC/bitrate/resolution/framerate/track/container as the OBS native run. Replay buffer DISABLED. |

**Record**:

- `overhead_plugin_total = median(baseline.avg_fps) − median(plugin.avg_fps)` — the plugin's total FPS hit while recording.
- `overhead_obs_total = median(baseline.avg_fps) − median(obs_native.avg_fps)` — OBS native's FPS hit for the same workload.
- `delta = overhead_plugin_total − overhead_obs_total` — the extra hit the plugin adds. Expected to be > 0 (the cost of per-slot independence). Capture this number as the baseline.

**Pass criteria**: numbers recorded in the report; no crash; recording file is valid. There is NO numeric threshold on `delta` for this feature — closing it would require breaking per-slot independence.

## Tests 2 / 3 / 4 — Same overhead measurement, NVENC / AMF / QSV (informational)

Same procedure as Test 1 with the corresponding encoder. Skip if the test machine doesn't have that encoder family. Capture the `delta` number per encoder family.

**Pass criteria**: numbers recorded; no crash.

## Test 5 — Non-program-scene recording (FR-009 disposition (b) verification)

**Goal**: confirm the per-group view path works correctly when the slot's scene differs from program. This is the irreducible path; FPS hit here is also expected.

1. Create two scenes in OBS: `Scene_Program` (a simple color source) and `Scene_Slot` (a game capture or video).
2. Make `Scene_Program` the current program scene.
3. Configure a plugin slot recording `Scene_Slot` at the same resolution/fps as the main canvas.
4. Start the slot. Confirm the recording file captures `Scene_Slot` content (not `Scene_Program`).
5. Switch OBS program from `Scene_Program` to a third scene partway through the recording. Confirm the slot's recording continues capturing `Scene_Slot` — NOT the new program scene. This is the per-slot independence guarantee in action.

**Pass criteria**: the recording file shows `Scene_Slot` content throughout, including during and after the program switch. No crash.

## Test 6 — User Story 2: 4-hour memory stability (P2)

**Goal**: verify FR-004 / FR-005 / SC-002 / SC-003.

1. Cold-start OBS. Wait 30 seconds for steady-state idle.
2. Note OBS's resident memory (Task Manager / Resource Monitor / `Get-Process obs64`). This is the **pre-recording baseline**.
3. Configure one plugin slot at default settings (x264 CBR 6000, 1920x1080, 60 fps, track 1, no replay buffer).
4. Start the slot. Let it record for **5 minutes**.
5. Note resident memory. This is the **5-minute plateau**.
6. Let the recording run for an additional **3 hours and 55 minutes** (4 hours total). Sample resident memory every 5–10 minutes. Optionally: open and close the slot editor a few times during this window to exercise the update-config path.
7. Note resident memory just before stopping. This is the **end-of-run memory**.
8. Stop the slot. Wait 30 seconds.
9. Note resident memory. This is the **post-stop memory**.

**Pass criteria**:

- `end_of_run − plateau ≤ 50 MB` (FR-004 / SC-002)
- `post_stop − pre_recording_baseline ≤ 30 MB` (FR-005 / SC-003)

## Test 7 — User Story 2: 50-cycle start/stop (FR-006 / SC-004)

**Goal**: catch per-cycle leaks that wouldn't show in continuous recording.

1. Cold-start OBS. Note resident memory (baseline).
2. Configure one slot at default settings.
3. Start the slot, wait ~10 seconds, stop the slot. Wait ~5 seconds for teardown to settle.
4. Repeat step 3 a total of **50 times**.
5. Note resident memory.

**Pass criteria**: `post_50_cycles − baseline ≤ ~ one running encoder's worth of memory` (typically < 80 MB). Any growth attributable to the cycles themselves (i.e., > 80 MB) is a leak.

## Test 8 — Scene-collection switch round-trip (FR-007)

1. Note baseline memory.
2. Configure one slot in scene collection A.
3. Switch to scene collection B (or create an empty new one).
4. Switch back to A.
5. Repeat steps 3–4 a second time.
6. Note resident memory.

**Pass criteria**: `final − baseline ≤ ~20 MB` (FR-007).

## Test 9 — Replay-buffer variant (informational)

Re-run Test 1 with replay buffer ENABLED on the plugin slot.

**Record**: the same `delta` measurement vs the recording-only run. The replay-buffer pipeline shares the same `SharedEncoder` and adds an additional output, but the GPU compositing work is unchanged from the recording-only case. The delta should not be meaningfully larger than Test 1's delta.

**Pass criteria**: numbers recorded; no crash; clip-save still works.

## Test 10 — Sharing-slot variant (informational)

1. Configure Slot A as an owner (its own encoder).
2. Configure Slot B with `shared_encoder_slot_id = Slot A`.
3. Start both.
4. Run the benchmark.

**Record**: the `delta` for the combined two-slot workload. The shared `SharedEncoder` means only one per-group compositing pass for both slots; the audio encoders are per-slot but their cost is the same as OBS native's multi-track recording. The delta should be close to Test 1's delta (one extra compositing pass total), not double it.

**Pass criteria**: numbers recorded; both slots produce valid recordings.

## Test 11 — Regression: features 001 + 002 still pass

Re-run the quickstart procedures from:

- `specs/001-fix-hotkey-registration/quickstart.md` (12 tests)
- `specs/002-fix-dock-hotkey-sync/quickstart.md` (9 tests + regression spot-checks)

**Pass criteria**: all prior-feature tests still pass (FR-010 / SC-006).

## Test 12 — Crash check (SC-007)

While performing tests 1–11, monitor for any plugin crash, OBS crash, or hang. Specifically watch:

- OBS log (Help → Log Files → View Current Log) for `[multi-scene-rec] ... LOG_ERROR` entries.
- Process Monitor for sudden DLL unloads.
- Windows Event Viewer for application errors.

**Pass criteria**: zero crashes attributable to the recording-pipeline changes (SC-007).

## Regression spot-checks (must still pass)

- Per-slot recording paths still write to configured directories.
- Dock state column reflects hotkey-initiated transitions (feature 002).
- Settings > Hotkeys shows `Multi-Scene Record: <slot name>` groups (feature 001).
- `[CBR fallback]` indicator still appears on x264 fallback.
- Shared-encoder slots still record correctly.
- Replay buffer still saves clips on hotkey or button press.

## Reporting template

For each test, record:

```
Test:           [test number and title]
Build:          [git rev-parse HEAD]
OBS version:    [OBS Help → About]
Hardware:       [CPU / GPU / RAM]
Settings:       [encoder / RC / bitrate / resolution / fps / tracks]
Run-by-run:     [list of (avg_fps, 1pct_low) per run, ≥5 runs]
Median:         [med_avg_fps, med_1pct_low]
OBS native med: [for comparison; only for tests 1-4]
Delta:          [plugin med - obs native med]
Verdict:        [PASS / FAIL] [+ notes on any anomaly]
```

Save the per-test reports under `specs/003-perf-parity-audit/results/` (gitignored — they don't need to ship with the plugin, but they're useful institutional memory for future regression triage). The `delta` numbers from Tests 1–4, 9, 10 are the most valuable forward-looking artifact: if a future change pushes any of these numbers up significantly, that's a signal that someone introduced new plugin-specific overhead beyond the irreducible per-slot rendering cost.
