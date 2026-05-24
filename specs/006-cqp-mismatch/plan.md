# Implementation Plan: CQP value coherence across editor, log, and shared-encoder consumer

**Branch**: `006-cqp-mismatch` | **Date**: 2026-05-24 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/006-cqp-mismatch/spec.md`

## Summary

The bug ([spec.md](./spec.md) US1) is that a consumer slot — one whose video encoder is "Use slot X's encoder" — still carries stale standalone `rate_control` / `rc_value` fields from before the user switched it to consumer mode, and those stale fields surface in (a) the slot start log line, (b) the editor's hidden-but-saved cfg, and (c) the replay-buffer memory-cap estimate (`src/slot.cpp:748`). The spec also identifies two adjacent silent-clobber paths on the owner side: a persisted value outside the encoder's range (FR-013) and a persisted mode string the encoder doesn't list (FR-015).

The fix is built around **one resolution helper** on `SlotManager` — `effective_rate_control(const Config&) -> EffectiveRC` — and **three load-time normalization steps** in `slot_from_data`:

- **Helper** (`manager.hpp` / `manager.cpp`): one function that returns the effective mode / value for any slot. Owners pass through their own Config; consumers resolve to the owner's Config (via the existing `config_by_slot_id` path under `mtx_`) and overlay the `SharedEncoder::encoder_fallback_` flag (briefly under leaf `shared_mtx_`) when the owner's encoder built under fallback. Every consumer-side read of rate control routes through this one function.

- **Load-time normalization** (`manager.cpp` `slot_from_data`): three idempotent steps after existing back-compat defaults:
  - **Decision 2 (FR-006 / FR-012)**: if `shared_encoder_slot_id` is non-empty, clear `rate_control` to the sentinel `"<inherited>"` and `rc_value` to `0`. This is the on-disk migration; no user action is required.
  - **Decision 4 (FR-015)**: if `rate_control` is not in the encoder's reported mode list, substitute the first listed mode and emit one warning.
  - **Decision 3 (FR-013)**: if `rc_value` is outside the encoder's range for the (possibly substituted) mode, clamp and emit one warning.

- **Editor inherited rows** (`ui-slot-editor.cpp` `update_shared_encoder_visibility`): stop hiding the rate-control rows for consumers; show them disabled and labeled `"(inherited from <owner>)"` (with `" [CBR fallback]"` suffix when applicable). Implements FR-005.

- **Slot start log reformat** (`src/slot.cpp:562-566`): route through the helper; print `Lossless` for lossless modes; prepend `[CBR fallback]` when applicable; append `inherited from '<owner>'` for consumers. Implements FR-001 / FR-002 / FR-003 / FR-008.

- **Replay-buffer memory estimate redirect** (`src/slot.cpp:748`): route through the helper. Implements FR-014 for the second known consumer-side read site.

- **Editor `on_accept` sentinel write** (`src/ui-slot-editor.cpp:691-758`): when the user saves a slot whose video encoder is `shared:`, write the sentinel + 0 to the consumer's `rate_control` / `rc_value`. Closes the save-side hole symmetric to load-side normalization.

- **Owner-side range/key alignment** (`src/slot.cpp` `set_quality_value` + `src/ui-slot-editor.cpp` `introspect_quality_range`): factor the shared quality-key list into a single named array (in `slot.hpp` under `rc_util`) so the editor's range source and the encoder-build's write target are derived from the same list by construction (Decision 7). Implements FR-007.

The fix is **non-contract-affecting** with respect to the on-disk save format in shape: the `rate_control` (string) and `rc_value` (int) keys still round-trip; only their semantic for consumers changes (sentinel + 0). Pre-006 saves with stale consumer fields load and are normalized silently at load (Test 5 in [quickstart.md](./quickstart.md)). Post-006 saves opened by a pre-006 build see the sentinel as an unknown mode — the same silent-clobber path FR-015 fixes; documented as a one-way upgrade gate.

Net code change (estimated): ~180 LOC added across 5 source files (`manager.hpp`, `manager.cpp`, `slot.hpp`, `slot.cpp`, `ui-slot-editor.cpp`); ~10 LOC removed; one new `struct EffectiveRC` and one new member function on `SlotManager`. The estimate includes the editor's encoder-combo change handler (T009b in [tasks.md](./tasks.md)) that seeds standalone defaults on the consumer → standalone return trip per FR-016 (~30 LOC). No new translation units, no CMake changes, no save-format additions.

The full design and call-site audit is in [research.md](./research.md); the data-model changes are documented in [data-model.md](./data-model.md); the four-way coherence contract (FR-011) is in [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md); the manual verification procedure is in [quickstart.md](./quickstart.md).

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: libobs (`obs_get_encoder_properties`, `obs_properties_get`, `obs_property_list_*`, `obs_property_int_*`, `obs_data_*`, `obs_encoder_*`, `obs_source_*`, `obs_view_*`), `obs-frontend-api` (save callback only), Qt 6 (`QFormLayout`, `QComboBox`, `QSpinBox`, `QLabel` — editor display only).

**Storage**: scene-collection JSON via libobs `obs_data_t`. The `multi_scene_record.slots[]` array and its per-slot `rate_control` (string) / `rc_value` (int) keys are unchanged in shape; their semantics for consumer slots change to "sentinel + 0" (in-memory normalization at load and on accept).

**Testing**: manual verification per [quickstart.md](./quickstart.md). 15 tests (numbered 1, 2, 3, 3b, 4, 5, 6, 6b, 7, 8, 9, 10, 11, 12, 13) covering: US1 headline bug (T1), US1 fallback (T2), US2 sentinel write (T3), FR-016 consumer→standalone seed (T3b), US2 read-only editor rows (T4), US2 load-time migration (T5), FR-013 quality-mode clamp (T6), FR-013 bitrate-mode clamp (T6b), FR-015 substitute (T7), FR-009 propagation (T8), FR-008 lossless (T9), FR-007 range/key per encoder (T10), standalone regression (T11), shared-encoder runtime semantics regression (T12), FR-010 consumer-starts-while-owner-stopped (T13). Manual coverage is **Windows-only** (the maintainer's test environment); cross-platform encoders (macOS VideoToolbox, Ubuntu `obs_x264 + CRF`) rely on the structural guarantee from the shared `rc_util::quality_keys()` / `quality_split_keys()` arrays (Decision 7 / tasks T013-T014) and on future community reports. No automated test harness on this project.

**Target Platform**: Windows x64 (primary and the **only manually-verified** target — CQP-bug is reproducible on NVENC / AMF / QSV which are most common on Windows, and the maintainer's test environment is Windows-only). macOS VideoToolbox and Ubuntu 24.04 `obs_x264 + CRF` are supported but NOT manually verified for this feature; FR-007 / SC-004 coverage on those platforms rests on the structural guarantee from tasks T013 / T014 (the editor's range introspection and the encoder-build write path walk the same shared `rc_util::quality_keys()` / `quality_split_keys()` arrays by construction) and on future community reports.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: the helper adds two short, bounded locked sections (`mtx_` briefly for owner Config lookup; `shared_mtx_` briefly for the fallback flag). The slot start path takes the helper once per start, BEFORE `setup_outputs` is called, and passes the resolved effective values into `setup_outputs` via the existing slot-local state or an added parameter — this avoids inverting the global lock order at the replay-buffer estimate site, where `setup_outputs` already holds `slot_mtx_` and the helper's internal `mtx_` acquisition would otherwise violate `mtx_ → slot_mtx_`. The editor takes the helper once per `update_shared_encoder_visibility` (which fires on encoder-combo change — UI thread, low frequency). None of these are hot paths.

**Constraints**:

- Constitution Principle III (lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf) — preserved. The helper takes `mtx_` and `shared_mtx_` **independently** (never nested). It is called only from contexts that DO NOT already hold `slot_mtx_`: from `SceneSlot::start()` BEFORE `setup_outputs` (the resolved values are passed into `setup_outputs`, not re-computed there), from the slot start log site, and from the editor's UI thread. All existing locking sites are unchanged.
- Constitution Principle IV (UI / Logic Separation) — preserved. The editor calls the helper on `SlotManager`; it does not call libobs to read encoder state at runtime (the existing `obs_get_encoder_properties` introspection in the editor for the standalone-owner combo population is unchanged — that's editor-internal introspection, not "reading state from libobs about another slot's encoder").
- Constitution Principle II (Clear Ownership) — preserved. The helper reads owner Config via the existing `config_by_slot_id` path; no slot reaches into another slot's pipeline.
- Constitution Principle V (Encoder Robustness & Graceful Fallback) — preserved and **enhanced**: the fallback flag (`SharedEncoder::encoder_fallback_`) is now also surfaced in the editor's inherited row label (`[CBR fallback]` suffix), in addition to the existing dock indicator and the new log line. The fallback path in `SharedEncoder::build` itself is unchanged.
- Constitution Principle VIII (Shared Encoder — Literal Semantics) — preserved and **strengthened**: consumer slots no longer carry contradictory stored values that could leak into any user-visible surface. The shared encoder is what the owner configured; the consumer reports exactly that.
- Constitution Principle IX (Configurable Settings Parity) — preserved. No setting is removed. The previously-hidden rate-control rows on consumer slots become read-only inherited rows (information is gained, not lost).
- No new fields on `SceneSlot::Config`. The sentinel value uses the existing `rate_control` field.
- No new persisted file-format keys. The existing `rate_control` (string) and `rc_value` (int) keys carry the sentinel.
- Patch notes: CHANGELOG.md will gain one entry under a new version section (per constitution Development Workflow); user-visible change is "consumer slots now report the owner's rate control everywhere, plus load-time clamp / substitute warnings for misconfigured saved values."

**Scale/Scope**: 5 source files modified — `manager.hpp` (add `EffectiveRC` struct + `effective_rate_control` declaration), `manager.cpp` (helper impl + `slot_from_data` normalization steps), `slot.cpp` (start-log reformat + replay-buffer estimate redirect + factor `quality_keys()` out of `set_quality_value`), `slot.hpp` (declare `rc_util::quality_keys` / `quality_split_keys`), `ui-slot-editor.cpp` (inherited rows in `update_shared_encoder_visibility` + sentinel write in `on_accept` + consumer → standalone seed handler for FR-016 + use shared `quality_keys` list in `introspect_quality_range`). Estimated ~180 LOC added, ~10 LOC removed.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | PASS | Only existing libobs / obs-frontend APIs are used (`obs_get_encoder_properties` etc.). No new headers, no internal symbols. |
| II. Clear Ownership & Minimal Shared State | PASS | The helper reads owner Config via the existing `config_by_slot_id` path; no slot reaches into another's pipeline. `SharedEncoder` ownership in `SlotManager` is unchanged. |
| III. Thread Safety (NON-NEGOTIABLE) | PASS | Helper takes `mtx_` and `shared_mtx_` independently and briefly. No new locks. No new threads. Global lock order preserved by construction: helper is never called while `slot_mtx_` is held — the replay-buffer estimate site (`setup_outputs`) receives pre-resolved values from `start()` rather than re-invoking the helper under `slot_mtx_`. See [contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md) § Threading.
| IV. UI / Logic Separation | PASS | Editor reads from `SlotManager::effective_rate_control`; it does not call libobs at runtime to read state about other slots. The editor's existing introspection of the **selected own** encoder's properties (combo population, range introspection) is unchanged — that is editor-local introspection. |
| V. Encoder Robustness & Graceful Fallback | PASS | The `obs_x264 / CBR` fallback path in `SharedEncoder::build` is untouched. The `encoder_fallback_` flag is **additionally** surfaced in the editor's inherited row label and in the slot start log, complementing the existing dock indicator. |
| VI. Pipeline Isolation From OBS Main | PASS | No interaction with OBS main outputs. No new `obs_output_t` calls. |
| VII. Recording & Replay Buffer Correctness (NON-NEGOTIABLE) | PASS | No change to replay-buffer length, mode, or save semantics. The only replay-related change is the memory-cap **estimate** (a safety-net upper bound), which is redirected to use the owner's effective rate-control value — strictly improving the estimate when a consumer's stale fields previously over- or under-sized the cap. Per-slot independence preserved. |
| VIII. Shared Encoder — Literal Semantics | PASS, strengthened | Consumer slots can no longer carry contradictory stored values that surface as if in effect. The shared encoder is what the owner configured; the consumer reports exactly that everywhere. |
| IX. Configurable Settings Parity | PASS | No user-configurable setting removed. Consumer-slot rate-control rows that were previously hidden become read-only inherited rows — the user gains information (which owner, what value, whether in fallback), losing no control. |

**Result**: PASS. No Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/006-cqp-mismatch/
├── plan.md              # This file (/speckit-plan output)
├── spec.md              # Feature spec (/speckit-specify + /speckit-clarify output)
├── research.md          # Phase 0: design decisions + call-site audit
├── data-model.md        # Phase 1: SceneSlot::Config semantics + EffectiveRC + state transitions
├── contracts/
│   └── rate-control-coherence.md  # Phase 1: four-way coherence contract + libobs API surface
├── quickstart.md        # Phase 1: 12-test manual verification procedure
├── tasks.md             # Phase 2 output (/speckit-tasks command - NOT created by /speckit-plan)
└── checklists/
    └── requirements.md  # Spec-quality checklist (from /speckit-specify)
```

### Source Code (repository root)

```text
src/
├── manager.hpp          # TOUCHED: add struct EffectiveRC; declare SlotManager::effective_rate_control.
├── manager.cpp          # TOUCHED: implement effective_rate_control; augment slot_from_data with three normalization steps (Decisions 2/3/4).
├── plugin-main.hpp      # (unchanged)
├── plugin-main.cpp      # (unchanged)
├── slot.hpp             # TOUCHED: rc_util gains quality_keys()/quality_split_keys() declarations (used by both slot.cpp set_quality_value and ui-slot-editor.cpp introspect_quality_range).
├── slot.cpp             # TOUCHED: factor quality_keys list; route start-log line and replay-buffer estimate through SlotManager::effective_rate_control.
├── ui-dock.hpp          # (unchanged)
├── ui-dock.cpp          # (unchanged — dock already reads encoder_fallback via Stats; no rate-control fields touched)
├── ui-slot-editor.hpp   # (unchanged — no signature changes; introspect_quality_range stays in the anonymous namespace, but its key list reads from rc_util now)
└── ui-slot-editor.cpp   # TOUCHED: update_shared_encoder_visibility shows read-only inherited rows; on_accept writes the sentinel for shared selection; introspect_quality_range uses the shared quality_keys list.
```

**Structure Decision**: Single-project OBS plugin. Five `.cpp`/`.hpp` files modified across four logical changes (resolution helper, load-time normalization, editor inherited rows, log/replay-buffer redirect, range/key alignment). No new translation units, no CMake changes, no save-format additions.

## Phase 0 — Research

The Phase 0 deliverable is [research.md](./research.md). All nine design decisions are resolved there with rationale + alternatives considered. No NEEDS CLARIFICATION items remain.

## Phase 1 — Design & Contracts

### data-model.md

[data-model.md](./data-model.md) documents:

- `SceneSlot::Config::rate_control` / `rc_value` semantics for owner vs. consumer slots (post-006: consumer carries the `"<inherited>"` sentinel and `0`).
- The new `SlotManager::EffectiveRC` return struct and `effective_rate_control` member.
- The three load-time normalization steps in `slot_from_data` (consumer-clear, mode-substitute-and-warn, value-clamp-and-warn) with idempotency rules.
- Editor accept / display state transitions.
- Slot start log line format.
- Backward / forward compatibility rules for older saves and post-006 saves opened by older builds.

### contracts/

[contracts/rate-control-coherence.md](./contracts/rate-control-coherence.md) documents:

- The four surfaces (editor / persisted / encoder / log) and the rule that all four MUST report the same single value (FR-011).
- The libobs API surface consumed (all calls already exist in this codebase — no new APIs).
- The internal contract for `SlotManager::effective_rate_control`: inputs, outputs, resolution rule, threading discipline, forbidden contexts.
- The internal contract for the three load-time normalization steps in `slot_from_data` (idempotency, warning format, ordering).
- The internal contract for editor inherited rows (FR-005).
- The internal contract for the slot start log format (FR-001 / FR-008).
- The constitution-principle mapping (every principle accounted for).

### quickstart.md

[quickstart.md](./quickstart.md) covers 15 tests:

1. US1 headline bug (consumer reports owner's encoder).
2. US1 Acceptance #3 (fallback values reported).
3. US2 Acceptance #1 (sentinel write on switch-to-consumer).
3b. FR-016 (consumer → standalone return-trip seeds valid defaults).
4. US2 Acceptance #2 (editor read-only inherited rows).
5. US2 Acceptance #3 (load-time migration of pre-006 stale saves).
6. FR-013 (quality-mode value clamp + warning).
6b. FR-013 (bitrate-mode value clamp + warning).
7. FR-015 (mode substitute + warning).
8. FR-009 (owner reconfigure propagates).
9. FR-008 (Lossless rendering).
10. FR-007 (range / key alignment per encoder).
11. Standalone-slot regression.
12. Shared-encoder runtime semantics regression (constitution principle II preserved).
13. FR-010 (consumer starts while owner stopped, no prior `SharedEncoder` row).

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point to this plan, replacing the pointer to feature 005's plan.

## Re-check Constitution after Phase 1

No new threads. No new locks. No new persisted state. No new UI dependencies. No new translation units. The save-format shape is unchanged (only consumer-side semantics on existing keys change). All nine principles remain satisfied — including the two NON-NEGOTIABLE ones (III Thread Safety, VII Recording & Replay Buffer Correctness). No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
