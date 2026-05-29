# Implementation Plan: Apply Codebase Audit Fixes

**Branch**: `013-audit-fixes` | **Date**: 2026-05-29 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/013-audit-fixes/spec.md`

**Audit Report**: [audit-report.md](../012-idle-slot-resource-audit/audit-report.md)

## Summary

Apply all 27 fixes identified in the feature-012 codebase performance & stability audit. Each fix is a discrete, individually confirmable task matching the audit report's labeling scheme (S-xxx, P-xxx, C-xxx, Q-xxx). Fixes address stability/safety (9), performance (7), correctness (5), and code quality (6) issues across the plugin's C++ source and CMake build configuration.

## Technical Context

**Language/Version**: C++17 (MSVC 2022, Clang/AppleClang Xcode 16, GCC Ubuntu 24.04)

**Primary Dependencies**: libobs, obs-frontend-api, Qt6 (Widgets + Core)

**Storage**: N/A (file-based recording managed by OBS output system)

**Testing**: Manual verification — no automated test framework for OBS plugins. Each fix is verified by compilation + targeted manual testing of the affected code path.

**Target Platform**: Windows x64 (Visual Studio 17 2022), macOS (Xcode 16.0), Ubuntu 24.04

**Project Type**: OBS Studio plugin (shared library loaded by OBS at runtime)

**Performance Goals**: Negligible idle overhead; linear per-slot CPU/RAM scaling under recording

**Constraints**: Must compile cleanly on all three CI toolchains. Must not regress any existing functionality.

**Scale/Scope**: 10 source files, 27 discrete fixes. No new features — pure bug-fix and quality pass.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Status | Notes |
|-----------|--------|-------|
| I. Native OBS API Compliance | PASS | All fixes use public libobs/obs-frontend-api interfaces only |
| II. Clear Ownership & Minimal Shared State | PASS | S-002 strengthens ownership with shared_ptr; S-003 uses shared_ptr snapshots |
| III. Thread Safety (NON-NEGOTIABLE) | PASS | S-002, S-003, S-005, S-006, S-007, S-009, P-002 directly improve thread safety |
| IV. UI / Logic Separation | PASS | No UI/logic boundary violations introduced |
| V. Encoder Robustness & Graceful Fallback | PASS | No changes to encoder fallback logic |
| VI. Pipeline Isolation From OBS Main | PASS | No changes to pipeline isolation |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | PASS | C-001 prevents recording overwrites; C-004 fixes memory clamp accuracy |
| VIII. Shared Encoder Literal Semantics | PASS | No changes to shared encoder behavior |
| IX. Configurable Settings Parity | PASS | No settings removed or disabled |

**Gate result**: PASS — no violations. All fixes align with or strengthen constitutional principles.

## Project Structure

### Documentation (this feature)

```text
specs/013-audit-fixes/
├── plan.md              # This file
├── spec.md              # Feature specification
├── research.md          # Phase 0: fix review notes and dependency analysis
├── data-model.md        # Phase 1: type changes introduced by fixes
├── quickstart.md        # Phase 1: build and verification guide
├── checklists/
│   └── requirements.md  # Spec quality checklist
└── tasks.md             # Phase 2 output (created by /speckit-tasks)
```

### Source Code (repository root)

```text
src/
├── plugin-main.hpp      # Clean — no fixes
├── plugin-main.cpp      # S-001, Q-003
├── manager.hpp          # S-002 (type change), P-001 (new method), P-005 (new index)
├── manager.cpp          # S-003, P-001, P-005, C-002
├── slot.hpp             # Clean — no fixes
├── slot.cpp             # S-002, S-004/C-001, S-005, S-006, S-007, S-008, S-009,
│                        #   P-002, P-006, C-003, C-004, Q-004, Q-006
├── ui-dock.hpp          # Clean — no fixes
├── ui-dock.cpp          # P-001 (consumer), P-003, Q-005
├── ui-slot-editor.hpp   # P-004 (cached member), P-007 (signature changes)
└── ui-slot-editor.cpp   # P-004, P-007, C-005
CMakeLists.txt           # Q-001, Q-002, Q-003
```

**Structure Decision**: No new files or directories. All fixes modify existing source files and the build configuration.

## Dependency Graph

Fixes are mostly independent. The following dependencies constrain ordering:

```text
S-002 (shared_ptr migration)
 ├──→ S-003 (shared_ptr snapshot in start_all/stop_all) — uses shared_ptr copies
 └──→ P-001 (snapshot_slots) — snapshot should return shared_ptr after migration

C-001 (recording filename collision fix)
 └──→ S-004 (filename sanitization) — C-001's fix includes sanitization; S-004 validates
```

All other fixes are independent and can be applied in any order.

**Recommended execution order**:
1. S-002 first (enables S-003 and improves P-001)
2. C-001 before S-004 (C-001 subsumes S-004's fix)
3. All others in any order, grouped by file for minimal context-switching

## Implementation Notes

### S-002: Additional requirement not in audit report

The audit report's `shared_from_this()` call on line 101 requires `SceneSlot` to inherit from `std::enable_shared_from_this<SceneSlot>`. This is a standard C++ requirement for `shared_from_this()` and must be added to the class declaration in `slot.hpp`:

```cpp
class SceneSlot : public std::enable_shared_from_this<SceneSlot> {
```

### P-001: Adaptation after S-002

The audit report proposes `SlotSnapshot` with `std::vector<SceneSlot*>` (raw pointers). After S-002's `shared_ptr` migration lands, the snapshot should use `std::vector<std::shared_ptr<SceneSlot>>` for lifetime safety:

```cpp
struct SlotSnapshot {
    std::vector<std::shared_ptr<SceneSlot>> slots;
    size_t generation;
};
```

This is a minor adaptation, not a disagreement with the fix.

### C-001 + S-004: Overlap handling

C-001's proposed fix for `build_output_filename` includes the `sanitize_for_filename` call that S-004 independently recommends. When C-001 is applied, S-004 becomes a verification task confirming that sanitization is present.

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| S-002 shared_ptr migration breaks existing callers | Medium | High | Apply first; compile and test thoroughly before proceeding |
| S-005 teardown restructure introduces new race conditions | Low | High | Careful review of lock acquire/release boundaries; test concurrent start/stop |
| S-007 deferred stop changes signal callback timing | Low | Medium | Verify UI refresh still occurs promptly after external stop |
| Q-001 CMake cleanup breaks build | Low | Medium | Build immediately after applying; verify all three platforms in CI |

## Complexity Tracking

> No constitution violations to justify. All fixes align with constitutional principles.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| (none)    | —          | —                                   |
