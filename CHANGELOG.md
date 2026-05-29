# Changelog

All notable user-visible changes to the **Multi Scene Record** OBS plugin are
documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The canonical version is the `version` field in `buildspec.json`.

Per the project constitution (Development Workflow → "Patch notes"), every new
feature and every bug fix MUST add an entry here before being merged. Use the
following section headings as applicable:

- **Added** — new features.
- **Changed** — changes in existing functionality.
- **Deprecated** — soon-to-be-removed features.
- **Removed** — removed features.
- **Fixed** — bug fixes.
- **Security** — vulnerability fixes.
- **Performance** — measurable performance/stability improvements with no
  behaviour change.

## [Unreleased]

### Added

- Full codebase performance and stability audit report
  (`specs/012-idle-slot-resource-audit/audit-report.md`) covering all 11 source
  files. 27 findings across 4 categories: 1 Critical, 5 High, 10 Medium,
  11 Low. No source code modified (`012-idle-slot-resource-audit`).

### Fixed

- Hotkey and signal callbacks now hold weak_ptr instead of raw pointers,
  eliminating a potential use-after-free if a slot is removed while a callback
  is in flight (`013-audit-fixes`, S-002).

- `g_dock` now nulls immediately when Qt destroys the widget, preventing a
  dangling-pointer window during OBS shutdown (`013-audit-fixes`, S-001).

- `start_all`/`stop_all` snapshot shared_ptr copies so a concurrent
  `remove_slot` cannot destroy a slot mid-iteration (`013-audit-fixes`, S-003).

- `teardown()` releases `slot_mtx_` before the up-to-5-second
  `wait_for_output_stop` busy-wait, preventing dock UI freezes during slot
  stop (`013-audit-fixes`, S-005).

- `log_replay_saved` snapshots `cfg_.name` under lock, fixing a data race
  with concurrent `update_config` (`013-audit-fixes`, S-006).

- Signal-handler stop callbacks defer `stop()` to the UI thread via
  `obs_queue_task`, avoiding potential deadlock from re-entering the signal
  handler system (`013-audit-fixes`, S-007).

- macOS `available_physical_mb()` now returns available (not total) memory,
  so the replay buffer memory clamp fires correctly under memory pressure
  (`013-audit-fixes`, S-008).

- `start()` snapshots `cfg_` identity fields under `slot_mtx_` before
  calling into SlotManager, eliminating a formal data race
  (`013-audit-fixes`, S-009).

- Recording filenames now embed a 6-hex-char slot ID suffix and sanitize the
  slot name, preventing filename collisions between same-named slots and
  fixing path-unsafe characters (`013-audit-fixes`, C-001 + S-004).

- `slot_from_data` clamps `fps_num` (0–240000) and `fps_den` (0–1001) to
  prevent corrupted save files from hanging the video pipeline
  (`013-audit-fixes`, C-002).

- Slot ID format now uses a separator (`%llx-%x`) to prevent theoretical
  ID collisions (`013-audit-fixes`, C-003).

- Linux `available_physical_mb()` reads `/proc/meminfo` `MemAvailable`
  instead of `sysinfo` `freeram`, preventing overly aggressive replay buffer
  clamping on systems with large page caches (`013-audit-fixes`, C-004).

- Replay preview Config now sets `replay_enabled` from the checkbox state
  (`013-audit-fixes`, C-005).

### Performance

- Dock `refresh()`/`refresh_stats()` use a single `snapshot_slots()` call
  reducing O(N) mutex acquisitions per tick to O(1)
  (`013-audit-fixes`, P-001).

- `stats()` holds `slot_mtx_` only long enough to snapshot pointers, then
  queries OBS output functions without the lock (`013-audit-fixes`, P-002).

- `refresh_stats` caches `s->config()` once per loop iteration
  (`013-audit-fixes`, P-003).

- `SlotEditor` caches `available_physical_mb()` at construction, eliminating
  a per-keystroke syscall (`013-audit-fixes`, P-004).

- `config_by_slot_id` and `slot_name_by_id` use an O(1) hash index instead
  of O(N) linear scans (`013-audit-fixes`, P-005).

- `popcount32` uses hardware intrinsics (`__popcnt`/`__builtin_popcount`)
  (`013-audit-fixes`, P-006).

- `on_encoder_changed` fetches encoder properties once and passes the handle
  to all three consumer functions (`013-audit-fixes`, P-007).

### Changed

- CMakeLists.txt: removed duplicate `find_package`/`target_link_libraries`
  block, consolidated dependencies unconditionally, added `plugin-main.hpp`
  to `target_sources` (`013-audit-fixes`, Q-001 + Q-002).

- Plugin version log now reads from `CMAKE_PROJECT_VERSION` instead of a
  hardcoded `"1.0.0"` (`013-audit-fixes`, Q-003).

- `wait_for_output_stop` magic numbers replaced with named constants
  `kStopTimeoutMs`/`kStopPollMs` (`013-audit-fixes`, Q-004).

- `fmt_bytes_rate` adds a 1-decimal tier for 10–99 kbps, smoothing the
  visual jump at the kbps/Mbps boundary (`013-audit-fixes`, Q-005).

- Static helper functions in slot.cpp wrapped in anonymous namespace for
  style consistency (`013-audit-fixes`, Q-006).

- Stats timer no longer starts unnecessarily when toggling "Show stats" while
  no slot is recording. Previously, toggling the checkbox off then on at idle
  left a 1 Hz timer running until the next slot state change; the timer now
  starts only when at least one slot is actively recording
  (`011-idle-slot-resource-audit`, F-UD1).

- Remove dead `obs_output_set_mixers` calls on encoded outputs (`ffmpeg_muxer`
  and `replay_buffer`) that produced an OBS warning on every slot start.
  Audio track routing is unaffected — it was already fully handled by
  `obs_output_set_audio_encoder` (`009-remove-dead-mixer-call`).

- Replay buffer no longer truncates to inconsistent short durations when
  the rate-control mode is quality-based (CQP / CRF / CQ / ICQ / QVBR /
  Lossless). The buffer's memory ceiling now scales with resolution x fps x
  per-mode bits-per-pixel-per-frame estimate x 2x margin, so configured
  replay duration is honored under typical scene complexity
  (`008-fix-replay-quality-truncation`).

### Added

- Per-slot "Max replay buffer size (MB)" override in the slot editor --
  empty / "Auto" uses the new auto-derived ceiling; a positive value
  bypasses auto-derivation and is used verbatim. Editor shows the currently
  resolved ceiling alongside the override field
  (`008-fix-replay-quality-truncation`).

### Changed

- Replay-save log line now includes observed bitrate alongside the
  auto-derived bitrate assumption. When the buffer's memory ceiling appears
  to have caused early eviction, a hedged warning identifies the achieved
  vs configured duration and names remediation knobs
  (`008-fix-replay-quality-truncation`).

### Performance

- Reuse `QTableWidgetItem` instances during dock refresh to eliminate
  per-tick allocations (F-UD1).
- Snapshot the slot list before iterating in `stop_all` to shorten the time
  `SlotManager::mtx_` is held during shutdown (F-M1).
- Gate the stats refresh timer and reuse dock state buttons to reduce idle
  CPU cost (F1/F2).

### Changed

- Split the slot editor into its own `ui-slot-editor` translation unit and
  hoisted the shared `F-USE1` constants for reuse (refactor; no
  user-visible behaviour change).
- Align each slot's `obs_video_info` to the OBS main `ovi` to prevent
  resolution/framerate drift between slots and the main canvas (D2/D3/D4).
- Replay-save log line wording: the proc-dispatch site now emits
  `replay save requested` (INFO) on dispatch and
  `replay save proc-dispatch FAILED (slot not capturing?)` (WARNING) on
  dispatch failure. A new line, `replay save wrote '<path>'` (INFO), is
  emitted from the OBS replay-buffer `saved` signal callback only after a
  successful on-disk write. A `requested` line with no matching `wrote`
  follow-up is now the explicit failure signal
  (`007-fix-replay-collision`).

### Fixed

- Refresh the dock UI after hotkey-triggered slot start/stop so the
  indicator reflects the actual slot state.
- Remove a redundant `running_.store(true)` in `SceneSlot::start()` that
  could mask a true start failure under contention (F-S1).
- Consumer slots now report the owner's effective rate-control mode and
  value everywhere (editor, slot-start log line, replay-buffer memory-cap
  estimate), with read-only inherited labels in the editor instead of the
  rows being hidden; load-time validation clamps out-of-range values and
  substitutes unknown rate-control modes against the encoder's introspected
  lists, emitting a single warning per affected slot
  (`006-cqp-mismatch`).
- Replay-buffer filename collision across slots sharing an output
  directory: two or more slots writing into the same directory with the
  same container no longer silently lose files when Save Replay fires on
  each within the same wall-clock second. The replay filename now embeds
  the sanitized slot name and a 6-hex-char per-slot identity prefix
  (shape `<name>_<id6>_Replay_<ts>.<ext>`), making cross-slot collision
  impossible by construction and giving the user per-file attribution
  from the filename alone (`007-fix-replay-collision`).

[Unreleased]: https://github.com/Toni19944/obs-multi-scene-record
