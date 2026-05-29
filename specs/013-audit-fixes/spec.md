# Feature Specification: Apply Codebase Audit Fixes

**Feature Branch**: `013-audit-fixes`

**Created**: 2026-05-29

**Status**: Draft

**Input**: User description: "Apply all 27 fixes identified in the feature-012 codebase performance & stability audit (audit-report.md). Each fix is a discrete task matching the audit report's labeling scheme."

**Audit Report Reference**: `specs/012-idle-slot-resource-audit/audit-report.md`

## Clarifications

### Session 2026-05-29

- Q: S-003 fix approach — Option A (assert-only) or Option B (shared_ptr snapshot)? → A: Option B — shared_ptr snapshot. Provides real runtime safety rather than debug-only assertions. S-002 must land first as a prerequisite.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Apply Stability & Safety Fixes (Priority: P1)

A developer applies the 9 stability and safety fixes (S-001 through S-009) identified in the audit report. These address the most dangerous class of issues: use-after-free, dangling pointers, data races, deadlocks, and undefined behaviour. The critical finding (S-001: dangling `g_dock` pointer) and high-severity findings are addressed first.

**Why this priority**: Stability fixes prevent crashes and undefined behaviour during OBS shutdown, slot removal, and concurrent callback dispatch. The critical finding (S-001) can cause a use-after-free crash during normal OBS shutdown. These fixes must land before any other category because they affect fundamental safety.

**Independent Test**: Each fix can be independently verified by code review against the audit-report prescription, compilation, and targeted manual testing of the affected code path (e.g., shutdown sequence for S-001, hotkey-while-removing for S-002).

**Acceptance Scenarios**:

1. **Given** the audit report prescribes a fix for S-001 through S-009, **When** the developer applies each fix, **Then** the fix matches the audit report's proposed code change and the plugin compiles without errors.
2. **Given** a stability fix is applied, **When** the developer exercises the affected code path, **Then** the previously identified unsafe behaviour no longer occurs.

---

### User Story 2 - Apply Performance Fixes (Priority: P2)

A developer applies the 7 performance fixes (P-001 through P-007) identified in the audit report. These address lock contention, redundant syscalls, redundant OBS API calls, and unnecessary computation.

**Why this priority**: Performance fixes reduce CPU overhead and mutex contention in the hot path (1 Hz stats refresh, encoder change UI). They improve responsiveness but do not cause crashes or data loss if deferred.

**Independent Test**: Each fix can be independently verified by code review, compilation, and observing reduced lock contention or eliminated redundant calls in the affected path.

**Acceptance Scenarios**:

1. **Given** the audit report prescribes a fix for P-001 through P-007, **When** the developer applies each fix, **Then** the fix matches the audit report's proposed code change and the plugin compiles without errors.
2. **Given** a performance fix is applied, **When** the developer profiles or observes the affected path, **Then** the previously identified inefficiency is eliminated.

---

### User Story 3 - Apply Correctness Fixes (Priority: P3)

A developer applies the 5 correctness fixes (C-001 through C-005) identified in the audit report. These address filename collisions, invalid data handling, ID ambiguity, and inaccurate platform-specific resource queries.

**Why this priority**: Correctness fixes prevent silent data loss (recording overwrites) and improve defensive validation. They are important but less urgent than stability fixes since the affected conditions require specific triggers (same-name slots, corrupted save files, Linux-specific memory reporting).

**Independent Test**: Each fix can be independently verified by code review, compilation, and testing the specific trigger condition described in the audit report.

**Acceptance Scenarios**:

1. **Given** the audit report prescribes a fix for C-001 through C-005, **When** the developer applies each fix, **Then** the fix matches the audit report's proposed code change and the plugin compiles without errors.
2. **Given** a correctness fix is applied, **When** the developer reproduces the trigger condition, **Then** the previously incorrect behaviour is resolved.

---

### User Story 4 - Apply Code Quality Fixes (Priority: P4)

A developer applies the 6 code quality fixes (Q-001 through Q-006) identified in the audit report. These address build configuration issues, version reporting, magic numbers, formatting, and code style consistency.

**Why this priority**: Code quality fixes improve maintainability, debuggability, and build hygiene. They carry the lowest risk and do not affect runtime behaviour beyond cosmetic improvements (version logging, bitrate display formatting).

**Independent Test**: Each fix can be independently verified by code review, compilation, and inspecting the affected output (log messages, UI formatting, CMake configuration).

**Acceptance Scenarios**:

1. **Given** the audit report prescribes a fix for Q-001 through Q-006, **When** the developer applies each fix, **Then** the fix matches the audit report's proposed code change and the project builds without errors or warnings.

---

### Edge Cases

- What happens when two fixes affect overlapping code regions? Fixes are applied sequentially; later fixes account for earlier changes. Notably, C-001 subsumes S-004 (both affect `build_output_filename` — C-001's fix includes S-004's sanitization as a side effect).
- What happens if a proposed fix introduces a compilation error? The developer reviews and adapts the fix to the current codebase state, preserving the fix's intent while resolving the conflict.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Each of the 27 audit findings (S-001 through S-009, P-001 through P-007, C-001 through C-005, Q-001 through Q-006) MUST be addressed as a discrete, individually confirmable task.
- **FR-002**: Each task MUST preserve the audit-report label (e.g., "S-001", "P-003") so it can be cross-referenced to the corresponding finding in `audit-report.md`.
- **FR-003**: Each fix MUST follow the proposed code change described in the audit report unless the developer identifies a technical issue with the proposal, in which case the deviation MUST be documented.
- **FR-004**: The developer MUST manually review and confirm each fix before it is applied to the codebase.
- **FR-005**: The plugin MUST compile successfully after each individual fix is applied.
- **FR-006**: Fixes that affect overlapping code regions MUST be applied in an order that accounts for their interdependencies (e.g., C-001 before S-004 since C-001 subsumes S-004).
- **FR-007**: The shared_ptr migration (S-002) MUST be applied before S-003, since S-003's shared_ptr snapshot fix depends on the S-002 type change.

### Key Entities

- **Finding**: An audit-report entry identified by category letter + number (e.g., S-001). Attributes: category, severity, file(s), line(s), issue description, proposed fix, rationale.
- **Task**: A work unit corresponding to exactly one finding. Attributes: finding ID, status (pending/confirmed/applied/verified), any deviation notes.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: All 27 findings from the audit report are addressed — each has a corresponding completed task.
- **SC-002**: The plugin compiles without errors after the full set of fixes is applied.
- **SC-003**: Each fix is individually reviewed and confirmed by the developer before application.
- **SC-004**: Zero regressions in existing plugin functionality (recording, replay buffer, hotkeys, dock UI, slot management) after all fixes are applied.
- **SC-005**: Any deviations from the audit report's proposed fixes are documented with rationale.

## Assumptions

- The audit report (`specs/012-idle-slot-resource-audit/audit-report.md`) is the authoritative and complete reference for all 27 fixes.
- The codebase has not changed since the audit was performed (the audit was conducted on the current branch state).
- The developer has access to a build environment capable of compiling the OBS plugin.
- Fixes are applied on branch `013-audit-fixes`, which was created from the same codebase state the audit examined.
- The S-002 shared_ptr migration is a confirmed prerequisite for S-003 (Option B — shared_ptr snapshot was chosen over Option A assert-only).
