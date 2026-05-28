# Implementation Plan: Replay buffer honors configured duration under quality-based rate control

**Branch**: `008-fix-replay-quality-truncation` | **Date**: 2026-05-25 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/008-fix-replay-quality-truncation/spec.md`

## Summary

The bug ([spec.md](./spec.md) US1) is at `src/slot.cpp:891`: the per-slot replay buffer's `max_size_mb` is computed from `est_kbps = is_bitrate_based(eff.mode) ? eff.value : 12000` — a flat 12 Mbps assumption for every quality-based rate-control mode (CQP, CRF, ICQ, CQ, QVBR, Lossless). Real per-mode bitrate at typical quality settings is 2–80× that constant. The user's reference scenario (CQP-17 at 1080p60 on NVENC P5 / full-res-double-pass / high-quality, running a modern open-world third-person shooter) peaks at 60+ Mbps. For a 40 s replay window at 30 Mbps real bitrate, the computed cap `12 × 40 × 1.5 / 8 ≈ 87 MB` fills in ~23 s of wall time, after which eviction kicks in and `max_size_mb` becomes the binding constraint instead of `max_time_sec` — the saved file is whatever fits in 87 MB, varying with scene complexity. Hence "random short durations."

The fix is built around **four structurally-coupled changes** in `src/slot.cpp` / `src/slot.hpp` plus **one editor row** in `src/ui-slot-editor.cpp`:

- **Per-mode sizing helper** (`namespace replay_buffer_util` in `slot.hpp`, impl in `slot.cpp`): a small set of pure functions that compute the auto-derived `max_size_mb` from the slot's resolution × fps × per-mode bits-per-pixel-per-frame coefficient × 2× safety margin, plus the existing audio-bitrate × track-count addition. Three coefficients (one each for bitrate-based, quality-based, lossless) keep the structural-shape contract from clarification Q3 visible at one site. Implements FR-001 / FR-001a / FR-001b / FR-003 / FR-004.

- **Override field on `SceneSlot::Config`** (`slot.hpp`) + persistence (`manager.cpp`): one new `uint32_t replay_max_size_mb = 0` field. `0` is the sentinel for "use auto-derived." Persisted via `obs_data_set_int / obs_data_get_int`. Absent in older saves → defaults to `0` (auto), backwards-compatible. Implements FR-012 (persistence) / FR-013 (override semantics).

- **Sizing set-site redirect** (`slot.cpp:891-908`): replace the inline `est_kbps × replay_seconds × 1.5 / 8 / 1024` computation with a call to `replay_buffer_util::resolve_max_size_mb(cfg_, eff)` that returns the resolved ceiling (the override when set, otherwise the auto-derived value), clamped against available physical RAM (clamp-and-warn per clarification Q4). One log line at `obs_data_set_int(rb, "max_size_mb", ...)` reports the resolved ceiling and, when clamped, the request-vs-clamp pair with actionable next steps. Implements FR-005 / FR-006.

- **Observed-bitrate sampling for FR-014** (no new sampler — reuse existing stats infrastructure at `slot.hpp:178` `Stats::kbps`): the existing `stats()` reads `obs_output_get_total_bytes` deltas; FR-014 just exposes the rolling figure in the save log. The save-log line emitted from `log_replay_saved` (added in 007 at `slot.cpp:1166`) gains an "observed Z Mbps; auto-derived assumed W Mbps" suffix, derived from a small EWMA of stats-sampled kbps. When the suffix would indicate cap-bound truncation (per the FR-011 inference rule), the line emits the hedged warning per FR-011 instead of the truthful success line. Implements FR-014 + the FR-011 threshold-fallback path.

- **Editor row** (`src/ui-slot-editor.cpp:392`, immediately after the existing "Replay length" row): one `QSpinBox` for the override (range 0–65536 MB, suffix " MB", special-value-text "Auto" when 0), one adjacent `QLabel` showing `"(auto: N MB)"` or `"(set: N MB)"` updated reactively when the spinbox, replay-length spinbox, resolution, fps, or rate-control combo changes. The label calls into `replay_buffer_util::resolve_max_size_mb` (pure function, no libobs handles required for the auto-derived computation; the eff-resolution for consumer slots is fetched via `SlotManager` per the existing pattern at `ui-slot-editor.cpp` for the inherited rate-control display). Implements FR-012 (UI surface).

The fix is **mostly non-contract-affecting**:

- One new persisted per-slot field (`replay_max_size_mb`); backwards-compatible (defaults to 0/auto for older saves).
- The runtime `max_size_mb` computation changes shape; existing `max_time_sec` is untouched (FR-009).
- No new libobs APIs (the platform-specific physical-RAM probe is the only non-libobs system call; one wrapper per platform — Windows `GlobalMemoryStatusEx`, macOS `sysctl HW_MEMSIZE`, Linux `sysinfo()`).
- Pre-fix saved slots load and run with the new auto-derived ceiling instead of the old 12 Mbps fallback (the persisted slot data is identical; only the runtime sizing function changes).

Net code change (estimated): ~250 LOC added across 4 files (`slot.hpp`, `slot.cpp`, `manager.cpp`, `ui-slot-editor.cpp`); ~10 LOC removed; one new namespace (`replay_buffer_util`); one new `SceneSlot::Config` field; one new editor row + one new label; one new EWMA member on `SceneSlot` for the observed-bitrate smoothing; one new atomic `start_time_ns_` for the FR-011 wall-clock-uptime suppression. No new translation units. No CMake changes.

The full per-decision rationale and the API-surface audit are in [research.md](./research.md); the entity / field map and state transitions are in [data-model.md](./data-model.md); the sizing-contract and log-line contract are in [contracts/replay-buffer-sizing.md](./contracts/replay-buffer-sizing.md); the manual verification procedure is in [quickstart.md](./quickstart.md).

## Technical Context

**Language/Version**: C++17 (per constitution Build & Platform Requirements). No C++20 features introduced.

**Primary Dependencies**: libobs (`obs_data_set_int`, `obs_data_get_int`, `obs_output_get_total_bytes`, plus the existing `signal_handler_connect / _disconnect`, `proc_handler_call`, `calldata_*` already used in 007). No new libobs APIs. **Platform-specific dependencies for available-RAM probe**: Windows `GlobalMemoryStatusEx` (kernel32), macOS `sysctlbyname("hw.memsize")` (libSystem), Linux `sysinfo()` (libc). Each platform's API is in its standard system headers; no new third-party dependencies.

**Storage**: scene-collection JSON via libobs `obs_data_t`. **One new persisted per-slot key**: `replay_max_size_mb` (uint32_t, default 0 sentinel for "auto-derived"). Existing keys (`replay_enabled`, `replay_only`, `replay_seconds`, etc.) are unchanged in shape, default, and meaning. Older saves without `replay_max_size_mb` load with the auto-derived behavior (the default 0 sentinel matches `obs_data_get_int`'s zero-return for absent keys — no special back-compat branch needed).

**Testing**: manual verification per [quickstart.md](./quickstart.md). 11 tests covering US1 headline bug (T1), US1 scene-complexity invariance (T2), US1 bitrate-mode regression (T3), US2 deterministic memory commitment (T4), US2 multi-slot RAM accounting (T5), US3 cause-detection log (T6), FR-006 clamp-and-warn (T7), FR-012 override accepted (T8), FR-012 override below auto-derived (T9), FR-012 backwards-compat with older saves (T10), FR-014 observed-bitrate in save log (T11). Manual coverage is **Windows-only** (the maintainer's test environment); cross-platform correctness rests on the platform-agnostic nature of the sizing arithmetic and on the platform-specific RAM probe being a single-line wrapper per OS — same scoping as features 006/007.

**Target Platform**: Windows x64 (primary and the only manually-verified target — the bug repro is the maintainer's actual configuration: CQP-17 at 1080p60 on NVENC P5 / full-res-double-pass / high-quality, modern open-world TPS game). macOS (Xcode 16.0) and Ubuntu 24.04 receive the same code path; the per-platform RAM-probe wrapper is the only platform-specific code.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: The sizing function is one integer-arithmetic operation per slot start (called once in `setup_outputs`). The bitrate EWMA update runs at the existing dock-refresh cadence (~1 Hz from `dock_refresh_timer` driving `stats()`); the EWMA is a single multiply-add per sample. The save-log enhancement reads the EWMA once per Save Replay (user-action frequency). The editor row's reactive label-update fires on Qt spinbox/combo signals (user-interaction frequency). **No hot-path code is touched.**

**Constraints**:

- Constitution Principle III (lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf) — preserved. The new `replay_max_size_mb` field is part of `Config`, read inside `setup_outputs` under `slot_mtx_` (existing locking, no change). The EWMA member lives next to existing kbps stats and is guarded by `stats_mtx_` (existing leaf-adjacent lock). The new atomic `start_time_ns_` is lock-free. No new locks; no new threads.
- Constitution Principle IV (UI / Logic Separation) — preserved. The editor row reads/writes `cfg_.replay_max_size_mb` (a plain integer field on a plain struct) and calls `replay_buffer_util::resolve_max_size_mb` (pure function, no libobs handles needed for the auto-derived computation). For consumer slots, the editor fetches the owner's effective rate-control via the existing `SlotManager::effective_rate_control` helper (the pattern from 006's FR-005). The editor does NOT call any libobs API directly; the auto-derived value is a function of `Config` fields plus `EffectiveRC`, no `obs_*` calls.
- Constitution Principle V (Encoder Robustness & Graceful Fallback) — preserved. Encoder construction is untouched. When the encoder falls back to obs_x264/CBR/6000 kbps at `slot.cpp:430`, the resulting `eff.mode` / `eff.value` flow into `resolve_max_size_mb` as a bitrate-based mode and the bitrate branch sizes correctly from the 6000 kbps value (the fallback path is already correct under the new helper; not a regression).
- Constitution Principle VI (Pipeline Isolation From OBS Main) — preserved. No interaction with OBS main outputs.
- Constitution Principle VII (Recording & Replay Buffer Correctness, NON-NEGOTIABLE) — preserved and **strengthened**: this fix directly fulfills the principle's headline requirement ("A replay save MUST produce a file containing the slot's configured duration"). Today that requirement silently fails for every quality-based rate-control configuration; after this fix, it holds for typical configurations. The clamp-and-warn path is the explicit documented exception for hosts that cannot allocate the full auto-derived ceiling — the warning makes the degradation visible per FR-006 / FR-011, which itself satisfies the spirit of the principle (no silent corruption, no silent truncation).
- Constitution Principle VIII (Shared Encoder — Literal Semantics) — preserved. No encoder changes. For consumer slots, the per-slot replay-buffer sizing uses the owner's effective rate-control (already wired via the existing `eff` parameter from feature 006's FR-014).
- Constitution Principle IX (Configurable Settings Parity) — preserved AND extended. No setting is removed. **One new setting is added** — the per-slot `replay_max_size_mb` override (FR-012). The setting is optional (auto-derived default) so existing user workflows are unaffected; users who want explicit control gain it. The new editor row is one form-row addition with no relayout of existing rows.
- Patch notes: `CHANGELOG.md` will gain entries under `[Unreleased]` — under **Fixed** for US1 (replay-buffer truncation under quality-based rate control) and **Added** for FR-012 (per-slot "Max replay buffer size" override).

**Scale/Scope**: 4 source files modified — `slot.hpp` (declare `namespace replay_buffer_util` and add `replay_max_size_mb` to `Config`; add EWMA member; add `start_time_ns_` atomic), `slot.cpp` (implement `replay_buffer_util`; redirect sizing set-site; sample EWMA in `stats()`; emit FR-014 observed-bitrate in `log_replay_saved` and emit FR-011 hedged warning when inference indicates cap-bound truncation), `manager.cpp` (persist/load the override via `obs_data_set_int / obs_data_get_int`), `ui-slot-editor.cpp` (add one form-row with spinbox + label; wire reactive label updates). Estimated ~250 LOC added, ~10 LOC removed.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | PASS | Only existing libobs APIs used (`obs_data_set_int`, `obs_data_get_int`, `obs_output_get_total_bytes`). The platform-specific physical-RAM probe is OS-level (Win32 / sysctl / sysinfo), not OBS-internal; allowed per the constitution's scope (libobs / obs-frontend-api compliance is what is required; standard OS APIs are unrestricted). |
| II. Clear Ownership & Minimal Shared State | PASS | No cross-slot state added. The override is a per-slot Config field; the EWMA is a per-slot stats member; the start-time atomic is per-slot. The sizing helper is pure (reads Config + EffectiveRC by const-ref, returns integer). |
| III. Thread Safety (NON-NEGOTIABLE) | PASS | New fields: `Config::replay_max_size_mb` (read under existing slot_mtx_); EWMA (read/write under existing stats_mtx_); `start_time_ns_` (`std::atomic<uint64_t>`, lock-free). No new locks; no new threads. EWMA sampling reuses the existing `stats()` call path. See [contracts/replay-buffer-sizing.md](./contracts/replay-buffer-sizing.md) § Threading. |
| IV. UI / Logic Separation | PASS | The new editor row reads/writes `cfg_.replay_max_size_mb` (a POD integer) and calls `replay_buffer_util::resolve_max_size_mb` (pure free function). For consumer slots, the editor calls the existing `SlotManager::effective_rate_control` to fetch the owner's `EffectiveRC`. The editor does NOT call any libobs API directly. |
| V. Encoder Robustness & Graceful Fallback | PASS | Encoder construction at `slot.cpp:412-441` is untouched. The fallback path's `eff.value == 6000 kbps` flows through `resolve_max_size_mb`'s bitrate-based branch, producing the same sizing the existing code would have for an explicit CBR/6000 slot — no regression. |
| VI. Pipeline Isolation From OBS Main | PASS | No interaction with OBS main outputs. The per-slot replay-buffer output remains an isolated `obs_output_t`. |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | PASS, strengthened | This fix directly fulfills the principle's "A replay save MUST produce a file containing the slot's configured duration." Today, that requirement silently fails for every quality-based rate-control configuration (US1 in spec). After this fix, the configured duration is honored for typical configurations; the clamp-and-warn path (FR-006) is the explicit documented exception with a loud start-of-slot warning per FR-006 and a per-save hedged warning per FR-011 — both surface the degradation rather than silently corrupting it. Per-slot independence is unaffected (each slot's `max_size_mb` is computed and clamped independently). |
| VIII. Shared Encoder — Literal Semantics | PASS | No encoder changes. The sizing helper uses the owner's `EffectiveRC` for consumer slots (the existing `eff` parameter wired in feature 006's FR-014). |
| IX. Configurable Settings Parity | PASS, extended | One new setting (`replay_max_size_mb` override) is added per FR-012. No existing setting is removed or rendered inoperative. Existing user workflows that did not touch the new setting see the auto-derived default — which is the headline bug fix. |

**Result**: PASS. No Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/008-fix-replay-quality-truncation/
├── plan.md              # This file (/speckit-plan output)
├── spec.md              # Feature spec (/speckit-specify + /speckit-clarify output)
├── research.md          # Phase 0: design decisions + sizing-coefficient calibration
├── data-model.md        # Phase 1: Config field map + EffectiveRC consumption + state transitions
├── contracts/
│   └── replay-buffer-sizing.md  # Phase 1: sizing formula contract + log-line contract + threading
├── quickstart.md        # Phase 1: 11-test manual verification procedure
├── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
└── checklists/
    └── requirements.md  # Spec-quality checklist (from /speckit-specify)
```

### Source Code (repository root)

```text
src/
├── manager.hpp          # (unchanged)
├── manager.cpp          # TOUCHED: persist + load `replay_max_size_mb` via obs_data_set_int / obs_data_get_int next to existing `replay_seconds` (lines 336 and 393). One line added in each of slot_to_data and slot_from_data.
├── plugin-main.hpp      # (unchanged)
├── plugin-main.cpp      # (unchanged)
├── slot.hpp             # TOUCHED: add `namespace replay_buffer_util { bits_per_pixel_frame_for_mode, auto_derived_max_size_mb, available_physical_mb, resolve_max_size_mb }`; add `uint32_t replay_max_size_mb = 0` to `Config`; add `std::atomic<uint64_t> start_time_ns_{0}` to SceneSlot; add EWMA member next to existing kbps stats; declare `infer_cap_bound_truncation` helper.
├── slot.cpp             # TOUCHED: implement replay_buffer_util (per-mode bpp coefficients, auto-derived sizer, platform-specific physical-RAM probe, resolve helper); redirect replay-buffer sizing set-site at line 891-908 through resolve_max_size_mb; clamp-and-warn against available RAM; log resolved ceiling with request-vs-clamp pair when clamped; update start() to set start_time_ns_; update stats() to maintain EWMA; update log_replay_saved (added in 007 at line 1166) to emit FR-014 observed-bitrate suffix and FR-011 hedged warning when inference indicates cap-bound truncation; teardown_locked clears start_time_ns_.
├── ui-dock.hpp          # (unchanged)
├── ui-dock.cpp          # (unchanged)
├── ui-slot-editor.hpp   # TOUCHED: declare new QSpinBox* replay_max_size_spin_ and QLabel* replay_max_size_label_ members; declare on_replay_max_size_inputs_changed slot.
└── ui-slot-editor.cpp   # TOUCHED: instantiate the spinbox + label after the "Replay length" row at line 392; wire reactive label update on spinbox / replay-length / resolution / fps / rate-control changes; on_accept writes cfg_.replay_max_size_mb from the spinbox at line 823 alongside replay_seconds.
```

**Structure Decision**: Single-project OBS plugin. Four `.cpp`/`.hpp` files modified across five logical changes (sizing helper, override field + persistence, sizing set-site redirect, observed-bitrate sampling + FR-011 / FR-014 log emission, editor row). No new translation units, no CMake changes, no new third-party dependencies. One new persisted per-slot key (`replay_max_size_mb`); backwards-compatible with older saves via the `0` sentinel.

## Phase 0 — Research

The Phase 0 deliverable is [research.md](./research.md). Eight design decisions are resolved there with rationale + alternatives considered. No NEEDS CLARIFICATION items remain.

Topics covered:

- **D1**: Per-mode bits-per-pixel-per-frame coefficient calibration (quality / lossless / bitrate-based branches; resolution × fps scaling; calibration against the maintainer's CQP-17 1080p60 reference scenario).
- **D2**: New persisted field name, type, default sentinel, and back-compat handling for `replay_max_size_mb`.
- **D3**: Available-RAM probe — per-platform helper (Windows `GlobalMemoryStatusEx`, macOS `sysctlbyname`, Linux `sysinfo`), clamp threshold (fraction of available physical RAM), defensive floor (50 MB), decline-vs-clamp boundary.
- **D4**: Cause-detection mechanism for FR-011 — OBS API audit showed no `replay_buffer_get_current_bytes` introspection exists; falls back to FR-014 observed-bitrate inference with hedged wording.
- **D5**: FR-011 threshold + suppression rule (wall-clock uptime ≥ replay_seconds; bitrate-inference comparison; clamp-already-warned suppression).
- **D6**: Observed-bitrate sampling — reuse existing `stats()` infrastructure; EWMA smoothing parameters; the EWMA half-life trade-off.
- **D7**: Editor row layout — spinbox + label placement; reactive update wiring; consumer-slot behavior (shows owner's effective rate-control in the auto-derived computation).
- **D8**: Audio overhead — preserve existing `popcount × audio_bitrate` addition under the new sizing path; applies under all three branches.

## Phase 1 — Design & Contracts

**Prerequisites**: [research.md](./research.md) complete.

Phase 1 deliverables:

- [data-model.md](./data-model.md) — fields on `SceneSlot::Config` (one new: `replay_max_size_mb`; existing fields read by the sizing helper); the `EffectiveRC` struct (existing, unchanged) consumed by the helper; SceneSlot runtime state (existing + new EWMA + new `start_time_ns_` atomic); state transitions for the start-of-slot clamp-and-warn path and the save-time FR-011 inference.
- [contracts/replay-buffer-sizing.md](./contracts/replay-buffer-sizing.md) — the sizing function's input/output contract (per-mode branch selection, bpp coefficient, margin, audio overhead, clamp policy, defensive floor); the log-line contracts (FR-005 start-of-slot, FR-006 clamp-warn, FR-011 hedged save-time warning, FR-014 observed-bitrate suffix); the threading model (no new locks; EWMA under stats_mtx_; start-time atomic).
- [quickstart.md](./quickstart.md) — 11-test manual verification procedure on Windows with the maintainer's reference scenario.
- Updated agent context (CLAUDE.md) pointing at this plan.

Phase 1 does NOT create `tasks.md`; that is `/speckit-tasks`' output.
