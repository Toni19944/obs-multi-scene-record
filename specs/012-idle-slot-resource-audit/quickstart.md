# Quickstart: Full Codebase Performance & Stability Audit

**Feature**: 012-idle-slot-resource-audit | **Date**: 2026-05-29

## What This Feature Does

Produces a comprehensive audit report (`audit-report.md`) covering every source file in the plugin codebase. Each finding details a specific performance, stability, correctness, or code-quality improvement with exact file locations, proposed code changes, and reasoning.

## How to Use the Audit Report

1. Open `specs/012-idle-slot-resource-audit/audit-report.md`
2. Review the summary section for counts by category and severity
3. Start with **Critical** findings — these are ship-blocking per the constitution
4. Work through **High** → **Medium** → **Low** in subsequent feature branches
5. Each finding includes step-by-step edit instructions that can be applied directly

## Files Involved

- **Output**: `specs/012-idle-slot-resource-audit/audit-report.md` (the deliverable)
- **Scope**: All files under `src/` and `CMakeLists.txt` at repo root

## Constraints

- No source code is modified during this feature (FR-004)
- Every finding references real, verifiable code locations (SC-002)
- The report is self-contained Markdown — no external tooling required (SC-004)
