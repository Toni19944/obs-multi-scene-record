---
description: "Task list for feature 008 — Replay buffer honors configured duration under quality-based rate control"
---

# Tasks: Replay buffer honors configured duration under quality-based rate control

**Input**: Design documents from `specs/008-fix-replay-quality-truncation/`

**Prerequisites**: plan.md ✓, spec.md ✓, research.md ✓, data-model.md ✓, contracts/replay-buffer-sizing.md ✓, quickstart.md ✓

**Tests**: Manual verification only (per plan.md "Testing" and quickstart.md). No automated test framework is requested or in scope for this plugin. The quickstart.md tests are run as the final validation phase, not as TDD-style task entries.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- File paths reference the repository root.

## Path Conventions

- **Project type**: Single-project native OBS Studio C++ plugin.
- Source: `src/slot.hpp`, `src/slot.cpp`, `src/manager.cpp`, `src/ui-slot-editor.hpp`, `src/ui-slot-editor.cpp`.
- Specs: `specs/008-fix-replay-quality-truncation/`.
- Polish: `CHANGELOG.md` at repository root.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: No project initialization required. The plugin's build system (CMake + `buildspec.json`) is already configured for OBS 31.1.1; this feature adds no new translation units, no new dependencies, and no CMake changes (plan.md § Scale/Scope).

- [x] T001 Verify the working tree is on branch `008-fix-replay-quality-truncation`, that the existing pre-008 build reproduces the headline bug (quickstart.md T1 step 6 — CQP-17 1080p60 40 s save produces a clip well under 40 s on high-motion content), and that feature 007's `on_replay_saved` / `log_replay_saved` machinery is in place at `src/slot.cpp` (this feature builds on top of 007). No code change.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Add the structural pieces that every user story depends on: (i) the pure `namespace replay_buffer_util` sizing helper with its per-mode branches and the platform-specific RAM probe; (ii) the new `replay_max_size_mb` field on `SceneSlot::Config`; (iii) its persistence in `manager.cpp`; (iv) the five new per-slot atomics on `SceneSlot` (`start_time_ns_`, `resolved_max_size_mb_`, `was_clamped_at_start_`, `replay_seconds_at_start_`, `assumed_kbps_at_start_`) and the new `observed_kbps_ewma_` stats member. Nothing in this phase changes runtime behaviour — the helper exists but isn't wired into `setup_outputs` yet (that's US1 / T010).

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [x] T002 [P] Add `uint32_t replay_max_size_mb = 0;` to `SceneSlot::Config` in `src/slot.hpp`, placed immediately after the existing `replay_seconds` field (lines around `slot.hpp:68`). Comment: `// FR-012: per-slot user override for the replay buffer's max_size_mb. 0 = auto-derived per replay_buffer_util::resolve_max_size_mb.` No other Config field is modified.

- [x] T003 [P] Add the five new per-slot atomics + EWMA member to `class SceneSlot` (private section) in `src/slot.hpp`. Members: `std::atomic<uint64_t> start_time_ns_{0};` (next to existing `running_` at `slot.hpp:274`), `std::atomic<uint64_t> resolved_max_size_mb_{0};`, `std::atomic<bool> was_clamped_at_start_{false};`, `std::atomic<uint32_t> replay_seconds_at_start_{0};`, `std::atomic<uint32_t> assumed_kbps_at_start_{0};`, and `double observed_kbps_ewma_ = 0.0;` placed next to existing `last_kbps_` at `slot.hpp:325` (guarded by `stats_mtx_`). Add `#include <atomic>` if not already present (it already is — `slot.hpp:5`). Comment each member with the FR it serves per data-model.md § New SceneSlot runtime state.

- [x] T004 [P] Add `namespace replay_buffer_util` declarations to `src/slot.hpp`, placed after the existing `namespace rc_util` block (`slot.hpp:352-374`) and following the same convention. Declarations:
  - `uint64_t estimated_kbps(const SceneSlot::Config &cfg, const struct EffectiveRC &eff);`
  - `uint64_t auto_derived_max_size_mb(const SceneSlot::Config &cfg, const struct EffectiveRC &eff);`
  - `uint64_t available_physical_mb();`
  - `uint64_t resolve_max_size_mb(const SceneSlot::Config &cfg, const struct EffectiveRC &eff, bool *out_was_clamped, uint64_t *out_requested_mb);`
  `estimated_kbps` is the per-mode per-second rate estimate (video + audio combined, in kbps), extracted as a sub-helper so the sizing helper (T005), the snapshot in `setup_outputs` (T010), and the L1 start-log line (T012) share **one source of truth** for the auto-derived bitrate assumption. Forward-declare `struct EffectiveRC` at the namespace boundary if not already in scope (it is forward-declared at `slot.hpp:14`).

- [x] T005 Implement `replay_buffer_util::estimated_kbps` and `replay_buffer_util::auto_derived_max_size_mb` in `src/slot.cpp` next to the existing `rc_util` block (around `slot.cpp:20-56`). Per contracts § Contract 1 + research D1.

  **`estimated_kbps(cfg, eff)`** — select branch via `rc_util::is_bitrate_based(eff.mode)` and `rc_util::is_lossless(eff.mode)`; compute per-second video bits as `eff.value * 1000` (bitrate) / `0.55 * cfg.width * cfg.height * fps` (quality) / `8.0 * cfg.width * cfg.height * fps` (lossless), where `fps = (double)cfg.fps_num / cfg.fps_den`. Add per-second audio bits `cfg.audio_bitrate * popcount(cfg.audio_tracks) * 1000`. Return `(uint64_t)((video_bps + audio_bps) / 1000.0)` — the combined per-second rate as integer kbps. Pure function. (The `0.55` quality coefficient is calibrated to clear the maintainer's reference scenario — CQP-17 at 1080p60, ~600 MB needed for a 40 s buffer — by ~9% headroom; see research.md D1.)

  **`auto_derived_max_size_mb(cfg, eff)`** — thin wrapper: `return (uint64_t)estimated_kbps(cfg, eff) * cfg.replay_seconds * 2 / (8 * 1024);` (kbps × seconds × 2× margin / 8 / 1024 = MB). Pure function.

  No logging, no plugin locks, no globals. Include `<cstdint>` if needed. The `popcount` of `cfg.audio_tracks` MUST follow the existing convention at the pre-008 `slot.cpp:898` audio-overhead line (verify it on the branch baseline — likely `__builtin_popcount` / `__popcnt` per compiler, or an existing helper in the codebase; keep one technique consistent with what's already there rather than introducing `std::popcount` which is C++20 and out of scope per Constitution Build & Platform Requirements).

- [x] T006 Implement `replay_buffer_util::available_physical_mb` in `src/slot.cpp` next to `auto_derived_max_size_mb`. Per research D3: prefer libobs's `os_get_sys_free_size` from `util/platform.h` if available (one cross-platform call, returns bytes — divide by `1024*1024`); if that wrapper does not exist or returns 0, fall back to platform-specific:
  - **Windows** (`#ifdef _WIN32`): `MEMORYSTATUSEX msex{sizeof(msex)}; GlobalMemoryStatusEx(&msex); return msex.ullAvailPhys / (1024*1024);` Requires `<windows.h>`.
  - **macOS** (`#elif __APPLE__`): `int64_t mem = 0; size_t sz = sizeof(mem); sysctlbyname("hw.memsize", &mem, &sz, nullptr, 0); return (uint64_t)mem / (1024*1024);` Requires `<sys/sysctl.h>`.
  - **Linux** (`#else`): `struct sysinfo si{}; sysinfo(&si); return (uint64_t)si.freeram * si.mem_unit / (1024*1024);` Requires `<sys/sysinfo.h>`.
  Pure-ish: no logging, no plugin locks; the OS calls are themselves stateless. Returns 0 on probe failure (caller treats 0 as "skip clamp — trust the formula").

- [x] T007 Implement `replay_buffer_util::resolve_max_size_mb` in `src/slot.cpp` next to the other helpers. Per contracts § Contract 1: compute `auto_mb = auto_derived_max_size_mb(cfg, eff)`; pick `requested_mb = (cfg.replay_max_size_mb > 0) ? cfg.replay_max_size_mb : auto_mb`; write `*out_requested_mb = requested_mb`; query `avail_mb = available_physical_mb()`; if `avail_mb > 0 && requested_mb > avail_mb / 2`, set `clamped_mb = avail_mb / 2` and `*out_was_clamped = true`, else `clamped_mb = requested_mb` and `*out_was_clamped = false`; if `clamped_mb < 50`, return `0` (sentinel for "decline replay buffer"); else return `clamped_mb`. Pure function: no logging, no plugin locks (the caller emits the FR-005 / FR-006 log lines from the `setup_outputs` call site).

- [x] T008 Add persistence of `replay_max_size_mb` to `src/manager.cpp`. In `slot_to_data` add `obs_data_set_int(d, "replay_max_size_mb", c.replay_max_size_mb);` immediately after the existing `replay_seconds` set at line 336. In `slot_from_data` add `c.replay_max_size_mb = (uint32_t)obs_data_get_int(d, "replay_max_size_mb");` immediately after the existing `replay_seconds` load at line 393. **No** explicit back-compat branch needed: `obs_data_get_int` returns 0 for absent keys, which is the auto-derived sentinel by design (research D2 / T002). Do NOT add a `replay_max_size_mb == 0 ? default` line — `0` IS the supported sentinel.

**Checkpoint**: Helpers compile and are callable; new Config field is persisted; new atomics exist on SceneSlot; nothing is wired into the runtime sizing yet. The pre-008 runtime behaviour is still in effect. Build the plugin — no behavioural change visible.

---

## Phase 3: User Story 1 — Saved replay length matches the configured replay length under quality-based rate control (Priority: P1) 🎯 MVP

**Goal**: Wire `replay_buffer_util::resolve_max_size_mb` into `SceneSlot::setup_outputs` so the replay buffer's `max_size_mb` is sized correctly for quality-based rate control. After this phase, a Save Replay against a slot configured for N seconds at any rate-control mode produces a clip approximately N seconds long, regardless of scene complexity. This is the headline fix.

**Independent Test**: quickstart.md T1 (CQP-17 1080p60 40 s save honors configured duration), T2 (scene-complexity invariance), T3 (bitrate-mode regression — no breakage on CBR path).

### Implementation for User Story 1

- [x] T009 [US1] Snapshot `start_time_ns_` in `SceneSlot::start()` in `src/slot.cpp`. Immediately after `setup_outputs` returns successfully (i.e., on the success path before `obs_output_start` is called for the recording/replay outputs), execute `start_time_ns_.store(os_gettime_ns(), std::memory_order_release);`. The store MUST happen before any output `start` call so that any signal callback (`on_replay_saved`, etc.) sees a populated value. This single line enables US3's FR-011 uptime check without coupling US1 to US3 implementation.

- [x] T010 [US1] Redirect the replay-buffer sizing block at `src/slot.cpp:885-908` to use `replay_buffer_util::resolve_max_size_mb`. Replace the existing inline computation (`uint32_t est_kbps = rc_util::is_bitrate_based(eff.mode) ? eff.value : 12000;` + popcount audio addition + `uint64_t max_size_mb = (uint64_t)est_kbps * cfg_.replay_seconds / 8 / 1024 * 3 / 2;` + `if (max_size_mb < 50) max_size_mb = 50;` + `obs_data_set_int(rb, "max_size_mb", ...);`) with:
  ```cpp
  bool was_clamped = false;
  uint64_t requested_mb = 0;
  uint64_t resolved_mb = replay_buffer_util::resolve_max_size_mb(
      cfg_, eff, &was_clamped, &requested_mb);
  if (resolved_mb == 0) {
      blog(LOG_ERROR, "[multi-scene-rec] '%s': replay buffer DECLINED — "
           "even the clamped ceiling falls below the 50 MB defensive floor; "
           "host has only %llu MB available physical memory. Slot will start "
           "without a replay buffer. Remedies: set 'Max replay buffer size (MB)' "
           "smaller, lower replay duration, lower quality, or switch to a "
           "bitrate-based rate-control mode.",
           cfg_.name.c_str(),
           (unsigned long long)replay_buffer_util::available_physical_mb());
      obs_data_release(rb);
      // Skip obs_output_create for the replay output; recording (if configured)
      // continues normally per FR-006.
      replay_out_ = nullptr;
  } else {
      // Populate the snapshot atomics for US3's FR-011 inference (no harm if
      // US3 hasn't been implemented yet — the values are simply unread).
      resolved_max_size_mb_.store(resolved_mb, std::memory_order_release);
      was_clamped_at_start_.store(was_clamped, std::memory_order_release);
      replay_seconds_at_start_.store(cfg_.replay_seconds, std::memory_order_release);
      // assumed_kbps snapshot (video + audio combined, in kbps) — read later by
      // L1 (T012 slot-start log), L3 (FR-014 augmented save log), and L4 (FR-011
      // hedged truncation warning). Single source of truth for the auto-derived
      // bitrate assumption: replay_buffer_util::estimated_kbps (declared T004,
      // implemented T005 — the same sub-helper auto_derived_max_size_mb calls
      // internally).
      assumed_kbps_at_start_.store(
          replay_buffer_util::estimated_kbps(cfg_, eff),
          std::memory_order_release);

      obs_data_set_int(rb, "max_time_sec", cfg_.replay_seconds);  // unchanged from existing line 904
      obs_data_set_int(rb, "max_size_mb", (long long)resolved_mb);

      // FR-005 / FR-006 log lines deferred to T013 (US2 phase); for now the
      // resolved value flows to OBS but is unannounced. US1's verification
      // (T1/T2/T3 in quickstart.md) doesn't depend on the log lines.
      // ... existing obs_output_create("replay_buffer", ...) and signal
      //     connects at slot.cpp:910-934 continue unchanged ...
  }
  ```
  Implements FR-001 / FR-001a / FR-001b / FR-002 / FR-003 / FR-004 / FR-007 / FR-009. The existing `obs_data_set_int(rb, "max_time_sec", cfg_.replay_seconds)` line is preserved verbatim per FR-009.

- [x] T011 [US1] Clear the snapshot atomics in `SceneSlot::teardown_locked` in `src/slot.cpp` (existing function around `slot.cpp:748`). After all outputs are released, add: `start_time_ns_.store(0, std::memory_order_release); resolved_max_size_mb_.store(0, std::memory_order_release); was_clamped_at_start_.store(false, std::memory_order_release); replay_seconds_at_start_.store(0, std::memory_order_release); assumed_kbps_at_start_.store(0, std::memory_order_release);` The clear MUST happen AFTER `signal_handler_disconnect("saved", ...)` returns (the 007 pattern — disconnect first, then the in-flight callback is guaranteed not running, then it's safe to clear the values the callback might have read).

**Checkpoint**: User Story 1 is functionally complete. Build the plugin. Run quickstart.md T1 (CQP-17 1080p60 reference scenario) — the saved clip MUST be approximately 40 s. Run T2 (low-complexity content) — the saved clip MUST also be approximately 40 s. Run T3 (bitrate-mode regression) — CBR path MUST still work. **This is the MVP scope.**

---

## Phase 4: User Story 2 — Per-slot memory use of the replay buffer is bounded and predictable (Priority: P2)

**Goal**: Surface the resolved per-slot ceiling to the user via slot-start log lines (FR-005), add the clamp-and-warn behaviour at slot start (FR-006), and expose the FR-012 per-slot override in the slot editor. After this phase, the user can predict per-slot memory commitment from the configuration and can dial the override when the auto-derived default doesn't suit their setup.

**Independent Test**: quickstart.md T4 (slot-start log shows resolved ceiling), T5 (multi-slot determinism), T7 (clamp-and-warn under low host RAM), T8 (override accepted and persisted), T9 (override below auto-derived → no warning), T10 (backwards-compat with older saves).

### Implementation for User Story 2

- [x] T012 [US2] Emit the FR-005 L1 log line at slot start. In `SceneSlot::setup_outputs` at `src/slot.cpp` (the success branch added in T010, immediately before `obs_data_set_int(rb, "max_size_mb", ...)`), add per contracts § C2 L1:
  ```cpp
  if (!was_clamped) {
      if (cfg_.replay_max_size_mb == 0) {
          blog(LOG_INFO, "[multi-scene-rec] '%s': replay buffer reserved %llu MB "
               "(auto-derived from %ux%u@%ufps %s; assumes %u kbps total, "
               "incl. %u kbps audio)",
               cfg_.name.c_str(), (unsigned long long)resolved_mb,
               cfg_.width, cfg_.height, cfg_.fps_num / cfg_.fps_den,
               eff.mode.c_str(), assumed_kbps_at_start_.load(),
               cfg_.audio_bitrate * popcount(cfg_.audio_tracks));
      } else {
          uint64_t auto_mb = replay_buffer_util::auto_derived_max_size_mb(cfg_, eff);
          blog(LOG_INFO, "[multi-scene-rec] '%s': replay buffer reserved %llu MB "
               "(user override; auto-derived would have been %llu MB)",
               cfg_.name.c_str(), (unsigned long long)resolved_mb,
               (unsigned long long)auto_mb);
      }
  }
  ```
  Implements FR-005 (both variants per contracts § C2 L1).

- [x] T013 [US2] Emit the FR-006 L2 clamp-and-warn line at slot start. In the same setup_outputs success branch, in the `if (was_clamped)` path, add per contracts § C2 L2:
  ```cpp
  if (was_clamped) {
      blog(LOG_WARNING, "[multi-scene-rec] '%s': replay buffer requested %llu MB "
           "but clamped to %llu MB (host has %llu MB available). Configured %u s "
           "replay duration will NOT be honored under typical bitrate; clip will "
           "be shorter than configured. Remedies: set 'Max replay buffer size (MB)' "
           "to a smaller explicit value to suppress this warning, OR lower the "
           "replay duration, OR lower the rate-control quality, OR switch to a "
           "bitrate-based rate-control mode.",
           cfg_.name.c_str(),
           (unsigned long long)requested_mb,
           (unsigned long long)resolved_mb,
           (unsigned long long)replay_buffer_util::available_physical_mb(),
           cfg_.replay_seconds);
  }
  ```
  Implements FR-006 clamp-and-warn variant. The decline (resolved_mb == 0) variant was already added in T010 inside the `if (resolved_mb == 0)` branch.

- [x] T014 [P] [US2] Add editor row widget member declarations to `src/ui-slot-editor.hpp` per research D7. In the `private:` section of `class SlotEditor`, add `QSpinBox* replay_max_size_spin_;` next to the existing `replay_secs_` declaration (search for `replay_secs_` declaration around the existing replay-related members) and `QLabel* replay_max_size_label_;` next to the existing `replay_mp4_warn_` declaration. Add the slot `void on_replay_max_size_inputs_changed();` to `private slots:`. Add `class QHBoxLayout;` to the forward-declarations at the top of the header if not already present.

- [x] T015 [US2] Instantiate the editor row in `src/ui-slot-editor.cpp` ctor. Immediately after the existing "Replay length" row at line 390-392 (the `replay_secs_ = new QSpinBox; ... form->addRow("Replay length", replay_secs_);` block), add:
  ```cpp
  replay_max_size_spin_ = new QSpinBox;
  replay_max_size_spin_->setRange(0, 65536);
  replay_max_size_spin_->setSuffix(" MB");
  replay_max_size_spin_->setSpecialValueText("Auto");
  replay_max_size_spin_->setValue((int)cfg_.replay_max_size_mb);
  replay_max_size_spin_->setToolTip(QString::fromUtf8(
      "Memory ceiling for the replay buffer.\n\n"
      "Empty / 0 (Auto): sized automatically from "
      "resolution × fps × replay seconds × 2× safety margin, "
      "calibrated for typical high-quality settings "
      "(around CQP-17 / CRF-18). The resolved value is shown "
      "alongside this field.\n\n"
      "Positive integer: overrides the auto-derived value verbatim. "
      "Set higher if you use an extreme-quality setting "
      "(e.g., CQP ≤ 12, CRF ≤ 14) and see \"suspected memory cap\" "
      "warnings in the log; set lower to cap RAM use at the cost of "
      "shorter saved clips."));
  replay_max_size_label_ = new QLabel;
  auto* mb_row = new QHBoxLayout;
  mb_row->addWidget(replay_max_size_spin_);
  mb_row->addWidget(replay_max_size_label_, /*stretch=*/1);
  form->addRow("Max replay buffer size", mb_row);
  ```
  Include `<QHBoxLayout>` at the top of the file if not present. Use the inline English literal as shown above — this matches the existing slot-editor tooltip convention at `src/ui-slot-editor.cpp:144 / 171 / 178 / 347 / 374`, none of which route through `obs_module_text(...)` (verified at the 008-fix branch base: `obs_module_text` is unused in `ui-slot-editor.cpp`, and `data/locale/en-US.ini` contains only the `multi_scene_record` plugin-display-name key, no editor-string keys). Do NOT add a locale key for this tooltip.

- [x] T016 [US2] Wire the reactive label updater in `src/ui-slot-editor.cpp` ctor. Immediately after T015's block, connect every input that affects the auto-derived value to `on_replay_max_size_inputs_changed`:
  ```cpp
  auto refresh_max_size_label = [this]() { on_replay_max_size_inputs_changed(); };
  connect(replay_max_size_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(replay_secs_,          QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(width_spin_,           QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(height_spin_,          QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(fps_num_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(fps_den_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  connect(rc_combo_,             QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, refresh_max_size_label);
  connect(rc_value_spin_,        QOverload<int>::of(&QSpinBox::valueChanged),
          this, refresh_max_size_label);
  refresh_max_size_label();  // populate the label with the initial resolved value
  ```
  Plan-phase verify the exact names of `width_spin_` / `height_spin_` / `fps_num_spin_` / `fps_den_spin_` / `rc_combo_` / `rc_value_spin_` in the editor header — these are the widget pointers that already drive the corresponding Config fields in `on_accept`.

- [x] T017 [US2] Implement `SlotEditor::on_replay_max_size_inputs_changed` in `src/ui-slot-editor.cpp`. The slot constructs a transient `SceneSlot::Config` from the current widget values (extract the construction from `on_accept` at lines 815-823 into a `cfg_from_widgets_for_preview()` helper that does NOT validate or commit — just builds the struct), resolves `EffectiveRC` (if `cfg.shared_encoder_slot_id` non-empty, call `SlotManager::instance().effective_rate_control(cfg.shared_encoder_slot_id)`; otherwise construct an `EffectiveRC{cfg.rate_control, cfg.rc_value, false, ""}`), calls `replay_buffer_util::resolve_max_size_mb(cfg, eff, &was_clamped, &requested_mb)`, and updates `replay_max_size_label_->setText(...)` via a strict precedence chain — the **first** matching branch wins:

  1. **Declined** (`resolved == 0`, most specific — fires regardless of override-set vs auto): `QString("(would be declined — host RAM too low)")`, plus `replay_max_size_label_->setStyleSheet("color: rgb(220, 140, 60);")` for the warning tint. Reset the stylesheet to empty on subsequent updates that don't hit this branch so the warning tint doesn't stick around when the user backs the values off.
  2. **User override set** (`resolved > 0 && cfg.replay_max_size_mb > 0`): `QString("(set: %1 MB — auto would be %2 MB)").arg(resolved).arg(replay_buffer_util::auto_derived_max_size_mb(cfg, eff))`. Also reset the stylesheet to empty (no warning tint).
  3. **Auto-derived** (`resolved > 0 && cfg.replay_max_size_mb == 0`, default): `QString("(auto: %1 MB)").arg(resolved)`. Also reset the stylesheet to empty.

  Implemented as `if (resolved == 0) { ... } else if (cfg.replay_max_size_mb > 0) { ... } else { ... }` so the branches are mutually exclusive and the declined-host-RAM case is checked first (it can co-occur with either of the other two — `resolved == 0` can happen whether or not the user set an override, so the declined branch MUST short-circuit). Constitution IV check: the slot calls pure functions in `replay_buffer_util` namespace and the existing `SlotManager::effective_rate_control` (a non-libobs accessor) — no `obs_*` API call from the editor.

- [x] T018 [US2] Persist `replay_max_size_mb` from the spinbox in `SlotEditor::on_accept` in `src/ui-slot-editor.cpp`. Immediately after the existing `cfg_.replay_seconds = (uint32_t)replay_secs_->value();` at line 823, add: `cfg_.replay_max_size_mb = (uint32_t)replay_max_size_spin_->value();` This is the only on_accept change; the spinbox's "Auto" special-value-text maps to integer `0` automatically (Qt convention), so the sentinel round-trips correctly.

**Checkpoint**: User Story 2 is functionally complete. Build the plugin. Run quickstart.md T4 (slot-start log shows resolved ceiling for three different configurations). Run T5 (two identical configurations report the same resolved ceiling — determinism). Run T7 (clamp-and-warn fires when configured 4K60 extreme exceeds 50% available RAM). Run T8 (override accepted, persists across editor close/reopen). Run T9 (override below auto-derived works without extra warning at start). Run T10 (older save without the `replay_max_size_mb` key loads with "Auto" and works correctly).

---

## Phase 5: User Story 3 — User can tell from the plugin log whether the replay buffer's memory ceiling caused early eviction (Priority: P3)

**Goal**: Add the FR-014 observed-bitrate sampling (via EWMA on the existing stats infrastructure) and the FR-011 hedged truncation warning in `log_replay_saved`. After this phase, when a save's duration is meaningfully shorter than configured due to the buffer's memory cap, the user sees a hedged warning line identifying the achieved vs configured duration, the observed bitrate, the assumed bitrate, and the resolved cap — with actionable remediation knobs.

**Independent Test**: quickstart.md T6 (FR-011 warning fires under cap-bound conditions), T11 (FR-014 observed-bitrate suffix in save log).

### Implementation for User Story 3

- [x] T019 [US3] Update `SceneSlot::stats()` in `src/slot.cpp` to maintain the EWMA per research D6. The existing `stats()` already computes `last_kbps_` from `obs_output_get_total_bytes` deltas under `stats_mtx_`; immediately after that computation (or just before `return` populates the result struct), add:
  ```cpp
  constexpr double alpha = 0.25;  // EWMA half-life ≈ 2.4 samples ≈ 2.4 s at ~1 Hz
  if (observed_kbps_ewma_ == 0.0)
      observed_kbps_ewma_ = last_kbps_;
  else
      observed_kbps_ewma_ = alpha * last_kbps_ + (1.0 - alpha) * observed_kbps_ewma_;
  ```
  Already under `stats_mtx_`; no new lock. Add a comment naming FR-014 / FR-011 as the consumers.

- [x] T020 [US3] Reset the EWMA in `SceneSlot::reset_stats_sampler` and `SceneSlot::teardown_locked` in `src/slot.cpp`. In `reset_stats_sampler` (existing helper declared at `src/slot.hpp:182`, implemented at `src/slot.cpp:1265`, invoked from `src/ui-dock.cpp:404` on slot restart — confirmed present at the 008-fix branch base): add `observed_kbps_ewma_ = 0.0;` next to the existing `last_kbps_ = 0.0;` reset at `slot.cpp:1270`, under the already-held `stats_mtx_` (acquired at `slot.cpp:1267`). In `teardown_locked` (alongside T011's atomic clears): the EWMA is implicitly reset by the next `start()` via `reset_stats_sampler`, so an explicit clear in `teardown_locked` is OPTIONAL — choose the clearer of the two for code-style consistency (the 007 pattern was to clear in teardown_locked; follow that).

- [x] T021 [US3] Add the FR-014 observed-bitrate suffix to the existing 007 `log_replay_saved` line in `src/slot.cpp` (currently at `slot.cpp:1166-1184`). Replace the existing `blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '%s'", cfg_.name.c_str(), path ...)` with:
  ```cpp
  // Snapshot the FR-014 / FR-011 inference inputs from the per-slot atomics
  // (T003 / T010 — populated at slot start; lock-free reads).
  uint64_t start_ns        = start_time_ns_.load(std::memory_order_acquire);
  uint64_t uptime_sec      = start_ns ? (os_gettime_ns() - start_ns) / 1000000000ULL : 0;
  uint64_t resolved_mb     = resolved_max_size_mb_.load(std::memory_order_acquire);
  bool     was_clamped     = was_clamped_at_start_.load(std::memory_order_acquire);
  uint32_t replay_seconds  = replay_seconds_at_start_.load(std::memory_order_acquire);
  uint32_t assumed_kbps    = assumed_kbps_at_start_.load(std::memory_order_acquire);

  // EWMA read under stats_mtx_ (the leaf-adjacent lock; OK from the mux thread
  // because it is independent of slot_mtx_ which the 007 callback avoided).
  double ewma_kbps = 0.0;
  {
      std::lock_guard<std::mutex> lk(stats_mtx_);
      ewma_kbps = observed_kbps_ewma_;
  }

  if (ewma_kbps > 0.0) {
      blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '%s' "
           "(observed %.0f Mbps, assumed %.0f Mbps)",
           cfg_.name.c_str(),
           (path && *path) ? path : "<unknown>",
           ewma_kbps / 1000.0,
           assumed_kbps / 1000.0);
  } else {
      blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '%s' "
           "(observed N/A, assumed %.0f Mbps)",
           cfg_.name.c_str(),
           (path && *path) ? path : "<unknown>",
           assumed_kbps / 1000.0);
  }
  ```
  Implements FR-014 + contracts § C2 L3.

- [x] T022 [US3] Emit the FR-011 hedged truncation warning (L4) in `log_replay_saved` in `src/slot.cpp`, immediately after T021's L3 line. Per contracts § C3 inference rule:
  ```cpp
  uint64_t needed_mb = (uint64_t)(ewma_kbps * replay_seconds / 8 / 1024);

  if (uptime_sec >= replay_seconds &&  // condition 1
      needed_mb   >  resolved_mb   &&  // condition 2
      !was_clamped                 &&  // condition 3
      ewma_kbps   >  0.0)               // condition 4
  {
      blog(LOG_WARNING, "[multi-scene-rec] '%s' replay save likely truncated to "
           "less than configured %u s: observed %.0f Mbps suggests buffer needed "
           "~%llu MB but resolved cap is %llu MB (auto-derived assumed %.0f Mbps); "
           "suspected memory cap (cause not directly confirmed). "
           "Consider setting 'Max replay buffer size (MB)' override, lowering "
           "replay duration, or lowering quality.",
           cfg_.name.c_str(), replay_seconds,
           ewma_kbps / 1000.0,
           (unsigned long long)needed_mb,
           (unsigned long long)resolved_mb,
           assumed_kbps / 1000.0);
  } else if (start_ns != 0 && uptime_sec < replay_seconds) {
      // Informational note when the buffer hasn't filled yet — distinct from
      // the warning. Don't fire if start_ns is 0 (already-stopped slot).
      blog(LOG_INFO, "[multi-scene-rec] '%s' note: slot uptime %llu s < configured "
           "replay %u s; saved file will be shorter than configured (this is "
           "expected — buffer hadn't filled).",
           cfg_.name.c_str(), (unsigned long long)uptime_sec, replay_seconds);
  }
  ```
  Implements FR-011 hedged-warning path + contracts § C2 L4 + C3 inference. The four conditions together prevent the three documented false-positive cases (slot-just-started, clamp-already-warned, EWMA-uninitialized).

**Checkpoint**: User Story 3 is functionally complete. Build the plugin. Run quickstart.md T6 (force cap-bound truncation via 100 MB override on the CQP-17 reference scenario; warning MUST fire with all four hedged-wording fields populated). Run T11 (verify the L3 line includes the observed-bitrate suffix after the EWMA has accumulated samples; verify "N/A" appears immediately after slot restart).

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation entries and end-to-end validation.

- [x] T023 [P] Add `CHANGELOG.md` entries under the existing `[Unreleased]` section per Constitution § Patch notes. Under **Fixed**: `Replay buffer no longer truncates to inconsistent short durations when the rate-control mode is quality-based (CQP / CRF / CQ / ICQ / QVBR / Lossless). The buffer's memory ceiling now scales with resolution × fps × per-mode bits-per-pixel-per-frame estimate × 2× margin, so configured replay duration is honored under typical scene complexity. (specs/008-fix-replay-quality-truncation)` Under **Added**: `Per-slot "Max replay buffer size (MB)" override in the slot editor — empty / "Auto" uses the new auto-derived ceiling; a positive value bypasses auto-derivation and is used verbatim. Editor shows the currently resolved ceiling alongside the override field. (specs/008-fix-replay-quality-truncation)` Under **Changed**: `Replay-save log line now includes observed bitrate alongside the auto-derived bitrate assumption. When the buffer's memory ceiling appears to have caused early eviction, a hedged warning identifies the achieved vs configured duration and names remediation knobs. (specs/008-fix-replay-quality-truncation)`

- [x] T024 Build the plugin from this branch against OBS 31.1.1 (`buildspec.json`); ensure the `check-format` CI step passes locally before committing. No new format violations.

- [x] T025 Run the full quickstart.md verification: T1, T2, T3 (US1 — MVP); T4, T5, T7, T8, T9, T10 (US2); T6, T11 (US3). All 11 tests MUST pass on the maintainer's Windows reference hardware. Any failing test is a ship-blocker for this feature.

---

## Dependencies & Execution Order

### Phase dependencies

- **Setup (Phase 1)**: trivial; one verification task, no code.
- **Foundational (Phase 2)**: T002-T008. T002 / T003 / T004 are parallelizable (different declaration sites in `slot.hpp`). T005, T006, T007 are sequential in `slot.cpp` (they live in the same source area, share helper utilities, and T007 calls T005 + T006). T008 is parallelizable with T002-T007 (different file: `manager.cpp`). **Phase 2 BLOCKS all user stories.**
- **User Story 1 (Phase 3)**: T009, T010, T011 — sequential in `slot.cpp` (T010 introduces the snapshot atomics that T011 clears; T009 / T010 are in different functions but reference each other's wiring).
- **User Story 2 (Phase 4)**: T012, T013 are sequential with each other (both in `setup_outputs` success branch; T013's clamp-warn replaces / extends T012's L1 line). T014 is parallelizable with T012/T013 (different file: `ui-slot-editor.hpp`). T015, T016, T017, T018 are sequential in `ui-slot-editor.cpp` (T015 creates widgets, T016 wires connects, T017 implements the reactive helper, T018 wires on_accept). **Phase 4 depends on Phase 3 only in that its log-line tests (T4/T5/T7) need the resolved_mb path from T010 to be live.**
- **User Story 3 (Phase 5)**: T019 / T020 in `stats()`-side; T021 / T022 in `log_replay_saved` side. T021 must precede T022 (T022 reads the snapshot computed in T021's preamble; structurally they're in the same function). **Phase 5 depends on Phase 3 (T010's snapshot atomics) and Phase 4 (T013's was_clamped suppression rule referenced in T022's condition 3).**
- **Polish (Phase 6)**: T023 is parallelizable with anything (different file: `CHANGELOG.md`). T024 / T025 are the final gate.

### User story dependencies

- **US1 (P1)**: depends on Phase 2 (the sizing helper + Config field + atomics). Standalone after Phase 2 completes; T1/T2/T3 in quickstart can pass without US2 or US3 implementations.
- **US2 (P2)**: depends on Phase 2 and US1's T010 (the log lines describe the resolved ceiling computed in T010). Standalone in terms of its tests (T4/T5/T7/T8/T9/T10 don't depend on US3).
- **US3 (P3)**: depends on Phase 2 and US1's T010 (the FR-011 inference reads the snapshot atomics populated in T010). Also depends on US2's T013 because T022's condition 3 (`!was_clamped_at_start_`) suppresses the warning when the clamp-warn from L2 already fired. Stronger ordering than US2 — US3 should be implemented after US2.

### Parallel opportunities

- Phase 2 T002 / T003 / T004 / T008 can be done in parallel (different files / different declaration sites). T005 / T006 / T007 are sequential within `slot.cpp` but can be done in parallel with T008 (manager.cpp).
- Phase 4 T014 (ui-slot-editor.hpp) is parallel with Phase 4 T012 / T013 (slot.cpp).
- Phase 6 T023 is parallel with all earlier phases (CHANGELOG.md only).

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Complete Phase 1 (T001) — verify branch and baseline.
2. Complete Phase 2 (T002-T008) — foundational helpers and Config field.
3. Complete Phase 3 (T009-T011) — wire the sizing helper into `setup_outputs`.
4. **STOP and VALIDATE**: run quickstart.md T1 / T2 / T3 — the headline bug MUST be fixed.
5. Demo-ready: a fix that delivers US1 alone is shippable as a bug-fix release. US2 and US3 add visibility and diagnostics on top, but US1 alone makes the maintainer's repro disappear.

### Incremental delivery

1. Phase 2 → foundational ready.
2. Phase 3 → US1 fixed → MVP shippable.
3. Phase 4 → US2 added (editor override + slot-start log lines + clamp-and-warn) → shippable improvement.
4. Phase 5 → US3 added (observed-bitrate diagnostic + hedged truncation warning) → final shippable.
5. Phase 6 → CHANGELOG + full quickstart sweep.

### Sequential (single-developer) strategy

T001 → T002, T003, T004, T008 (parallelizable but a single dev does them in any order) → T005 → T006 → T007 → T009 → T010 → T011 → quickstart.md T1/T2/T3 to validate MVP → T012 → T013 → T014 → T015 → T016 → T017 → T018 → quickstart.md T4/T5/T7/T8/T9/T10 to validate US2 → T019 → T020 → T021 → T022 → quickstart.md T6/T11 to validate US3 → T023 → T024 → T025.

---

## Notes

- This feature builds on top of feature 007's `on_replay_saved` / `log_replay_saved` machinery (T021 / T022 modify the existing `log_replay_saved` body). Verify 007 is committed and the `saved` signal subscription is in place before starting Phase 5.
- For consumer slots, the existing feature 006 wiring (`eff.mode` / `eff.value` passed into `setup_outputs`) flows directly into the new sizing helper — no new consumer-resolution code path is introduced. T010 just passes `eff` through to `replay_buffer_util::resolve_max_size_mb`.
- The platform-specific RAM probe in T006 has three `#ifdef` branches; if the build fails on macOS or Linux due to a header difference, prefer the libobs `os_get_sys_free_size` wrapper (research D3 alternative note) which is one cross-platform call.
- The five new atomics on SceneSlot (T003) all live next to existing members; no struct layout-sensitive ordering required, but place them in the same private-section block as the related existing members for readability.
- All [P] tasks operate on different files or independent declaration sites; conflicting [P] tasks have been left sequential.
- Verify the working tree is clean and the build passes after EACH checkpoint (end of Phase 2, end of Phase 3, end of Phase 4, end of Phase 5) — commit at each checkpoint per the Constitution § Commit hygiene.
- Avoid: editing `setup_outputs` while in the middle of editing `log_replay_saved` (both reference the snapshot atomics from T010; finish T010 fully before starting T021).
