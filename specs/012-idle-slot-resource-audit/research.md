# Research: Full Codebase Performance & Stability Audit

**Feature**: 012-idle-slot-resource-audit | **Date**: 2026-05-29

## R1: Audit Methodology for OBS Plugins

**Decision**: Static analysis via exhaustive manual code review, file-by-file, organized by category (Performance, Stability/Safety, Correctness, Code Quality).

**Rationale**: The plugin is ~4,500 LOC across 10 source files — small enough for complete manual review. Static analysis tools (clang-tidy, PVS-Studio) would add toolchain complexity for diminishing returns on a codebase this size. Manual review catches architectural and OBS-API-specific issues that generic linters miss.

**Alternatives considered**:
- clang-tidy + clang-analyzer: Would catch some patterns but miss OBS-API-specific lifecycle issues (source refcounting, output signal handler ordering, view/encoder teardown order).
- Runtime profiling under OBS: Would identify hot-spots but not correctness/stability issues, and requires a full OBS build + test sessions.

## R2: Categorization Framework

**Decision**: Four categories aligned with FR-003:
1. **Performance** — CPU waste, unnecessary allocations, lock contention, redundant work
2. **Stability/Safety** — Thread safety, resource lifecycle, crash resistance, error handling
3. **Correctness** — Behavioral bugs, logic errors, OBS API misuse
4. **Code Quality** — Build configuration, code organization, unnecessary complexity

**Rationale**: Maps directly to the spec's required categories and the constitution's concerns (Thread Safety III, Pipeline Isolation VI, Recording Correctness VII).

**Alternatives considered**:
- OWASP-style security categories: Not applicable — this is a desktop plugin with no network attack surface.
- Per-file organization: Would scatter related findings and make prioritization harder.

## R3: Severity Framework

**Decision**: Four severity levels per FR-006:
- **Critical** — Ship-blocking per constitution (deadlock, data corruption, crash-on-unload)
- **High** — Significant impact on reliability or performance under normal operation
- **Medium** — Measurable improvement, but current behavior is functional
- **Low** — Code quality, maintainability, minor efficiency

**Rationale**: Aligns with the constitution's "NON-NEGOTIABLE" principles (III, VII) for Critical, the "Product Quality Bar" for High, and general best practices for Medium/Low.

## R4: OBS API Lifecycle Best Practices

**Decision**: Key lifecycle patterns to audit against:
- `obs_source_t*`: Must `obs_source_release` every `obs_get_source_by_name` / `obs_source_get_ref`. `inc_showing` must pair with `dec_showing`.
- `obs_output_t*`: Must `obs_output_release` after create. Signal handlers must disconnect before release.
- `obs_encoder_t*`: Must `obs_encoder_release` after create. `get_ref`/`release` must pair.
- `obs_view_t*`: Must remove, set source null, destroy. Order matters.
- `obs_data_t*` / `obs_data_array_t*`: Reference-counted; every `create` / `get_obj` / `get_array` / `array_item` needs a release.
- `obs_properties_t*`: Owned by caller from `obs_get_encoder_properties`; must `obs_properties_destroy`.
- Signal handlers: `signal_handler_disconnect` blocks on in-flight dispatch (synchronization barrier).

**Rationale**: OBS refcounting is the #1 source of plugin memory leaks and crashes. The audit must verify every acquire has a matching release.

## R5: Thread Safety Audit Scope

**Decision**: Verify the declared lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` is honored on every code path. Check:
- Atomic operations use appropriate memory ordering
- No data races on non-atomic members accessed from multiple threads
- Signal handler callbacks (libobs worker threads) don't violate lock order
- Qt cross-thread posting uses `Qt::QueuedConnection` correctly

**Rationale**: Constitution Principle III is NON-NEGOTIABLE. The plugin operates across the OBS main thread, UI thread, hotkey thread, and libobs worker threads.

## R6: CMakeLists.txt Audit Scope

**Decision**: Check for:
- Duplicate find_package / target_link_libraries calls
- Missing or unnecessary dependencies
- CMake version range correctness
- AUTOMOC/AUTOUIC/AUTORCC configuration
- Proper use of OBS plugin template macros

**Rationale**: Build configuration issues cause silent failures on CI and downstream packaging.
