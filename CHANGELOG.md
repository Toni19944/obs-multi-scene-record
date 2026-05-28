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

### Fixed

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
