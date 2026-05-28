# Phase 1 — Data Model

**Feature**: Replay buffer honors configured duration under quality-based rate control

**Branch**: `008-fix-replay-quality-truncation` | **Date**: 2026-05-25

---

## Persisted slot-configuration fields

### New (added by this feature)

| Field | Type | Default | Read by | Persisted key | Notes |
|---|---|---|---|---|---|
| `replay_max_size_mb` | `uint32_t` | `0` (sentinel for "auto-derived") | `replay_buffer_util::resolve_max_size_mb`; editor's reactive label updater | `"replay_max_size_mb"` | The FR-012 user override. `0` → auto-derived per the formula in [research.md](./research.md) D1. Positive value → used verbatim as `max_size_mb`, subject to the D3 clamp. Editor renders `0` as "Auto" via `QSpinBox::setSpecialValueText`. Absent in older saves → reads as `0` (auto) via libobs convention; no back-compat branch needed. |

### Existing (unchanged in shape and meaning — read by the new sizing helpers)

| Field | Type | Read by | Notes |
|---|---|---|---|
| `width` | `uint32_t` | `replay_buffer_util::estimated_kbps` | Video output width in pixels. Multiplied by `height` × `fps` × per-mode bpp coefficient. |
| `height` | `uint32_t` | `replay_buffer_util::estimated_kbps` | Video output height in pixels. |
| `fps_num`, `fps_den` | `uint32_t`, `uint32_t` | `replay_buffer_util::estimated_kbps` | Framerate as rational; the formula uses `fps_num / fps_den`. |
| `rate_control` | `std::string` | `replay_buffer_util::estimated_kbps` (via `eff.mode` for consumer slots) | Selects which branch of the formula applies (bitrate / quality / lossless). |
| `rc_value` | `uint32_t` | `replay_buffer_util::estimated_kbps` (via `eff.value`, bitrate branch only) | For bitrate-based modes, used directly as the kbps estimate. Quality-based and lossless branches do NOT consume this field. |
| `audio_bitrate` | `uint32_t` | `replay_buffer_util::estimated_kbps` | Audio overhead per track in kbps. |
| `audio_tracks` | `uint32_t` | `replay_buffer_util::estimated_kbps` | Bitmask of enabled audio tracks. `popcount(audio_tracks)` × `audio_bitrate` = total audio overhead. |
| `replay_seconds` | `uint32_t` | `replay_buffer_util::auto_derived_max_size_mb`; OBS `max_time_sec` setting (unchanged from current) | Buffer length in seconds. Multiplied by per-second kbps to get total kilobits. |
| `replay_enabled` | `bool` | gate at `slot.cpp:886` (unchanged) | Whether the slot configures a replay-buffer output at all. |
| `shared_encoder_slot_id` | `std::string` | gate in editor's `on_replay_max_size_inputs_changed` (whether to consult `SlotManager::effective_rate_control` or use own rate_control / rc_value) | For consumer slots, the editor's auto-derived value uses the owner's effective rate-control per FR-007. |

**Note**: every other `Config` field on `slot.hpp` is untouched by this feature.

---

## Sizing helpers (introduced by this feature)

`namespace replay_buffer_util` in `slot.hpp` / `slot.cpp` provides four pure helpers. The signatures and full contract live in [contracts/replay-buffer-sizing.md](./contracts/replay-buffer-sizing.md) § Contract 1; the data-model role of each is:

| Helper | Role | Consumers |
|---|---|---|
| `estimated_kbps(cfg, eff)` | Single source of truth for the auto-derived bitrate assumption (video + audio combined, in kbps). Per-mode branch selection (bitrate / quality / lossless) lives here. | `auto_derived_max_size_mb` (sizing math); the L1 FR-005 start-of-slot log line (assumed-kbps field); the L3 FR-014 save-log suffix (`assumed Mbps` value); the L4 FR-011 hedged warning (`auto-derived assumed Mbps` clause). The snapshot is captured at slot start into the `assumed_kbps_at_start_` atomic so the save callback can read it lock-free. |
| `auto_derived_max_size_mb(cfg, eff)` | Thin wrapper over `estimated_kbps`: applies 2× safety margin and time multiplication, returns MB. | `resolve_max_size_mb`; editor's reactive label updater (to show the "would be" auto value alongside the user override). |
| `available_physical_mb()` | Platform-specific RAM probe (Win32 `GlobalMemoryStatusEx` / macOS `sysctlbyname` / Linux `sysinfo` — or libobs's `os_get_sys_free_size` if available). Stateless. | `resolve_max_size_mb` (clamp threshold); the L2 FR-006 clamp-warn log line (`host has N MB` clause); the FR-006 declined-replay log line. |
| `resolve_max_size_mb(cfg, eff, out_was_clamped, out_requested_mb)` | Entry point called from `setup_outputs`. Selects override vs auto, applies clamp + decline floor, reports request-vs-clamp via out-params. | `setup_outputs` (the redirect site at the pre-008 `slot.cpp:891-908` block); editor's reactive label updater. |

---

## Runtime structures (existing, consumed by the new code)

### `EffectiveRC` (forward-declared at `slot.hpp:14`, defined in `manager.hpp`)

| Field | Type | Notes |
|---|---|---|
| `mode` | `std::string` | The owner's effective rate-control mode (CBR / CQP / Lossless / etc.) for consumer slots; the slot's own `rate_control` for owner / standalone slots. Already wired in feature 006 FR-014 and consumed by `setup_outputs(const EffectiveRC& eff)` at `slot.cpp:891`. Unchanged by this feature; consumed verbatim by `replay_buffer_util::resolve_max_size_mb`. |
| `value` | `uint32_t` | The owner's effective rate-control value (kbps for bitrate-based, quality level for quality-based). Same as `mode` — feature 006 already wires this. |

`replay_buffer_util::resolve_max_size_mb(const Config& cfg, const EffectiveRC& eff)` reads `eff.mode` and `eff.value` directly (no further dereferencing). For owner / standalone slots `eff.mode == cfg.rate_control` and `eff.value == cfg.rc_value` by feature 006's invariant.

---

## New SceneSlot runtime state (added by this feature)

| Member | Type | Guarded by | Set in | Read in | Notes |
|---|---|---|---|---|---|
| `start_time_ns_` | `std::atomic<uint64_t>` | (atomic, lock-free) | `SceneSlot::start()` after `setup_outputs` succeeds, via `start_time_ns_.store(os_gettime_ns(), std::memory_order_release)` | `log_replay_saved` to compute slot uptime for FR-011 suppression rule (per [research.md](./research.md) D5 condition 1) | Cleared (`.store(0, std::memory_order_release)`) in `teardown_locked()` before output release. Lock-free read in the save callback (which holds no plugin locks — matches the 007 pattern). |
| `observed_kbps_ewma_` | `double` | `stats_mtx_` | `SceneSlot::stats()` on every sample (per [research.md](./research.md) D6) | `log_replay_saved` for the FR-014 observed-bitrate suffix and the FR-011 inference (per [research.md](./research.md) D4) | Reset to `0.0` in `reset_stats_sampler()` (so a slot restart starts EWMA fresh) and in `teardown_locked()`. |
| `resolved_max_size_mb_` | `uint64_t` | `slot_mtx_` | `SceneSlot::setup_outputs` after the clamp resolves (per [research.md](./research.md) D3) | `log_replay_saved` for the FR-011 inference (compares `needed_size_mb > resolved_max_size_mb_`) | Set inside the existing locked region in `setup_outputs`; read at save time via a brief `slot_mtx_` acquisition (matches the existing pattern for other slot_mtx_-guarded reads). Cleared in `teardown_locked()`. |
| `was_clamped_at_start_` | `bool` | `slot_mtx_` | `setup_outputs` after the clamp decision (true when the requested cap exceeded D3's threshold and was clamped) | `log_replay_saved` for the FR-011 suppression rule (condition 3 in [research.md](./research.md) D5: skip the warning if the clamp already warned the user at start) | Cleared in `teardown_locked()`. |

All four new members are per-slot. No cross-slot state. No new mutexes.

---

## State transitions

### Slot start — `setup_outputs(const EffectiveRC& eff)`

```
ENTER setup_outputs (slot_mtx_ held)
  ...
  if (cfg_.replay_enabled) {                              /* existing gate at slot.cpp:886 */
    auto_max_mb     = replay_buffer_util::auto_derived_max_size_mb(cfg_, eff);
    requested_mb    = (cfg_.replay_max_size_mb > 0)
                      ? cfg_.replay_max_size_mb
                      : auto_max_mb;
    avail_mb        = replay_buffer_util::available_physical_mb();
    threshold_mb    = avail_mb / 2;                       /* D3 clamp threshold */

    if (requested_mb > threshold_mb) {
      clamped_mb              = threshold_mb;
      was_clamped_at_start_   = true;
      blog(LOG_WARNING, ...  /* FR-006 clamp-and-warn line */);
    } else {
      clamped_mb              = requested_mb;
      was_clamped_at_start_   = false;
    }

    if (clamped_mb < 50) {                                /* D3 defensive floor */
      blog(LOG_ERROR, ...  /* FR-006 declined-replay line */);
      /* skip obs_output_create for replay; rec_out_ alone (if any) carries the slot */
    } else {
      resolved_max_size_mb_   = clamped_mb;
      blog(LOG_INFO, ...   /* FR-005 resolved-ceiling line: "replay buffer reserved N MB
                              (requested M MB, %s)", "auto-derived"|"user override", clamp note if any */);
      obs_data_set_int(rb, "max_size_mb", (long long)clamped_mb);
      /* rest of replay-buffer output creation per existing code at slot.cpp:910-934 */
    }
  }
  ...
EXIT setup_outputs

ENTER SceneSlot::start (after setup_outputs returns successfully)
  start_time_ns_.store(os_gettime_ns(), std::memory_order_release);
EXIT start
```

### Stats sampling — `stats()` (existing, augmented)

```
ENTER stats() (stats_mtx_ held)
  ... existing logic: read obs_output_get_total_bytes, compute delta, derive last_kbps_ ...

  /* NEW: maintain EWMA */
  const double alpha = 0.25;
  if (observed_kbps_ewma_ == 0.0)
    observed_kbps_ewma_ = last_kbps_;
  else
    observed_kbps_ewma_ = alpha * last_kbps_ + (1.0 - alpha) * observed_kbps_ewma_;

  ... existing return value population ...
EXIT stats()
```

### Save Replay — `log_replay_saved()` (existing from 007, augmented)

```
ENTER log_replay_saved (NO plugin locks — runs on mux worker thread per 007 D1)
  /* 007's existing logic: call get_last_replay proc, extract path string */
  path  = obs_output_get_proc_handler(replay_out_) ... get_last_replay ... calldata_string ...

  /* NEW: snapshot the per-slot inference inputs under stats_mtx_ + slot_mtx_ briefly */
  uint64_t uptime_sec      = (os_gettime_ns() - start_time_ns_.load(std::memory_order_acquire)) / 1000000000ULL;
  double   ewma_kbps       = /* read observed_kbps_ewma_ under stats_mtx_ */;
  uint64_t resolved_mb     = /* read resolved_max_size_mb_ under slot_mtx_ */;
  bool     was_clamped     = /* read was_clamped_at_start_ under slot_mtx_ */;
  uint32_t replay_seconds  = /* read cfg_.replay_seconds under slot_mtx_ */;
  uint32_t assumed_kbps    = /* recompute from D1 formula under slot_mtx_ */;
  uint64_t needed_mb       = (uint64_t)(ewma_kbps * replay_seconds / 8 / 1024);

  /* Spec FR-012 (007): truthful success line, unchanged */
  blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '%s' (observed %.0f Mbps, assumed %.0f Mbps)",
       cfg_.name.c_str(), path, ewma_kbps / 1000.0, assumed_kbps / 1000.0);

  /* FR-011 hedged warning (D4 / D5 inference) */
  if (uptime_sec >= replay_seconds &&
      needed_mb   >  resolved_mb   &&
      !was_clamped &&
      ewma_kbps   >  0.0)
  {
    blog(LOG_WARNING,
         "[multi-scene-rec] '%s' replay save likely truncated to less than configured %us: "
         "observed %.0f Mbps suggests buffer needed ~%llu MB but resolved cap is %llu MB "
         "(auto-derived assumed %.0f Mbps). Consider setting 'Max replay buffer size (MB)' "
         "override, lowering replay duration, or lowering quality.",
         cfg_.name.c_str(), replay_seconds, ewma_kbps / 1000.0,
         needed_mb, resolved_mb, assumed_kbps / 1000.0);
  } else if (uptime_sec < replay_seconds) {
    blog(LOG_INFO,
         "[multi-scene-rec] '%s' note: slot uptime %llu s < configured replay %u s; "
         "saved file will be shorter than configured (this is expected — buffer hadn't filled).",
         cfg_.name.c_str(), uptime_sec, replay_seconds);
  }
EXIT log_replay_saved
```

### Teardown — `teardown_locked()` (existing, augmented)

```
ENTER teardown_locked (slot_mtx_ held)
  ... existing logic at slot.cpp:748-735 (signal disconnects, output stops, releases) ...

  /* NEW: clear inference state so a subsequent restart doesn't see stale data */
  start_time_ns_.store(0, std::memory_order_release);
  resolved_max_size_mb_  = 0;
  was_clamped_at_start_  = false;
  /* observed_kbps_ewma_ is reset under stats_mtx_ by reset_stats_sampler() — same as
     the existing last_kbps_ reset; no change to that path */
EXIT teardown_locked
```

---

## Lock-order audit (per Constitution Principle III)

| Acquisition order | Purpose | Notes |
|---|---|---|
| `slot_mtx_` (in `setup_outputs`) | Compute clamp; set `resolved_max_size_mb_` / `was_clamped_at_start_`. | Existing lock; no new acquisitions. |
| `stats_mtx_` (in `stats()`) | Update `observed_kbps_ewma_`. | Existing lock; no new acquisitions. |
| `stats_mtx_` (in `log_replay_saved`, brief) | Read `observed_kbps_ewma_`. | New acquisition site, but reuses the existing leaf-adjacent lock; no order violation. |
| `slot_mtx_` (in `log_replay_saved`, brief) | Read `resolved_max_size_mb_`, `was_clamped_at_start_`, `cfg_.replay_seconds`, recompute `assumed_kbps`. | New acquisition site in the save callback. **The 007 callback takes NO plugin locks**; this is a deviation. See contracts § Threading for the resolution: pre-compute these values at slot start and cache them on lock-free atomics, so the save callback remains lock-free. Plan-phase: switch `resolved_max_size_mb_` to `std::atomic<uint64_t>` and `was_clamped_at_start_` to `std::atomic<bool>`; treat `cfg_.replay_seconds` as a constant-after-start snapshot cached in another atomic. This preserves the 007 callback's no-locks property. |
| `start_time_ns_` (atomic, all sites) | Read/write the slot's start timestamp. | Lock-free; no order constraint. |

The plan-phase resolution noted in the audit (promote `resolved_max_size_mb_` / `was_clamped_at_start_` to atomics, snapshot `cfg_.replay_seconds`) is captured in the contracts document.
