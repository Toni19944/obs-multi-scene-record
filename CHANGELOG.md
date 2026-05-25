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

[Unreleased]: https://github.com/Toni19944/obs-multi-scene-record
