# Data Model: Full Codebase Performance & Stability Audit

**Feature**: 012-idle-slot-resource-audit | **Date**: 2026-05-29

## Entities

### AuditFinding

Represents a single finding in the audit report.

| Field | Type | Description |
|-------|------|-------------|
| id | string | Sequential finding ID (e.g., "F-001") |
| category | enum | Performance, Stability/Safety, Correctness, Code Quality |
| severity | enum | Critical, High, Medium, Low |
| file_path | string | Relative path from repo root (e.g., "src/slot.cpp") |
| line_numbers | string | Line range (e.g., "123-145" or "42") |
| title | string | Short summary of the finding |
| description | string | Detailed explanation of the issue |
| proposed_fix | string | Concrete code-edit steps (before/after or step-by-step) |
| rationale | string | Why the fix is an improvement |

### AuditReport

The top-level document structure.

| Field | Type | Description |
|-------|------|-------------|
| title | string | Report title |
| date | string | Report generation date |
| files_audited | list[string] | All source files examined (confirms full coverage per SC-001) |
| summary | object | Counts by category and severity |
| findings | list[AuditFinding] | Grouped by category, ordered by severity within each group |

## Relationships

- AuditReport 1:N AuditFinding (one report contains all findings)
- AuditFinding references exactly one source file via file_path
- Findings that span multiple files list all affected locations in their description

## Validation Rules

- Every AuditFinding must have all 8 fields populated (FR-002)
- line_numbers must reference actual lines in the current codebase (SC-002)
- files_audited must include all files in `src/` plus `CMakeLists.txt` (SC-001)
- category must be one of the four defined values (FR-003)
- severity must be one of the four defined values (FR-006)

## State Transitions

N/A — the audit report is a static document with no runtime state.
