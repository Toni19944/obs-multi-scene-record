# Research: Apply Codebase Audit Fixes

**Feature**: 013-audit-fixes | **Date**: 2026-05-29

## Research Context

This feature applies 27 pre-analyzed fixes from `specs/012-idle-slot-resource-audit/audit-report.md`. The audit report itself serves as the primary research artifact — each finding includes file locations, issue analysis, proposed code changes, and rationale. This research.md documents supplementary analysis: fix correctness review, dependency identification, and implementation adaptations.

## Fix Correctness Review

All 27 proposed fixes were reviewed for technical soundness. **All are correct.** Two require minor adaptation:

### Decision: S-002 requires `enable_shared_from_this`

- **Decision**: Add `std::enable_shared_from_this<SceneSlot>` base class to `SceneSlot`
- **Rationale**: The audit report's `shared_from_this()` call is undefined behavior without this base class. Standard C++ requirement.
- **Alternatives considered**: None — this is the only way to use `shared_from_this()`.

### Decision: P-001 snapshot type adapts to S-002

- **Decision**: Use `std::vector<std::shared_ptr<SceneSlot>>` in `SlotSnapshot` instead of raw pointers
- **Rationale**: After S-002 migrates `slots_` to `shared_ptr`, the snapshot should copy `shared_ptr`s for lifetime safety consistency. Raw pointers would still be safe (dock refresh is UI-thread-only and short-lived) but `shared_ptr` copies are zero-cost when already shared.
- **Alternatives considered**: Raw pointer snapshot (functionally safe but inconsistent with S-002's safety model).

### Decision: S-003 uses Option B (shared_ptr snapshot)

- **Decision**: Apply Option B (shared_ptr snapshot) for `start_all`/`stop_all`, not Option A (assert-only)
- **Rationale**: User confirmed during clarification. Option B provides runtime safety in release builds; Option A only fires in debug builds.
- **Alternatives considered**: Option A (Q_ASSERT only) — rejected because it does not prevent the bug in production.

## Dependency Analysis

### Hard dependencies (ordering required)

| Upstream | Downstream | Reason |
|----------|-----------|--------|
| S-002 | S-003 | S-003 Option B copies `shared_ptr`s from `slots_`, which requires S-002's type migration |
| S-002 | P-001 | P-001's `SlotSnapshot` should use `shared_ptr` after S-002 lands |
| C-001 | S-004 | C-001's `build_output_filename` rewrite includes sanitization; S-004 becomes a validation check |

### Soft dependencies (recommended but not required)

| Upstream | Downstream | Reason |
|----------|-----------|--------|
| S-005 | P-002 | S-005 restructures teardown lock scope; P-002 reduces stats lock scope. Both touch `slot_mtx_` patterns. Applying S-005 first avoids merge conflicts. |
| Q-001 | Q-002, Q-003 | Q-001 removes duplicate CMake blocks. Q-002 and Q-003 modify `CMakeLists.txt`. Applying Q-001 first avoids editing lines that will be deleted. |

### File-level grouping (minimize context-switching)

| File | Fixes |
|------|-------|
| `CMakeLists.txt` | Q-001, Q-002, Q-003 |
| `src/plugin-main.cpp` | S-001, Q-003 (compile def is in CMake) |
| `src/slot.hpp` | S-002 (base class), Q-006 |
| `src/slot.cpp` | S-002, S-004/C-001, S-005, S-006, S-007, S-008, S-009, P-002, P-006, C-003, C-004, Q-004 |
| `src/manager.hpp` | S-002 (type change), P-001 (new method), P-005 (new index) |
| `src/manager.cpp` | S-003, P-001, P-005, C-002 |
| `src/ui-dock.cpp` | P-001 (consumer), P-003, Q-005 |
| `src/ui-slot-editor.hpp` | P-004 (new member), P-007 (signature changes) |
| `src/ui-slot-editor.cpp` | P-004, P-007, C-005 |

## Audit Report Discrepancy (informational)

The audit report's summary table has a minor severity-distribution discrepancy versus individual finding headings:

| Category | Summary claims | Headings show |
|----------|---------------|---------------|
| Stability/Safety | 1C/3H/4M/1L | 1C/1H/5M/2L |
| Correctness | 0C/1H/1M/3L | 0C/1H/0M/4L |
| Performance | 0C/1H/3M/3L | 0C/1H/3M/3L (matches) |
| Code Quality | 0C/0H/2M/4L | 0C/0H/2M/4L (matches) |

Per-category totals (9/7/5/6) and grand total (27) are correct in both. The individual finding headings are the source of truth for severity labeling in tasks.
