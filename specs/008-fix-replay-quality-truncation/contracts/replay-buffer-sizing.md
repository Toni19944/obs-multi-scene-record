# Contracts — Replay Buffer Sizing + Truthful Save Logging

**Feature**: Replay buffer honors configured duration under quality-based rate control

**Branch**: `008-fix-replay-quality-truncation` | **Date**: 2026-05-25

This document specifies the four contracts the implementation MUST satisfy. Each contract is testable from the manual verification procedure in [quickstart.md](../quickstart.md) and traceable to FRs in [spec.md](../spec.md).

---

## Contract 1 — Sizing function (`replay_buffer_util::resolve_max_size_mb`)

**Signature**:

```cpp
namespace replay_buffer_util {
    uint64_t estimated_kbps(const SceneSlot::Config& cfg, const EffectiveRC& eff);
    uint64_t auto_derived_max_size_mb(const SceneSlot::Config& cfg, const EffectiveRC& eff);
    uint64_t available_physical_mb();
    uint64_t resolve_max_size_mb(const SceneSlot::Config& cfg,
                                 const EffectiveRC&       eff,
                                 bool*                    out_was_clamped,
                                 uint64_t*                out_requested_mb);
}
```

`resolve_max_size_mb` is the entry point called from `setup_outputs`. It returns the resolved cap to pass to `obs_data_set_int(rb, "max_size_mb", ...)`, writes whether the clamp fired to `*out_was_clamped`, and writes the pre-clamp requested value to `*out_requested_mb` for the FR-005 / FR-006 log lines.

`estimated_kbps` is a sub-helper extracted as the **single source of truth** for the auto-derived bitrate assumption (video + audio combined). It is consumed both by `auto_derived_max_size_mb` (sizing math) and at the call site to snapshot the assumed-kbps value for the FR-005 start-of-slot log line, the FR-014 save-log suffix, and the FR-011 hedged truncation warning — so the assumption reported to the user matches the assumption used in sizing.

**Inputs**: pure functions of `cfg` (Config) and `eff` (EffectiveRC). No globals, no logging, no plugin locks acquired by the sizing math itself (`available_physical_mb` is a stateless OS call).

**Per-mode branch selection** (FR-001 / FR-001a / FR-003) — `estimated_kbps` returns the per-second video kbps estimate, with audio added:

| Selector | Branch | Per-second video kbps estimate |
|---|---|---|
| `rc_util::is_bitrate_based(eff.mode)` | Bitrate-based | `eff.value` |
| `!rc_util::is_bitrate_based(eff.mode) && !rc_util::is_lossless(eff.mode)` | Quality-based | `0.55 * cfg.width * cfg.height * (cfg.fps_num / cfg.fps_den) / 1000.0` |
| `rc_util::is_lossless(eff.mode)` | Lossless | `8.0 * cfg.width * cfg.height * (cfg.fps_num / cfg.fps_den) / 1000.0` |

The bpp coefficients (`0.55` quality, `8.0` lossless) are calibrated in [research.md](../research.md) D1; they multiply pixels × fps to produce bits-per-second, which the `/ 1000.0` converts to kbps (matching libobs / OBS-code convention where `kbps` is bits-per-second / 1000).

**Audio overhead** (FR-008 → preserved per D8): add `cfg.audio_bitrate * popcount(cfg.audio_tracks)` (already in kbps — `cfg.audio_bitrate` is the per-track kbps) to the per-second kbps before the time multiplication.

**Safety margin** (FR-001b): `auto_derived_max_size_mb` multiplies by 2× after audio addition.

**Time multiplication**: multiply the resulting kbps by `cfg.replay_seconds`, then convert to MB via `/ 8 / 1024`. This matches the existing pre-008 convention at `slot.cpp:905` (`kbps × s / 8 / 1024 ≈ MB`, where the numerator's `kilo` is 1000 and the denominator's `kilo` is 1024 — a long-standing ~2.4% approximation in the OBS-code lineage). Concretely: `auto_derived_max_size_mb = estimated_kbps × cfg.replay_seconds × 2 / (8 × 1024)`.

**Override** (FR-012): if `cfg.replay_max_size_mb > 0`, replace the computed auto value with `cfg.replay_max_size_mb` (the user-supplied override is used verbatim, before clamping).

**Clamp** (FR-006 / FR-013, per [research.md](../research.md) D3): if the requested value exceeds `available_physical_mb() / 2`, clamp to that threshold and set `*out_was_clamped = true`. If the clamped value falls below the defensive floor (`50 MB`), return `0` to signal "decline replay buffer entirely" (caller checks for zero and skips `obs_output_create` for the replay output).

**Determinism** (FR-004): given identical `cfg`, `eff`, and host RAM, the function MUST return the same value. The function MUST NOT consume any runtime observation (no encoder stats, no observed bitrate, no clock).

**Postconditions**:

- Return value of `0` means "decline replay buffer" (FR-006 floor-failure case).
- Return value > `0` is the value to pass to OBS `max_size_mb`.
- `*out_requested_mb` is the pre-clamp value (auto-derived, OR override if set).
- `*out_was_clamped` is `true` iff the requested value was reduced by the clamp.

---

## Contract 2 — Log-line wording

**Four distinct log lines** are produced by this feature. Each has a fixed shape that the FR-005 / FR-006 / FR-011 / FR-014 acceptance tests verify.

### L1 — FR-005 (slot start, replay-buffer ceiling resolved, NOT clamped)

```
[multi-scene-rec] '<slot>': replay buffer reserved <N> MB (auto-derived from <res>@<fps>fps,
<rc-mode>; assumes <K> Mbps + <A> kbps audio)
```

Variant when override is set:

```
[multi-scene-rec] '<slot>': replay buffer reserved <N> MB (user override; auto-derived would
have been <M> MB)
```

LOG_INFO. Emitted once per slot start in `setup_outputs` after `resolve_max_size_mb` returns and BEFORE `obs_data_set_int(rb, "max_size_mb", ...)`. Required for every slot start that enables the replay buffer (FR-005).

### L2 — FR-006 (slot start, replay-buffer ceiling CLAMPED)

```
[multi-scene-rec] '<slot>': replay buffer requested <REQ> MB but clamped to <CLAMP> MB
(host has <AVAIL> MB available). Configured <SECS>s replay duration will NOT be honored
under typical bitrate; clip will be shorter than configured. Remedies: set 'Max replay
buffer size (MB)' to a smaller explicit value to suppress this warning, OR lower the
replay duration, OR lower the rate-control quality, OR switch to a bitrate-based rate-
control mode.
```

LOG_WARNING. Emitted when `*out_was_clamped == true`. Replaces L1 for the same slot start (the user gets exactly one line per start: L1 OR L2, never both).

Variant when the clamp falls below the 50 MB floor:

```
[multi-scene-rec] '<slot>': replay buffer DECLINED — even the clamped ceiling (<CLAMP> MB)
falls below the 50 MB defensive floor; host has only <AVAIL> MB available physical memory.
Slot will start without a replay buffer; the recording output (if configured) is unaffected.
Remedies: as for the clamp case.
```

LOG_ERROR. The slot starts without a replay buffer.

### L3 — Augmented 007 truthful-save line (FR-014)

The existing 007 line:

```
[multi-scene-rec] '<slot>' replay save wrote '<path>'
```

is augmented to:

```
[multi-scene-rec] '<slot>' replay save wrote '<path>' (observed <Z> Mbps, assumed <W> Mbps)
```

LOG_INFO. The `observed` figure is the EWMA from [research.md](../research.md) D6; the `assumed` figure is the per-second bitrate the sizing helper computed at slot start (or, for the bitrate branch, the kbps `eff.value`). When the EWMA hasn't accumulated a sample yet (very short slot uptime), `observed N/A` is emitted in place of the figure.

### L4 — FR-011 hedged truncation warning

Emitted from `log_replay_saved` immediately after L3, when all four inference conditions hold (per [research.md](../research.md) D5):

```
[multi-scene-rec] '<slot>' replay save likely truncated to less than configured <SECS>s:
observed <Z> Mbps suggests buffer needed ~<NEED> MB but resolved cap is <CAP> MB (auto-
derived assumed <W> Mbps). Consider setting 'Max replay buffer size (MB)' override,
lowering replay duration, or lowering quality.
```

LOG_WARNING. When uptime < replay_seconds (condition 1 fails), L4 is replaced by an informational note:

```
[multi-scene-rec] '<slot>' note: slot uptime <U>s < configured replay <SECS>s; saved file
will be shorter than configured (this is expected — buffer hadn't filled).
```

LOG_INFO. When `was_clamped_at_start_ == true` (condition 3 fails), L4 is suppressed entirely (L2's start-of-slot warning already covered the case; per-save repetition is log spam).

**Negative contract (FR-010)**: when none of the truncation-warning conditions hold (i.e., the save honored the configured duration), L3 is the ONLY line emitted. No L4. No "success" / "OK" wording beyond the existing "wrote '<path>'" from 007.

---

## Contract 3 — Inference rule for FR-011 (cap-bound truncation detection)

**Rule** (per [research.md](../research.md) D4 + D5):

```
emit_warning =
    slot_uptime_sec(now) >= cfg_.replay_seconds  &&    // condition 1: buffer had time to fill
    needed_size_mb       >  resolved_max_size_mb_  &&    // condition 2: EWMA inference says cap-bound
    !was_clamped_at_start_                         &&    // condition 3: clamp already warned at start
    observed_kbps_ewma_  >  0.0                          // condition 4: EWMA has at least one sample
where
    slot_uptime_sec(now) = (now - start_time_ns_) / 1_000_000_000
    needed_size_mb       = observed_kbps_ewma_ * cfg_.replay_seconds / 8 / 1024
```

Inputs at evaluation time:

- `start_time_ns_` — set in `start()` after `setup_outputs` succeeds; cleared in `teardown_locked()`. `std::atomic<uint64_t>`.
- `cfg_.replay_seconds` — snapshotted at slot start into a per-slot atomic (`replay_seconds_at_start_`) so the save callback can read it without taking `slot_mtx_`.
- `observed_kbps_ewma_` — read under `stats_mtx_` (the callback takes this briefly; it's the leaf-adjacent lock and the EWMA is updated under the same lock in `stats()`).
- `resolved_max_size_mb_` — atomic snapshot from `setup_outputs`.
- `was_clamped_at_start_` — atomic snapshot from `setup_outputs`.

**Determinism**: given identical inputs at the evaluation moment, the rule produces the same `emit_warning` answer. No randomness.

---

## Contract 4 — Threading model

**Lock acquisitions added by this feature**:

| Site | Lock | Duration | Notes |
|---|---|---|---|
| `setup_outputs` (existing) | `slot_mtx_` | Existing — feature only adds work inside this lock. Sets atomics from inside the lock; no new acquisition. | OK. |
| `stats()` (existing) | `stats_mtx_` | Existing — feature only adds a 2-line EWMA update inside this lock. | OK. |
| `log_replay_saved` (existing, 007) | `stats_mtx_` (NEW brief acquisition) | One read of `observed_kbps_ewma_`. | OK — `stats_mtx_` is the leaf-adjacent lock; the 007 callback's "no plugin locks" property was specific to `slot_mtx_` (to avoid blocking the mux worker thread on slot teardown); `stats_mtx_` is independent. |

**Atomics added by this feature** (no lock):

| Member | Type | Why atomic |
|---|---|---|
| `start_time_ns_` | `std::atomic<uint64_t>` | Read in `log_replay_saved` (mux worker thread) without holding `slot_mtx_`. Lock-free is the only way the 007 callback can compute slot uptime without re-introducing the `slot_mtx_` dependency that 007 explicitly avoided. |
| `resolved_max_size_mb_` | `std::atomic<uint64_t>` | Same reason — read in `log_replay_saved` without `slot_mtx_`. |
| `was_clamped_at_start_` | `std::atomic<bool>` | Same reason. |
| `replay_seconds_at_start_` | `std::atomic<uint32_t>` | Snapshot of `cfg_.replay_seconds` at slot start; lets `log_replay_saved` read it without `slot_mtx_`. |
| `assumed_kbps_at_start_` | `std::atomic<uint32_t>` | Snapshot of the auto-derived per-second kbps from sizing helper at slot start; reads in `log_replay_saved` for L3 / L4 wording without re-running the formula. |

All five atomics are per-slot. Set in `start()` / `setup_outputs`; cleared in `teardown_locked()`. The release-store at slot start happens BEFORE `obs_output_start` is called (so by the time any output signal fires, the atomics are populated); the cleared-store at teardown happens AFTER `signal_handler_disconnect` returns (per the 007 disconnect-before-release pattern, the callback is guaranteed not running before teardown clears the atomics).

**Lock order** (per Constitution Principle III): `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_`. This feature acquires `slot_mtx_` and `stats_mtx_` only in their existing positions; no new lock order edges are introduced.

**Reentrancy**: `log_replay_saved` runs on the OBS mux worker thread (per 007 D1). It takes `stats_mtx_` briefly; that lock is also taken by `stats()`, which runs on the dock-refresh timer (Qt main thread). The two acquisitions are independent (different threads, no nested acquisition); no deadlock risk.

**Race window for teardown**: `signal_handler_disconnect("saved", ...)` in `teardown_locked` blocks on any in-flight `log_replay_saved` dispatch (libobs's signal mutex serializes dispatch + disconnect, same as 007). The atomics being cleared AFTER the disconnect returns ensures any callback in flight saw the live values; any callback that starts AFTER the disconnect cannot happen (the signal handler is unregistered). No torn reads.

---

## Acceptance tests (mapped to [quickstart.md](../quickstart.md))

| Test | Contract verified |
|---|---|
| T1 — CQP-17 1080p60 40s save | C1 quality-mode branch + C3 inference + C2 L3 |
| T2 — Same slot, low-complexity content same configuration | C1 determinism + C3 inference must NOT fire (true negative) |
| T3 — Bitrate-mode regression | C1 bitrate-mode branch unchanged from pre-fix |
| T4 — Slot-start log shows resolved ceiling | C2 L1 |
| T5 — Multi-slot RAM accounting | C1 determinism × N slots |
| T6 — FR-011 truncation warning fires under cap-bound conditions | C2 L4 + C3 all four conditions |
| T7 — Clamp-and-warn under low host RAM | C2 L2 + C1 clamp |
| T8 — FR-012 override accepted | C1 override branch + C2 L1 user-override variant |
| T9 — FR-012 override below auto-derived | C1 override branch (no warning when smaller) |
| T10 — Pre-fix saved slot loads with auto-derived ceiling | C1 back-compat (replay_max_size_mb absent → 0 → auto) |
| T11 — Observed-bitrate suffix in save log | C2 L3 augmentation |
