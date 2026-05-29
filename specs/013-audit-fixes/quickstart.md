# Quickstart: Apply Codebase Audit Fixes

**Feature**: 013-audit-fixes | **Date**: 2026-05-29

## Prerequisites

- OBS Studio build environment configured per `buildspec.json`
- CMake 3.28–3.30
- One of: MSVC 2022 (Windows), Xcode 16 (macOS), GCC + ninja-build (Ubuntu 24.04)
- Qt6 development libraries (Widgets + Core)

## Branch Setup

```bash
git checkout 013-audit-fixes
```

## Build

```bash
cmake --preset windows-x64  # or macos-universal / ubuntu-x64
cmake --build build --config RelWithDebInfo
```

## Workflow Per Fix

Each of the 27 fixes is a discrete task. The workflow for each:

1. Read the task description (references the audit-report finding ID)
2. Read the corresponding finding in `specs/012-idle-slot-resource-audit/audit-report.md`
3. Review the proposed code change
4. Apply the fix to the source file(s)
5. Build to verify compilation
6. Manually test the affected code path
7. Confirm the fix is correct

## Verification

After all fixes are applied:

1. **Build**: Clean build on all three platforms (or at minimum the primary dev platform)
2. **Smoke test**: Launch OBS with the plugin loaded
3. **Recording test**: Start/stop recording on multiple slots
4. **Replay test**: Start replay buffer, trigger save, verify file output
5. **Hotkey test**: Use hotkeys to start/stop while also using UI controls
6. **Shutdown test**: Close OBS cleanly — verify no crash on unload (S-001)
7. **Concurrent test**: Start multiple slots, stop one while others record (S-002, S-003)

## Key Files Modified

| File | Fix Count |
|------|-----------|
| `src/slot.cpp` | 12 fixes |
| `src/manager.cpp` | 4 fixes |
| `CMakeLists.txt` | 3 fixes |
| `src/ui-slot-editor.cpp` | 3 fixes |
| `src/ui-dock.cpp` | 3 fixes |
| `src/manager.hpp` | 3 fixes |
| `src/slot.hpp` | 2 fixes |
| `src/plugin-main.cpp` | 2 fixes |
| `src/ui-slot-editor.hpp` | 2 fixes |

## Reference

- **Spec**: [spec.md](spec.md)
- **Plan**: [plan.md](plan.md)
- **Audit Report**: [audit-report.md](../012-idle-slot-resource-audit/audit-report.md)
- **Constitution**: `.specify/memory/constitution.md`
