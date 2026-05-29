# Research / Audit: Idle-State Background Resource Usage for Configured-but-Not-Running Slots

This document is the Phase 0 deliverable — the audit itself. It enumerates every background resource a **configured-but-not-running** slot can hold while idle (FR-001), assigns each a disposition (**CLOSE** = confirmed correctness leak fixed here; **ACCEPT** = minimum necessary, verified correct; **DEFER** = acceptable-but-reducible, handed to a follow-up), and cross-references the three concerns raised during clarification (FR-012 inactive-output frame callbacks, FR-013 timer interval, FR-014 platform GPU idle wakeups).

**Definition used throughout**: a slot is *idle* when it exists in `SlotManager::slots_` and `SceneSlot::is_running()` is `false` — never started, or started then stopped.

## Method

Static walk of the idle-relevant code paths in `slot.cpp`/`slot.hpp`, `manager.cpp`/`manager.hpp`, `ui-dock.cpp`, and `plugin-main.cpp`, tracing each resource from where it is acquired to where it is released, and checking which acquisitions are reachable while a slot is *not* running. Line references are against the tree at the time of writing.

## Summary table

| ID | Subsystem | Resource class | Finding | Disposition |
|----|-----------|----------------|---------|-------------|
| F-UD1 | `ui-dock.cpp` | Scheduled work (1 Hz QTimer) | `on_stats_toggled(true)` starts the stats timer with no `any_running()` gate → timer ticks at idle until the next `refresh()` | **CLOSE** |
| F-TIMER-INT | `ui-dock.cpp` | Timer cost while running | 1 Hz interval hardcoded; `refresh_stats()` takes `mtx_` per slot per tick. Not an *idle* cost (timer paused at idle after F-UD1) | **DEFER** |
| F-SHOW | `slot.cpp` | Scene "showing" / compositing | `inc_showing` confined to `SharedEncoder::build`; idle slot keeps no scene shown | **ACCEPT** |
| F-PIPE | `slot.cpp`/`manager.cpp` | Video pipeline (view/`video_t`/encoder), GPU/D3D11 | `SharedEncoder` refcounted; registry empty at idle; no held GPU pipeline | **ACCEPT** |
| F-OUT | `slot.cpp` | Outputs, audio encoders, replay buffer RAM | Created in `setup_*`, released in `teardown_locked`; idle holds none | **ACCEPT** |
| F-HK | `slot.cpp` | Inert `hotkey_out_` output + 2 hotkeys | Minimal legitimate idle retention for the Settings hotkey group; inactive output → no frame callback | **ACCEPT** |
| F-CYCLE | `slot.cpp`/`manager.cpp` | Per-cycle accumulation | `~SceneSlot`/`remove_slot`/`load_from` release cleanly; rename destroy+recreate is leak-free | **ACCEPT** |
| F-CB | `manager.cpp` | Module-level callbacks | `frontend_event_cb` + `save_cb` registered once; not per-slot, not periodic | **ACCEPT** |

One CLOSE, one DEFER, six ACCEPT.

---

## F-UD1 — Stats timer started at idle by `on_stats_toggled` (CLOSE)

**Code**: `ui-dock.cpp:394-410` (`on_stats_toggled`), `:106-108` (timer creation, 1000 ms), `:117-119` (constructor start + `refresh()`), `:213-224` (the `refresh()` gate), `:227-254` (`refresh_stats`). Gate helper: `SlotManager::any_running()` (`manager.cpp:59`).

**Observation**: Feature 004 (F1) gated the 1 Hz stats `QTimer` so it runs only while at least one slot is recording — `refresh()` does this at `ui-dock.cpp:218`:

```cpp
if (stats_enabled_ && mgr.any_running()) {
    if (!stats_timer_->isActive()) stats_timer_->start();
} else {
    if (stats_timer_->isActive()) stats_timer_->stop();
}
```

But `on_stats_toggled(true)` starts the timer **unconditionally**:

```cpp
if (on) {
    ... reset samplers ...
    stats_timer_->start();      // ui-dock.cpp:406 — NO any_running() gate
    refresh_stats();            // one-shot populate
}
```

`on_stats_toggled` calls `refresh_stats()` (a single populate), **not** `refresh()` — so the gate is never applied. Therefore: with no slot running, toggling "Show stats" off→on starts the 1 Hz timer and it keeps firing `refresh_stats()` once per second. The timer is only re-gated (and stopped) by the *next* `refresh()`, which happens on a state transition (add / remove / edit / start / stop / hotkey toggle). Until then the plugin does per-second work at true idle: per tick `refresh_stats()` iterates every row, calling `slot_at()` (which takes `SlotManager::mtx_`) and `s->stats()` for each, plus `slot_name_by_id()` for shared rows.

**Why it matters**: This violates the zero-idle-work bar (spec FR-005 / SC-001) and the Constitution's "Plugin overhead in idle MUST be negligible" Product Quality Bar. It is precisely the always-on idle tick the 2026-05-29 clarification flagged (and the partial state the spec's concern (b) described: "stats reads/second … even when idle"). It is a regression against feature 004's documented intent (whose research stated `on_stats_toggled` should "also call the same any-running check when enabling — so toggling 'Show stats' on while no slot is running doesn't start a pointless timer"); the gate was placed in `refresh()` but not in this path.

**Disposition — CLOSE.** Gate the start in `on_stats_toggled` on `any_running()`, mirroring `refresh()`:

```cpp
if (on) {
    auto& mgr = SlotManager::instance();
    for (size_t i = 0; i < mgr.slot_count(); ++i)
        if (auto* s = mgr.slot_at(i)) s->reset_stats_sampler();
    if (mgr.any_running()) stats_timer_->start();   // gated
    refresh_stats();                                // one-shot populate is harmless when idle
} else { ... }
```

The constructor at `:117` (`if (stats_enabled_) stats_timer_->start();`) is left to the immediately-following `refresh()` at `:119`, which already gates correctly (at dock-create time no slot is running, so it stops the timer) — the redundant `:117` start can be dropped so timer-gating has a single source of truth, but this is cosmetic since `:119` self-corrects.

**Threading**: the added `any_running()` runs on the Qt main thread, takes only outermost `mtx_`, and the per-slot `is_running()` reads are lock-free atomics — identical to the call `refresh()` already makes. No new lock, no order change (Constitution III preserved). Widget-internal, routes through `SlotManager` (Constitution IV preserved).

---

## F-TIMER-INT — Stats timer interval hardcoded; per-tick `mtx_` per slot (DEFER)

**Code**: `ui-dock.cpp:107` (`setInterval(1000)`), `:227-254` (`refresh_stats` loop calling `slot_at()` per row).

**Observation**: The refresh interval is a hardcoded 1000 ms; there is no user setting for it. Each tick, `refresh_stats()` calls `slot_at(i)` per row, and `slot_at` takes `SlotManager::mtx_` on every call (`manager.cpp:49`), so a 10-slot table takes/releases `mtx_` ten times per tick (plus `slot_name_by_id` for shared rows). The clarification context raised whether the interval is configurable and noted the per-tick read cost.

**Why it is not an idle finding**: After F-UD1, the timer is **paused whenever no slot is running**. The per-tick cost is therefore only paid *while recording* — by definition not an idle-state cost, which is this feature's scope. The 1 Hz cadence over a handful of running slots is small and was accepted by feature 005 (research §107/§133).

**Disposition — DEFER** (acceptable-but-reducible, per the 2026-05-29 hybrid-scope clarification). Recommendations for a follow-up feature:
1. Make the refresh interval a user preference (or expose a coarser/finer option), since 1 Hz is a fixed product decision rather than a correctness requirement.
2. Snapshot the slot pointers once per tick under a single `mtx_` acquisition (mirroring `start_all`/`stop_all`'s snapshot pattern) instead of calling `slot_at()` per row, reducing per-tick lock traffic from O(rows) to O(1).

Neither is a correctness leak, so neither is fixed in this feature.

---

## F-SHOW — Scene "showing" reference confined to running state (ACCEPT)

**Code**: `slot.cpp:470` (`obs_source_inc_showing` in `SharedEncoder::build`), `:560-564` (`obs_source_dec_showing` + release in `~SharedEncoder`).

**Observation**: The only `inc_showing` in the plugin is inside `SharedEncoder::build`, and it is matched by `dec_showing` in `~SharedEncoder`. A `SharedEncoder` exists only between the first consumer's `acquire_shared_encoder` and the last consumer's `release_shared_encoder` (both gated on running state). A configured-but-not-running slot holds **no** `SharedEncoder`, therefore keeps **no** scene in a "showing" state.

**Why it matters**: This is the headline idle correctness property (FR-002 / SC-004): if a stopped slot left its scene "shown", OBS would keep that scene's video sources (cameras, window/display capture) active and rendering in the background, a large hidden idle cost. Verified that it does not.

**Disposition — ACCEPT** (verified correct, no leak). Backstopped by the quickstart "scene source not shown at idle" check (FR-002/SC-004) and the "stop returns to baseline" check (FR-007).

---

## F-PIPE — Video pipeline / GPU surfaces refcounted, empty at idle (ACCEPT)

**Code**: `slot.cpp:462-536` (`SharedEncoder::build`: scene/view/`video_t`/encoder), `:538-565` (`~SharedEncoder`); `manager.cpp:235-271` (`acquire_shared_encoder`), `:273-293` (`release_shared_encoder`), `:15-37` (`shutdown` leak check), `:159` (`shared_` registry).

**Observation**: The entire video pipeline (scene source, `obs_view_t`, `video_t` from `obs_view_add2`, and the video encoder) lives in `SharedEncoder`, owned exclusively by `SlotManager` and reference-counted by `use_count_`. It is built on the first running consumer's acquire and destroyed (mandatory encoder→view→scene order) on the last consumer's release. The `shared_` map is therefore **empty** whenever no slot is running; `shutdown()` logs `leaked shared encoder context …` for any survivor, giving a runtime tripwire.

**Cross-reference FR-014 (platform GPU/D3D11 idle wakeups)**: Idle GPU/D3D11 device wakeups attributable to "held but unused video pipelines" require a `video_t`/view to exist at idle. Since none exists at idle (registry empty), the plugin contributes no such wakeups while no slot runs. The per-group extra compositing pass (one `video_t` per encoder group) is an irreducible *running* cost already accepted by feature 003; it does not persist into idle.

**Disposition — ACCEPT** (verified correct, no leak). Backstopped by the quickstart idle-GPU-activity check and the `shutdown()` leak log.

---

## F-OUT — Outputs, audio encoders, replay-buffer RAM released on stop (ACCEPT)

**Code**: `slot.cpp:963-1092` (`setup_outputs`: `rec_out_`, `replay_out_`, replay `max_size_mb` RAM), `:919-957` (`setup_encoders`: per-track audio encoders), `:843-913` (`teardown_locked` releases all of the above).

**Observation**: Recording/replay outputs, per-track audio encoders, and the replay buffer's reserved memory are all created in `start()`'s `setup_*` helpers and released in `teardown_locked()` (which also disconnects signal handlers and waits for async stop). A not-running slot holds none of these. Replay-only slots reserve replay RAM only while running (FR-004 edge case verified).

**Disposition — ACCEPT** (verified correct, no leak).

---

## F-HK — Inert hotkey output + two hotkeys: minimal legitimate idle retention (ACCEPT)

**Code**: `slot.cpp:1098-1160` (`register_hotkeys`: creates `hotkey_out_` via `obs_output_create("ffmpeg_muxer", …)`, never started; registers two hotkeys), `:1223-1240` (`unregister_hotkeys` releases all three), `manager.cpp:78` (`add_slot` registers immediately), `:101` (`remove_slot` unregisters).

**Observation**: Each configured slot holds, at idle: its `Config`, two registered hotkeys (record-toggle + save-replay — intentionally alive while stopped so the toggle works), and one inert `hotkey_out_` `ffmpeg_muxer` output whose **sole** purpose is to give the Settings→Hotkeys panel an object to group the two rows under the slot's name. This is the idle baseline the spec defines.

**Cross-reference FR-012 (per-frame callbacks against inactive outputs)**: `hotkey_out_` is created but `obs_output_start` is **never** called on it. An OBS output performs encode/mux work and receives raw-video/frame callbacks only between `obs_output_start` and stop; an output that is merely created has no active media thread and is not wired into any `video_t`'s raw-frame fan-out. Therefore the inert `hotkey_out_` (and any registered-but-inactive `rec_out_`/`replay_out_`, which at idle do not even exist) incur **no** per-frame/compositing callback. FR-012 is satisfied: a configured-but-not-running slot engages none of the host's compositing/encode pipeline.

**Why ACCEPT, not CLOSE**: Hotkeys are a shipped, user-configurable setting (Constitution IX); the grouping output is the libobs-sanctioned way to label a hotkey group (libobs has no `obs_output_set_name`, hence the destroy+recreate-on-rename design). The retention is one lightweight, never-started output per slot — the minimum necessary for the feature. It does not scale into pipeline cost and does not grow with anything but slot count.

**Disposition — ACCEPT** (minimum necessary, verified).

---

## F-CYCLE — No per-cycle accumulation across start/stop, rename, rebuild (ACCEPT)

**Code**: `slot.cpp:577-587` (`~SceneSlot`: `stop()` + `unregister_hotkeys()` + releases pending arrays), `:589-626` (`update_config`: rename = `capture` → `unregister` → `register`, preserving bindings), `:1164-1221` (binding capture/save/set helpers, each releasing stale arrays first), `manager.cpp:90-103` (`remove_slot`), `:595-658` (`load_from`: `slots_.clear()` destroys old slots before rebuild).

**Observation**:
- **Start/stop cycles** return to baseline each time (F-SHOW/F-PIPE/F-OUT all release on `teardown_locked`).
- **Rename** (`update_config`) destroys and recreates `hotkey_out_` to refresh the group label; `unregister_hotkeys` releases the old output and `register_hotkeys` re-applies the captured binding. Every binding-array path releases any stale `pending_*` before overwriting (`:1169-1176`, `:1215-1218`), and `~SceneSlot` releases any unconsumed pending array (`:583-586`) — no per-rename leak of outputs or `obs_data_array_t`.
- **Scene-collection rebuild** (`load_from`) clears `slots_`, destroying each `SceneSlot` (→ `stop()` + `unregister_hotkeys()`); on the `SCENE_COLLECTION_CHANGING` path the slots were already stopped/unregistered (`manager.cpp:676-681`), so the dtor calls are idempotent no-ops. Old idle resources are released, not orphaned.

**Disposition — ACCEPT** (verified correct, no leak). Backstopped by the quickstart 100-cycle leak check (SC-005) and rename check (FR-008).

---

## F-CB — Module-level callbacks registered once, not periodic (ACCEPT)

**Code**: `manager.cpp:9-13` (`init` adds `frontend_event_cb` + `save_cb`), `:15-37` (`shutdown` removes them), `plugin-main.cpp:34-56` (`obs_module_load`/`unload`; `dock_create_cb`).

**Observation**: The plugin registers exactly two `obs-frontend-api` callbacks plus one one-shot dock-create callback at module load. None is per-slot, none is periodic, and none does work unless the frontend fires the corresponding event (save, scene-collection change, exit). There is no module-level timer or background thread. Idle cost does not scale with the number of configured slots.

**Disposition — ACCEPT** (minimum necessary, verified).

---

## Pass log

Per FR-010, the audit is a single inspection pass; the only code change is one CLOSE.

- **Pass 1** — walked `slot.cpp`/`slot.hpp`, `manager.cpp`/`manager.hpp`, `ui-dock.cpp`, `plugin-main.cpp` for idle-reachable resource acquisitions. Surfaced eight findings (F-UD1, F-TIMER-INT, F-SHOW, F-PIPE, F-OUT, F-HK, F-CYCLE, F-CB). One CLOSE (F-UD1, `ui-dock.cpp`), one DEFER (F-TIMER-INT), six ACCEPT.
- **Contract-affecting check**: F-UD1 is internal to the Qt slot `on_stats_toggled`; it changes no exported symbol, no signature, and crosses no translation-unit boundary. **Zero contract-affecting changes** → no earlier file must be revisited. The audit **converges in one pass**.

## Decisions

- **Decision**: Fix F-UD1 by gating `on_stats_toggled`'s `stats_timer_->start()` on `SlotManager::any_running()`.
  **Rationale**: Restores the zero-idle-work invariant (FR-005/SC-001) the rest of the dock already honors via `refresh()`; one-line, thread-safe, no new API.
  **Alternatives considered**: (a) Have `on_stats_toggled` call `refresh()` instead of `refresh_stats()` — works (refresh() gates) but does more work than needed (full table rebuild) on a stats toggle; rejected as heavier. (b) Remove the manual start entirely and rely on the next `refresh()` — rejected because the user expects stats to begin immediately when a slot *is* already running at toggle time.

- **Decision**: DEFER the timer-interval configurability and per-tick `mtx_` reduction (F-TIMER-INT).
  **Rationale**: Both are *while-recording* costs, not idle costs; per the 2026-05-29 hybrid-scope clarification, acceptable-but-reducible costs are documented and handed to a follow-up rather than changed here.
  **Alternatives considered**: Fixing them now — rejected as out of this feature's idle scope and risk-without-benefit for the idle bar.
