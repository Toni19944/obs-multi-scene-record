# Phase 0 — Research

**Feature**: Replay buffer honors configured duration under quality-based rate control

**Branch**: `008-fix-replay-quality-truncation` | **Date**: 2026-05-25

Eight design decisions resolved. No NEEDS CLARIFICATION items remain.

---

## D1 — Per-mode bits-per-pixel-per-frame coefficient calibration

**Decision**: Three branches in `replay_buffer_util::auto_derived_max_size_mb`, selected by the effective rate-control mode:

| Branch | Selector | Bitrate estimate (bits/sec) |
|---|---|---|
| Bitrate-based | `rc_util::is_bitrate_based(eff.mode) == true` | `eff.value × 1000` (i.e., the user-supplied kbps, unchanged from the pre-fix bitrate branch) |
| Quality-based | `!is_bitrate_based(eff.mode) && !is_lossless(eff.mode)` | `0.55 bpp × width × height × (fps_num / fps_den)` |
| Lossless | `rc_util::is_lossless(eff.mode) == true` | `8.0 bpp × width × height × (fps_num / fps_den)` |

The 2× safety margin (FR-001b) is applied to every branch identically: `est_bytes_per_sec × replay_seconds × 2`. The existing audio overhead (`audio_bitrate × popcount(audio_tracks) × 1000 / 8`) is added per-second before the multiplication by `replay_seconds`.

Final formula:

```
est_bits_per_sec  = per_branch_bps(eff, cfg)
                  + cfg.audio_bitrate * popcount(cfg.audio_tracks) * 1000
auto_max_size_mb  = est_bits_per_sec * cfg.replay_seconds * 2  /* margin */
                                       / 8 / 1024 / 1024
```

Calibration against the maintainer's CQP-17 1080p60 reference scenario (NVENC P5 / full-res-double-pass / high-quality, modern open-world TPS game observed peaking at 60+ Mbps):

- `0.55 × 1920 × 1080 × 60 = 68.4 Mbps` quality-mode estimate at 1080p60 — comfortably covers the observed 60+ Mbps peak.
- At 40 s × 2× margin: `68.4 × 40 × 2 / 8 = 684 MB` — ~14% above the spec clarification Q3 target of "~600 MB," providing headroom for the worst-case scene that the calibration scenario only captured approximately.
- Scaling to 1440p60: `0.55 × 2560 × 1440 × 60 = 121.6 Mbps` × 40 s × 2 / 8 = 1217 MB.
- Scaling to 4K60: `0.55 × 3840 × 2160 × 60 = 273.7 Mbps` × 40 s × 2 / 8 = 2737 MB (handled by clamp-and-warn per D3).

For lossless, the 8 bpp coefficient covers typical lossless H.264 / HEVC output at 1080p60 (~1 Gbps): `8.0 × 1920 × 1080 × 60 = 996 Mbps`. The lossless branch will almost always trigger clamp-and-warn at long replay_seconds (a 40 s lossless buffer at 1080p60 would request `996 × 40 × 2 / 8 = 9960 MB ≈ 10 GB`); the user receives the warning per FR-006 and can decide to use the FR-012 override, shorten replay_seconds, or switch to a quality-based mode.

**Rationale**: Per clarification Q3 (Option A — lock structure, defer numbers), the spec binds the per-mode branch structure, the 2× margin, and "scales with at least resolution × fps." The bits-per-pixel-per-frame heuristic is the simplest scaling that satisfies that contract. The `0.55` bpp quality-mode coefficient (raised from an initial calibration of `0.5`, which left only ~3% headroom over the spec's worked example — the L5 finding from the cross-artifact analysis) is now calibrated to clear the maintainer's reference scenario by ~14%, which absorbs measurement noise in the "60+ Mbps peak" observation and minor inter-encoder QP-scale variance. The flat coefficient is deliberately quality-value-insensitive: real bitrate scales meaningfully with quality value (CQP-17 produces ~4× the bitrate of CQP-29), but a quality-value-aware formula would require per-encoder calibration curves and introduce under-allocation failure modes at low quality values; the design prefers over-allocation at low quality values (silent-safe, just unused RAM) plus the FR-012 user override as the escape valve for extreme high-quality cases (CQP ≤ 12, CRF ≤ 14). The 8 bpp lossless coefficient is sized to cover typical lossless encoders (lossless H.264 at 1080p60 ≈ 1 Gbps); a tighter coefficient would silently truncate lossless buffers, a looser one would over-allocate and trigger clamp-and-warn more often than needed.

**Alternatives considered**:

- *Flat constants (e.g., 40 Mbps quality, 500 Mbps lossless from the user's original input)*: rejected in clarification Q3 because they don't scale to higher resolutions (the maintainer's 60 Mbps at 1080p60 already exceeds a flat 40 Mbps assumption).
- *Encoder-property introspection for the maximum quality-mode bitrate (`obs_property_int_max` on the rate-control value key)*: the value-key range is the quality value's range (e.g., CQP 0-51), not the bitrate the encoder produces at that quality. The encoder does not expose a "what bitrate will you produce at quality X for resolution Y" introspection. Not viable.
- *Two-pass dry-run encode to measure the actual bitrate before sizing the buffer*: way too expensive for slot-start latency; defeats the "start the slot quickly" UX.
- *Per-resolution-bracket table (e.g., 60 Mbps for ≤1080p60, 120 Mbps for ≤1440p60, 250 Mbps for ≤4K60)*: equivalent to the bpp heuristic with a step function instead of linear scaling; harder to maintain (every new resolution bracket needs a code change); rejected.

---

## D2 — New persisted field name, type, default, back-compat

**Decision**: Add `uint32_t replay_max_size_mb = 0` to `SceneSlot::Config` (at `slot.hpp` next to `replay_seconds`). Persistence via `obs_data_set_int(d, "replay_max_size_mb", c.replay_max_size_mb)` in `slot_to_data` (line 336 alongside `replay_seconds`) and `c.replay_max_size_mb = (uint32_t)obs_data_get_int(d, "replay_max_size_mb")` in `slot_from_data` (line 393 alongside `replay_seconds`). `0` is the sentinel for "use auto-derived." Absent in older saves → `obs_data_get_int` returns 0 by libobs convention → the field reads as the sentinel → auto-derived behavior applies. **No explicit back-compat branch needed.**

**Rationale**: Follows the exact same persistence pattern as every other Config field (the existing `slot_to_data` / `slot_from_data` already use this style for `replay_seconds`, `audio_bitrate`, etc.). The `0` sentinel is unambiguous because a real user-supplied override would always be ≥ 1 MB (a 0-MB ring buffer is nonsensical). The editor's spinbox uses Qt's `setSpecialValueText("Auto")` to render `0` as "Auto" in the UI per D7, completing the user-visible cycle.

**Alternatives considered**:

- *Negative sentinel (`int32_t replay_max_size_mb = -1`)*: requires a signed type for what is a memory size; introduces a "what if user sets a negative value via direct JSON edit" defensive branch. Rejected; `0` is clean.
- *Optional via separate `bool replay_max_size_override_enabled` field*: two fields where one suffices; two pieces of state to keep in sync. Rejected.
- *Default = the auto-derived value, recomputed at slot creation*: makes the persisted value go stale when resolution / fps / rate-control changes; would surprise the user ("I set this once and didn't touch it, why did it lose its meaning"). Rejected.

---

## D3 — Available-RAM probe + clamp policy

**Decision**: New per-platform helper `replay_buffer_util::available_physical_mb()` returning total available physical memory in MB:

- **Windows**: `MEMORYSTATUSEX msex{sizeof(msex)}; GlobalMemoryStatusEx(&msex); return msex.ullAvailPhys / (1024*1024);`
- **macOS**: `uint64_t mem = 0; size_t sz = sizeof(mem); sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0);` — note macOS does not expose a per-platform "available" figure as cleanly as Windows; use `host_statistics64(...HOST_VM_INFO64...)` to compute `free_count + inactive_count` × page size, fall back to `hw.memsize` / 2 if that fails.
- **Linux**: `struct sysinfo si{}; sysinfo(&si); return (uint64_t)si.freeram * si.mem_unit / (1024*1024);`

Clamp policy: compute `auto_max_size_mb` per D1. If `auto_max_size_mb > available_physical_mb() / 2` (i.e., the auto-derived ceiling would exceed half of available physical RAM), clamp to `available_physical_mb() / 2` and emit the FR-006 warning (request → clamp, named actionable knobs). If the clamped value falls below the defensive floor of 50 MB, decline the replay buffer entirely and emit the FR-006 error variant (per spec: "When even the defensive floor cannot be allocated, the replay buffer for that slot MUST be declined").

The same clamp applies to the user-supplied FR-012 override (per spec FR-013: "FR-006's clamp-and-warn path applies symmetrically").

**Rationale**: 50% of available physical RAM is a defensible upper bound for a single replay buffer — it leaves room for OBS itself, the operating system, other applications, AND other slots' buffers (each of which would also clamp independently). The 50 MB floor matches the existing pre-fix floor at `slot.cpp:907`; below that, the replay buffer is too small to be useful even for short replay_seconds at low bitrates.

The platform-specific helper is one function with three `#ifdef`-guarded implementations; ~30 LOC total. No new dependencies — each platform's API is in standard system headers (`Windows.h`, `sys/sysctl.h`, `sys/sysinfo.h`).

**Alternatives considered**:

- *No clamp; pass through whatever max_size_mb the formula computes; trust the user to read FR-005's log line*: rejected by clarification Q4 (chose clamp-and-warn over no-clamp).
- *Clamp at a fixed total RAM threshold (e.g., 4 GB per slot)*: doesn't scale to hosts with very different RAM. Rejected.
- *Probe via OBS's own memory introspection (`bnum_allocs()` etc.)*: those are allocator-internal counters, not host-RAM figures. Wrong abstraction.
- *libobs `os_get_sys_total_size` / `os_get_sys_free_size` (libutil)*: ACTUALLY — these exist (`util/platform.h`). Updating decision: prefer the libobs platform wrappers if available, fall back to direct platform calls only if those wrappers aren't suitable. Plan-phase verify-then-pick.

**Note** for implementation: at code-time, audit `.deps/obs-studio-31.1.1/libobs/util/platform.h` for `os_get_sys_total_size` / `os_get_sys_free_size`. If present, use them (one cross-platform call site, no `#ifdef` in our code); if not, fall back to the per-platform helpers above. Either path satisfies the spec contract.

---

## D4 — Cause-detection mechanism for FR-011

**Decision**: No clean OBS introspection exists for "buffer was at its `max_size_mb` ceiling at save time." The replay-buffer output (`obs-ffmpeg-mux.c`) does not expose a `get_current_bytes` or `get_buffer_fullness` proc / property. Therefore FR-011's cause-detection path is **not available**; FR-011 falls back to its threshold-fallback path (the hedged warning).

The inference rule (executed in `log_replay_saved` after the existing 007 `wrote '<path>'` line):

```
ewma_kbps              = current EWMA value from stats() (D6)
auto_assumed_kbps      = est_bits_per_sec / 1000   /* same formula as D1, recomputed for the log line */
resolved_max_size_mb   = (from setup_outputs at slot start, cached on SceneSlot)
needed_size_mb         = ewma_kbps * cfg_.replay_seconds / 8 / 1024
slot_uptime_sec        = (os_gettime_ns() - start_time_ns_) / 1e9

if (slot_uptime_sec >= cfg_.replay_seconds &&
    needed_size_mb   >  resolved_max_size_mb &&
    !was_clamped_at_start)
{
    emit hedged-warning log line:
      "[multi-scene-rec] '%s' replay save likely truncated: configured %us, observed %.0f Mbps,
       auto-derived assumed %.0f Mbps, resolved cap %llu MB; needed ~%llu MB. Consider setting
       replay_max_size_mb override, lowering replay duration, or lowering quality."
}
```

`was_clamped_at_start` is a per-slot bool set in `setup_outputs` when the clamp path executed; the FR-011 suppression rule from the spec ("MUST NOT fire when the buffer has been actively constrained by the FR-006 clamp path — that case has its own warning at slot start, repeating it on every save would be log spam") is honored by checking this flag.

**Rationale**: The OBS replay-buffer API surface was audited (the same audit pattern used in 007 D1 — read `.deps/obs-studio-31.1.1/plugins/obs-ffmpeg/obs-ffmpeg-mux.c` for the public signals/procs). The output exposes:

- `signal_handler_add(sh, "void saved()")` — already used by 007.
- `proc_handler_add(ph, "void save(...)")` — already used by 007 for triggering saves.
- `proc_handler_add(ph, "void get_last_replay(out string path)")` — already used by 007 for the path.

**No buffer-size, buffer-fullness, or eviction-count procs/signals exist.** Adding one would require an OBS patch (out of scope; this plugin is an OBS plugin, not an OBS fork). The FR-014 observed-bitrate sampling gives us the data we need to *infer* the cap-bound condition without introspection — the inference is hedged-wording-only per the spec, which is exactly what the threshold-fallback path of FR-011 (per clarification Q5) calls for.

**Alternatives considered**:

- *Compute the actual saved-file duration by demuxing the file post-save*: requires a demuxer library; adds dependency; latency unacceptable on Save Replay. Rejected.
- *Stat the saved file's size and divide by the assumed bitrate*: file size ÷ bitrate ≠ duration for VBR content. Misleading. Rejected.
- *Add a custom signal in our own code at the eviction site*: we don't own the eviction logic; it's inside libobs. Would require patching OBS. Rejected.
- *Heuristic on file size alone (e.g., warn if file size > 0.8 × max_size_mb)*: doesn't account for whether the duration was actually short. Rejected; the bitrate-inference is cleaner.

---

## D5 — FR-011 threshold + suppression rules

**Decision**: The hedged warning from D4 fires when **all** of the following hold:

1. `slot_uptime_sec >= cfg_.replay_seconds` — the buffer had time to fill (spec edge case: "false-positive suppression — MUST NOT fire when the slot just started").
2. `(ewma_kbps × replay_seconds / 8 / 1024) > resolved_max_size_mb` — by EWMA inference, the buffer would need more MB than the cap permits.
3. `!was_clamped_at_start` — the start-of-slot clamp warning already covered this case; per-save repetition is log spam (per spec FR-011 false-positive suppression).
4. `ewma_kbps > 0` — the EWMA has accumulated at least one sample; first save right after slot start gets no warning even if conditions 1–3 hold (the EWMA initial value of 0 makes (2) trivially false anyway, but stated explicitly for safety).

When condition (1) fails — the buffer hasn't been running long enough to fill — the save log emits the FR-014 observed-bitrate suffix but no warning. Wording: `"replay save wrote '<path>' (slot uptime %us < configured %us; no truncation inference)"`.

When the warning fires, it includes:
- saved file path (from the existing 007 `get_last_replay` proc),
- configured `replay_seconds`,
- observed EWMA kbps,
- auto-derived assumed kbps,
- resolved `max_size_mb`,
- inferred "needed" size in MB,
- list of remediation knobs (override / shorter duration / lower quality / bitrate-mode).

**Rationale**: The four conditions together prevent the three known false-positive cases (slot-just-started, clamp-already-warned, EWMA-uninitialized) while still firing on the genuine cap-bound truncation case. The condition-1 alternative wording (when uptime < replay_seconds) is informative without alarming — it tells the user "the buffer wasn't full, so this is expected."

**Alternatives considered**:

- *Threshold on a percentage of replay_seconds (e.g., uptime ≥ 0.9 × replay_seconds)*: introduces a magic number that has to be tuned; cleaner to use the strict ≥ replay_seconds threshold.
- *Always emit the warning when (2) holds, even if uptime is short*: false-positive prone; rejected.
- *Smaller wording inside the existing `wrote` line vs separate WARNING line*: separate WARNING line at LOG_WARNING level so the user's log filters / monitoring (if any) catch it correctly. The existing `wrote` line stays as the truthful-success line per 007 FR-012.

---

## D6 — Observed-bitrate sampling for FR-014

**Decision**: Reuse the existing `SceneSlot::stats()` infrastructure (`slot.hpp:172-180`, `slot.cpp` Stats sampler). The dock refreshes at ~1 Hz; each refresh calls `stats()` which already computes `kbps` from `obs_output_get_total_bytes` deltas. Add one new EWMA member to SceneSlot guarded by the existing `stats_mtx_`:

```cpp
// slot.hpp
double observed_kbps_ewma_ = 0.0;  // FR-014 / FR-011: smoothed observed bitrate.
                                   // Updated in stats() under stats_mtx_.

// slot.cpp, in stats() after computing the new sample's kbps:
constexpr double alpha = 1.0 / 4.0;  // EWMA half-life ≈ 3 samples (~3 s at 1 Hz).
if (observed_kbps_ewma_ == 0.0)
    observed_kbps_ewma_ = new_kbps;  // seed from first sample, no smoothing yet
else
    observed_kbps_ewma_ = alpha * new_kbps + (1.0 - alpha) * observed_kbps_ewma_;
```

The save-log enhancement in `log_replay_saved` (added in 007) reads the EWMA via a small accessor that takes `stats_mtx_` briefly. This call happens at user-action frequency (one per Save Replay), not on a hot path.

Half-life choice: `alpha = 0.25` gives a half-life of `log(0.5) / log(1 - 0.25) ≈ 2.4 samples`. At ~1 Hz sampling, that's ~2.4 s of smoothing — fast enough to track scene-complexity changes within a single replay_seconds window, slow enough to filter out per-second noise.

**Rationale**: No new sampler. No new thread. The existing stats infrastructure provides exactly the data FR-014 needs, plus the EWMA smoothing handles the per-sample noise that would otherwise make the FR-011 inference noisy. The `stats_mtx_` is already the leaf-adjacent lock for this kind of read; no lock-order risk.

**Alternatives considered**:

- *Sample more often (e.g., 10 Hz from a dedicated timer)*: more responsive but more locks taken; the 1 Hz dock-refresh cadence is fine for the FR-011 use case (which fires at save time, not per-frame).
- *Compute rolling average over a fixed window of N seconds instead of EWMA*: requires a circular buffer of samples; more state; EWMA is simpler and equivalent in practice.
- *Pull `obs_output_get_total_bytes` directly at save time and divide by uptime*: gives the all-time average bitrate, not the recent bitrate. The FR-011 inference wants the *current* bitrate (to predict whether the cap is being hit *now*); EWMA over recent samples is the right answer.

---

## D7 — Editor row layout + reactive label

**Decision**: Add one form-row to `ui-slot-editor.cpp` immediately after the existing "Replay length" row at line 392. Layout: an `HBox` containing a `QSpinBox` (range 0–65536, suffix " MB", `setSpecialValueText("Auto")` so `0` renders as the literal text "Auto") and a `QLabel` (initial text `"(auto: -- MB)"`, updated reactively).

```cpp
// In SlotEditor ctor, after replay_secs_ row:
replay_max_size_spin_ = new QSpinBox;
replay_max_size_spin_->setRange(0, 65536);
replay_max_size_spin_->setSuffix(" MB");
replay_max_size_spin_->setSpecialValueText("Auto");
replay_max_size_spin_->setValue((int)cfg_.replay_max_size_mb);
replay_max_size_label_ = new QLabel;

auto* row = new QHBoxLayout;
row->addWidget(replay_max_size_spin_);
row->addWidget(replay_max_size_label_);
form->addRow("Max replay buffer size", row);

// Reactive update — fired by anything that affects the auto-derived value
auto refresh_label = [this]() { on_replay_max_size_inputs_changed(); };
connect(replay_max_size_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(replay_secs_,          QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(width_spin_,           QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(height_spin_,          QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(fps_num_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(fps_den_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
connect(rc_combo_,             QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, refresh_label);
connect(rc_value_spin_,        QOverload<int>::of(&QSpinBox::valueChanged),
        this, refresh_label);
refresh_label();
```

The `on_replay_max_size_inputs_changed` slot:

1. Builds a transient `SceneSlot::Config` from the current editor widget values (the same construction the existing `on_accept` does at line 815-823; refactor that into a `cfg_from_widgets()` helper for reuse).
2. Resolves the slot's `EffectiveRC`: if `shared_encoder_slot_id` is non-empty (consumer), call `SlotManager::effective_rate_control` on the owner per the pattern at the existing inherited-row display; otherwise use the slot's own rate_control / rc_value.
3. Calls `replay_buffer_util::resolve_max_size_mb(cfg, eff)` — pure function — to compute the resolved cap.
4. Updates the label text:
   - When spinbox value is `0`: `"(auto: 622 MB)"` (the auto-derived value)
   - When spinbox value is > `0`: `"(set: 200 MB — auto would be 622 MB)"` (informational)

**Rationale**: Keeps the editor's UI/Logic Separation principle intact — the editor calls a pure free function in `slot.cpp`'s namespace and `SlotManager::effective_rate_control` (a non-libobs accessor). No `obs_*` calls from the editor. The reactive update covers every input that feeds the auto-derived formula, so the label is always in sync with the current widget state. The special-value-text "Auto" makes the `0` sentinel discoverable without a separate checkbox.

**Alternatives considered**:

- *Separate checkbox "Override max size"*: two widgets where one suffices; the spinbox's "Auto" special-value-text is the standard Qt idiom for this.
- *Stat the resolved value only on Save click, not reactively*: the user wouldn't see the auto-derived value while choosing replay_seconds / quality; defeats the discoverability purpose.
- *Open a separate dialog for advanced replay-buffer settings*: too much UI for one setting.

---

## D8 — Audio overhead under the new sizing path

**Decision**: Preserve the existing `est_kbps += cfg_.audio_bitrate * popcount(cfg_.audio_tracks)` addition (currently at `slot.cpp:898`) under all three branches of the new sizing helper. The addition happens to the `est_bits_per_sec` figure before the `× replay_seconds × 2 / 8 / 1024 / 1024` multiplication.

**Rationale**: The audio data lives in the same in-memory ring as video; sizing the ring for video bitrate alone would under-size it by exactly the audio overhead. The existing addition is correct; the new sizing helper just preserves it. For typical configurations (one 160 kbps audio track) the audio overhead is ~160 kbps — negligible compared to video at 12+ Mbps but worth keeping the accounting correct.

**Alternatives considered**:

- *Drop the audio overhead because it's small*: rejected; correctness is correctness, the addition is one line.
- *Replace `popcount(audio_tracks)` with a more accurate per-track-bitrate sum*: today every track shares the same `audio_bitrate` (one persisted field), so the popcount approach is exact. If per-track bitrates ever become independent, this addition would need revisiting — but that's a separate feature.
