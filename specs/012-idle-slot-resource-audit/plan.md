# Implementation Plan: Full Codebase Performance & Stability Audit

**Branch**: `012-idle-slot-resource-audit` | **Date**: 2026-05-29 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/012-idle-slot-resource-audit/spec.md`

## Summary

Produce a comprehensive audit report (`audit-report.md`) of the entire plugin codebase (`src/` + `CMakeLists.txt`) identifying every actionable performance, stability, correctness, and code-quality improvement. No source code is modified. Each finding includes file, line numbers, issue description, proposed code change, severity, and rationale. The report is the sole deliverable.

## Technical Context

**Language/Version**: C++17 (MSVC 2022 / Clang / GCC)

**Primary Dependencies**: libobs, obs-frontend-api, Qt 6 (Widgets + Core)

**Storage**: N/A (audit produces a markdown file only)

**Testing**: Manual review — each finding is verified against actual source code locations

**Target Platform**: Windows x64, macOS (Xcode 16.0), Ubuntu 24.04

**Project Type**: OBS Studio plugin (shared library / MODULE)

**Performance Goals**: Plugin idle overhead negligible; per-slot CPU/RAM scales linearly (Constitution: Product Quality Bar)

**Constraints**: No source code modifications (FR-004); audit-only deliverable

**Scale/Scope**: 10 source files (~4,500 LOC) + 1 CMakeLists.txt

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native OBS API Compliance | PASS | Audit-only; no API calls introduced |
| II. Clear Ownership & Minimal Shared State | PASS | No state changes; findings may recommend ownership improvements |
| III. Thread Safety (NON-NEGOTIABLE) | PASS | Audit will identify thread-safety issues; no code changes made |
| IV. UI / Logic Separation | PASS | Audit will flag UI/logic coupling; no code changes made |
| V. Encoder Robustness & Graceful Fallback | PASS | Audit may flag fallback-path gaps; no code changes made |
| VI. Pipeline Isolation From OBS Main | PASS | No pipeline changes |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | PASS | Audit may flag correctness issues; no code changes made |
| VIII. Shared Encoder — Literal Semantics | PASS | No encoder changes |
| IX. Configurable Settings Parity | PASS | No settings removed |

All gates pass. This feature produces documentation only — no code changes are made, so no principle can be violated.

## Project Structure

### Documentation (this feature)

```text
specs/012-idle-slot-resource-audit/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── data-model.md        # Phase 1 output
├── quickstart.md        # Phase 1 output
└── audit-report.md      # The deliverable (produced during /speckit-implement)
```

### Source Code (repository root)

```text
src/
├── plugin-main.hpp      # Forward declarations, get_dock()
├── plugin-main.cpp      # Module lifecycle (load/unload), dock creation
├── manager.hpp          # SlotManager + SharedEncoder + EffectiveRC declarations
├── manager.cpp          # SlotManager CRUD, start/stop, persistence, shared encoder registry
├── slot.hpp             # SceneSlot class, Config, Stats, replay/rc utilities
├── slot.cpp             # SceneSlot implementation, encoder build, output setup, hotkeys, stats
├── ui-dock.hpp          # MultiSceneRecordDock declaration
├── ui-dock.cpp          # Dock widget, table, stats polling, state toggle
├── ui-slot-editor.hpp   # SlotEditor dialog declaration
└── ui-slot-editor.cpp   # Slot configuration editor, encoder introspection, combo population

CMakeLists.txt           # Build configuration
```

**Structure Decision**: Single-project plugin. All source lives under `src/`; no tests directory (OBS plugin — tested via manual execution inside OBS). The audit report is a standalone Markdown document in the feature spec directory.

## Complexity Tracking

No constitution violations to justify. This feature is a read-only audit producing a markdown report.
