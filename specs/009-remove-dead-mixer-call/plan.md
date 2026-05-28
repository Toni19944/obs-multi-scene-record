# Implementation Plan: Remove Dead obs_output_set_mixers Calls

**Branch**: `009-remove-dead-mixer-call` | **Date**: 2026-05-28 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/009-remove-dead-mixer-call/spec.md`

## Summary

Remove two dead calls to `obs_output_set_mixers()` in `src/slot.cpp` that produce OBS runtime warnings on every slot start. Both calls target encoded outputs (`ffmpeg_muxer` and `replay_buffer`) where the OBS API rejects the call with a warning and performs no state change. Audio track routing is already fully handled by the preceding `obs_output_set_audio_encoder()` loop. The removal eliminates log noise and has zero behavior change.

## Technical Context

**Language/Version**: C++17

**Primary Dependencies**: libobs, obs-frontend-api, Qt 6 (Widgets + Core)

**Storage**: N/A (file output is managed by OBS output subsystem)

**Testing**: Manual runtime verification — start slots with various track configurations, confirm no warning in log and audio tracks are correct in output files

**Target Platform**: Windows x64 (MSVC 2022), macOS (Xcode 16), Ubuntu 24.04

**Project Type**: OBS Studio plugin (shared library / MODULE)

**Performance Goals**: N/A — dead-code removal only

**Constraints**: Change must be confined to `src/slot.cpp`. No header, build-system, or other source file changes.

**Scale/Scope**: 2-line deletion in a single file

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native OBS API Compliance | **PASS** | Removing an invalid API call improves compliance — `obs_output_set_mixers` is documented for raw outputs only |
| II. Clear Ownership & Minimal Shared State | **PASS** | No ownership model change |
| III. Thread Safety (NON-NEGOTIABLE) | **PASS** | No locking or concurrency change — deletion of a no-op call |
| IV. UI / Logic Separation | **PASS** | No UI change |
| V. Encoder Robustness & Graceful Fallback | **PASS** | Encoder routing unchanged |
| VI. Pipeline Isolation From OBS Main | **PASS** | No interaction with main pipeline |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | **PASS** | Audio routing is handled by `obs_output_set_audio_encoder` loop; the removed call had no effect (OBS discards it) |
| VIII. Shared Encoder — Literal Semantics | **PASS** | Video encoder sharing unchanged |
| IX. Configurable Settings Parity | **PASS** | No setting removed or altered |
| Product Quality Bar | **PASS** | Eliminates log spam at default verbosity (log spam is a defect per constitution) |
| Patch Notes | **REQUIRED** | CHANGELOG.md entry required before merge |

**Gate result: PASS** — no violations, no complexity tracking needed.

## Project Structure

### Documentation (this feature)

```text
specs/009-remove-dead-mixer-call/
├── plan.md              # This file
├── research.md          # Phase 0 output
├── spec.md              # Feature specification
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (created by /speckit-tasks)
```

### Source Code (repository root)

```text
src/
├── slot.cpp             # ONLY file modified (lines 983, 1072)
├── slot.hpp
├── manager.cpp
├── manager.hpp
├── plugin-main.cpp
├── plugin-main.hpp
├── ui-dock.cpp
├── ui-dock.hpp
├── ui-slot-editor.cpp
└── ui-slot-editor.hpp
```

**Structure Decision**: Single-project OBS plugin. Change is confined to `src/slot.cpp` — the `setup_outputs()` method within `SceneSlot`.
