# Quickstart: verifying the idle-state resource audit

Manual verification procedure for the FRs and SCs of feature 011. The plugin has no automated test harness; verification is via the OBS GUI, Task Manager / Resource Monitor (CPU, memory, handle count), a GPU activity view (Windows Task Manager "GPU" column, GPU-Z, or PresentMon) for FR-014, and the OBS log window for leak warnings.

## Prerequisites

- OBS Studio 31.1.1+ with the patched plugin installed.
- A scene collection with **at least two scenes**, at least one of which contains an **active capture source** (webcam, window capture, or display capture) so its "active" state is observable.
- Task Manager (Details tab → add the "Handles" column) **or** Resource Monitor watching the OBS process.
- A GPU activity tool (Windows Task Manager GPU view is enough) for the idle-wakeup check.
- OBS log window open (Help → Log Files → View Current Log) so `leaked … context` / `encoder leaked` / `output leaked` warnings surface immediately.

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

Save reports under `specs/011-idle-slot-resource-audit/results/` (gitignored — institutional memory for future regression triage).

---

## Test 1 — F-UD1: toggling "Show stats" while idle must NOT start the timer (the fix)

**Goal**: confirm the CLOSE — no 1 Hz tick at true idle, even after a stats toggle.

1. Configure **10 slots**. Start **none** of them (all "off").
2. Confirm "Show stats" is checked (default ON). Note OBS CPU% over ~30 s.
3. Uncheck "Show stats", then re-check it. (This is the path through `on_stats_toggled(true)`.)
4. Watch OBS CPU% and, if you have a profiler, watch for a `QTimer::timeout`/`refresh_stats` firing every second.

**Pre-fix expectation**: after step 3 the 1 Hz timer runs at idle — `refresh_stats()` fires every second, iterating all 10 rows, until you next add/remove/start/stop a slot. Small but measurable per-second work with zero slots recording.

**Post-fix expectation**: after step 3 the timer stays paused (no slot is running). No per-second `refresh_stats` activity; CPU matches step 2's idle baseline.

**Pass criteria** (FR-005 / SC-001): no recurring per-second plugin work at idle in any of the states above; toggling stats on/off while idle never starts a running timer.

## Test 2 — Idle CPU baseline does not scale with slot count

**Goal**: SC-001 — idle CPU with N stopped slots ≈ idle CPU with 0 slots.

1. Remove all slots. Record OBS idle CPU% over ~60 s (baseline A).
2. Configure 10 slots, start none. Record OBS idle CPU% over ~60 s (baseline B).
3. Toggle "Show stats" off and on (per Test 1) and re-measure.

**Pass criteria**: B ≈ A within measurement noise; no per-second tick; the plugin adds no idle CPU that grows with slot count.

## Test 3 — FR-002 / SC-004: an idle slot does not keep its scene's sources active

**Goal**: a configured-but-not-running slot must not keep its scene "showing" (which would keep cameras/captures live).

1. Create a scene "CamScene" containing a webcam (or a capture with a visible active indicator / tally light).
2. Make sure "CamScene" is **not** the current program/preview scene in OBS.
3. Configure a slot targeting "CamScene". **Do not start it.**
4. Observe the capture source: its device should NOT be held active by the plugin (no webcam tally light attributable to the slot; the source is not rendering).
5. Start the slot → the source becomes active. Stop the slot → it returns to inactive within ~5 s.

**Pass criteria** (FR-002/SC-004): the source is active only while the slot is running; a configured-but-stopped slot leaves it inactive.

## Test 4 — FR-014: no idle GPU/D3D11 wakeups from held pipelines

**Goal**: confirm no video pipeline (and thus no GPU device wakeup) is held at idle.

1. Configure 10 slots (varied resolutions/encoders), start none.
2. Open the GPU activity view. Observe the OBS process's GPU engine usage with no slot running (and OBS otherwise idle, e.g. no active program scene rendering a heavy source).
3. Start one slot, observe GPU usage rise; stop it, observe it return to the idle level within ~5 s.

**Pass criteria** (FR-014/FR-003): with no slot running, the plugin holds no extra `video_t`/compositing pass; GPU activity attributable to the plugin returns to the no-slot baseline. The OBS log shows no `leaked shared encoder context` on shutdown.

## Test 5 — SC-002 / SC-005: idle memory + 100-cycle leak check

**Goal**: idle memory grows only by a small per-slot constant; repeated cycles do not accumulate.

1. Record OBS private bytes + handle count with 0 slots (baseline).
2. Configure 10 slots, start none; re-measure. The delta should be a small, bounded per-slot constant (config + 2 hotkeys + 1 inert output per slot) with no recording-pipeline allocation.
3. Pick one slot. Start → stop it **100 times** (hotkey or per-row toggle).
4. Return to idle; re-measure private bytes + handle count.

**Pass criteria**: post-cycle memory/handles match the pre-cycle idle figure (no monotonic growth); no `encoder leaked` / `output leaked` warnings in the log.

## Test 6 — FR-007 / SC-003: stop returns to the pre-start baseline

**Goal**: everything `start()` acquired is released by `stop()`.

1. With one slot, record idle handle count + private bytes (pre-start).
2. Start it, let it record ~15 s.
3. Stop it; wait ~5 s; re-measure.

**Pass criteria** (FR-007/SC-003): handle count and memory return to the pre-start value within ~5 s; encoder-group case — start two slots sharing an encoder group, stop one, confirm the other keeps recording and the shared pipeline survives; stop the second, confirm the pipeline is fully released (log: no leak).

## Test 7 — FR-008: rename while idle does not leak

**Goal**: the destroy+recreate of the inert hotkey-group output on rename is leak-free.

1. Configure one slot; record handle count.
2. Open the editor and rename it; save. Repeat ~10 times.
3. Confirm in OBS Settings → Hotkeys that the group label updates to the new name and the bound keys are preserved.
4. Return to idle; re-measure handle count.

**Pass criteria** (FR-008): handle count unchanged after the rename cycles; hotkey bindings preserved; group label reflects the latest name.

## Test 8 — Regression: features 004 / 005 idle behavior (SC-007)

**Goal**: the F-UD1 fix does not regress the running-slot stats display or prior idle gating.

1. Start one slot. Confirm the Frames / Dropped / Kbps columns update once per second (timer runs because a slot is running).
2. Stop it. Confirm the columns return to "--" and the timer pauses.
3. Toggle "Show stats" off while a slot is running → columns hide, timer pauses; toggle on → columns reappear, timer resumes (a slot is running, so the gate allows it).

**Pass criteria** (SC-007): stats update normally while recording; the timer runs iff (`stats_enabled_` AND a slot is running); no regression versus feature 004/005 behavior.

---

## Notes / known-good invariants confirmed by the audit (for fast regression triage)

- `obs_source_inc_showing` appears **only** in `SharedEncoder::build` (`slot.cpp:470`), matched in `~SharedEncoder` (`slot.cpp:561`). If a future change adds an `inc_showing` reachable at idle, Test 3 will catch it.
- The `shared_` registry must be **empty** with no slot running; `SlotManager::shutdown()` logs `leaked shared encoder context` otherwise (`manager.cpp:31`).
- The stats `QTimer` must be active **iff** `stats_enabled_ && any_running()`. After F-UD1, both `refresh()` and `on_stats_toggled` enforce this.
