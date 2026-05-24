# Implementation Plan: Performance parity with OBS native recording

**Branch**: `003-perf-parity-audit` | **Date**: 2026-05-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/003-perf-parity-audit/spec.md`

## Summary

Close every plugin↔OBS-native architectural difference that contributes overhead AND doesn't sacrifice per-slot scene independence (the plugin's identity). The Phase 0 cross-tree comparison (see [research.md](./research.md)) catalogued 11 differences; **D1** (the per-group `obs_view_t + video_t`) is the dominant FPS cost but is **explicitly accepted as irreducible** because it IS the mechanism that gives each slot its own scene-rendering independent of OBS's program. The remaining closeable items are smaller: hardcoded video-info parameters in the per-slot pipeline that diverge from OBS's main config without reason.

**Why D1 is not closed**: the user has made it clear that per-slot scene independence is the plugin's identity. A slot configured to record `Scene_Game` must keep recording `Scene_Game` even if the user switches OBS program to `Scene_BRB` mid-recording. The "use `obs_get_video()`" optimization would tie the slot to whatever the program is currently showing — silently capturing `Scene_BRB` content in `Scene_Game`'s file the moment the user switched scenes. That's a worse outcome than the FPS hit. The per-group view IS the mechanism that delivers independence; closing it deletes the feature.

**Closeable items** (the actual code work for this feature):

- **D2** — `ovi.output_format = VIDEO_FORMAT_NV12;` is hardcoded; should be `main_ovi.output_format`.
- **D3** — `ovi.scale_type = OBS_SCALE_BICUBIC;` is hardcoded; should be `main_ovi.scale_type`.
- **D4** — `ovi.gpu_conversion = true;` is hardcoded; should be `main_ovi.gpu_conversion`.

All three live in `SharedEncoder::build` (`src/slot.cpp:269-282`). Total code change: three one-line edits.

**Memory bounds** (FR-004 — FR-007): the plugin's teardown ordering looks correct on inspection (see research.md's memory section). The 4-hour memory test in [quickstart.md](./quickstart.md) is the live confirmation. No code change planned unless the live test reveals a leak.

**FPS measurement**: the quickstart still includes a single-slot benchmark against OBS native, but the delta is now recorded as institutional memory (a baseline for future regression detection — "did the gap grow?") rather than as a pass/fail bar. Absolute OBS-native parity in single-slot benchmarks would require breaking per-slot independence; the user has chosen not to do that.

## Technical Context

**Language/Version**: C++17.

**Primary Dependencies**: libobs (`obs_view_create`, `obs_view_add2`, `obs_get_video`, `obs_get_video_info`, `obs_get_audio`, `obs_video_encoder_create`, `obs_encoder_set_video`, `obs_output_create`).

**Storage**: N/A — no persisted state changes.

**Testing**: manual benchmark per [quickstart.md](./quickstart.md). A stable, FPS-bound game scenario (locked benchmark, demo, or replay) is required to get reproducible median FPS across runs.

**Target Platform**: Windows x64 (primary; FPS testing is most actionable here), macOS, Ubuntu 24.04.

**Project Type**: Native C++ OBS Studio plugin.

**Performance Goals**: defined by spec — FPS delta vs OBS native within measurement noise (≤1 fps OR ≤1%, whichever is larger), median of ≥5 runs.

**Constraints**:

- **Per-slot scene independence is non-negotiable.** A slot must keep recording its configured scene regardless of OBS program changes. The per-group `obs_view_t + video_t` is what delivers this and is explicitly accepted as irreducible (FR-009 (b), D1 in research.md).
- The plugin supports **shared encoder contexts** (one slot referencing another via `shared_encoder_slot_id`). Multiple sharing slots reuse the SAME `SharedEncoder`. The closeable items here apply once per encoder group, not per consumer slot.
- Constitution Principle V (encoder fallback) is intact — the x264/CBR fallback is unrelated to the video-info parameter alignment.

**Scale/Scope**: three one-line edits to `SharedEncoder::build` (`src/slot.cpp:269-282`). No header changes. No new fields. No new helpers.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | Uses only public libobs API. The change reads `obs_get_video_info` (which the file already calls at line 278 for colorspace/range) and applies the same struct's `output_format / scale_type / gpu_conversion` fields to the per-group ovi. |
| II. Clear Ownership & Minimal Shared State | ✅ | No structural changes. Three lines flip from hardcoded values to copies from `main_ovi`. |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | All affected code (`SharedEncoder::build`) already runs under `shared_mtx_`. No new locks. The `obs_get_video_info` call is already in this function (line 278); we're just consuming three additional fields from its result. |
| IV. UI / Logic Separation | ✅ | No UI changes. |
| V. Encoder Robustness & Graceful Fallback | ✅ | The x264/CBR fallback path is untouched. |

**Result**: PASS, no Complexity Tracking entries.

## Project Structure

### Documentation (this feature)

```text
specs/003-perf-parity-audit/
├── plan.md              # This file
├── spec.md              # Feature spec
├── research.md          # Phase 0: the cross-tree audit document (the US3 deliverable)
├── quickstart.md        # Phase 1: benchmark + leak-test verification procedure
└── checklists/
    └── requirements.md  # Spec-quality checklist (created by /speckit-specify)
```

No `data-model.md` — the change is to lifecycle/wiring logic; no new persisted entities. No `contracts/` — no new public interfaces; the audit document (`research.md`) is itself the institutional-memory artifact for US3.

### Source Code (repository root)

```text
src/
├── slot.cpp            # TOUCHED: SharedEncoder::build aligns three ovi fields to main_ovi
│                       #           (output_format / scale_type / gpu_conversion).
├── slot.hpp            # (unchanged)
├── manager.cpp         # (unchanged)
├── manager.hpp         # (unchanged)
├── ui-dock.cpp         # (unchanged)
├── ui-dock.hpp         # (unchanged)
├── ui-slot-editor.cpp  # (unchanged)
├── ui-slot-editor.hpp  # (unchanged)
├── plugin-main.cpp     # (unchanged)
└── plugin-main.hpp     # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. Three one-line edits in `SharedEncoder::build` at `src/slot.cpp:276-282`. No new translation units, no header changes, no CMake change.

## Phase 0 — Research (the audit)

The Phase 0 deliverable is the **audit document itself** — [research.md](./research.md) — fulfilling User Story 3 (P3) and Functional Requirements FR-008 / FR-009. Every plugin↔OBS-native architectural difference identified in the recording pipeline surface area (FR-008) is catalogued there with disposition.

Summary of dispositions (full details in research.md):

| # | Difference | Disposition | Why |
|---|---|---|---|
| D1 | Plugin creates per-group `obs_view_t + video_t`; OBS native uses `obs_get_video()` | **ACCEPT** (irreducible, FR-009 (b)) | This is the mechanism that delivers per-slot scene independence. Closing it would tie slots to OBS's program and silently capture program content if the user switched scenes mid-recording. Per-slot independence is the plugin's identity. |
| D2 | Plugin hardcodes `VIDEO_FORMAT_NV12`; OBS native uses the user-configured format | **CLOSE** — use `main_ovi.output_format` | Saves a GPU conversion pass when OBS main is configured for a non-NV12 format. No per-slot-independence rationale for the hardcoded value. |
| D3 | Plugin hardcodes `OBS_SCALE_BICUBIC`; OBS native uses configured scale type | **CLOSE** — use `main_ovi.scale_type` | Aligns scale filtering with the user's Settings > Video choice. |
| D4 | Plugin hardcodes `gpu_conversion = true`; OBS native uses configured value | **CLOSE** — use `main_ovi.gpu_conversion` | Aligns with the user's Settings > Advanced choice. |
| D5 | Plugin calls `obs_source_inc_showing` on its scene source per encoder group | **KEEP** | Required to make the slot's scene actually render into the per-group view (the divergent-scene case — which is exactly when we need the view). |
| D6 | Per-slot audio encoders attached to `obs_get_audio()` | **KEEP** | Same pattern OBS native uses. At parity. |
| D7 | `ffmpeg_muxer` + `replay_buffer` output types | **KEEP** | Same types OBS native uses. At parity. |
| D8 | Per-slot output `stop` signal handling | **KEEP** | Same pattern as OBS native. At parity. |
| D9 | Shared encoder context across slots | **KEEP** | Plugin-specific feature, not a divergence from native. |
| D10 | Per-slot encoder fallback (x264/CBR) | **KEEP** | Same logic OBS native uses. Constitution V. |
| D11 | Per-slot scene independence | **ACCEPT** (irreducible, FR-009 (b)) | The plugin's identity. D1 is the implementation; the two are inseparable. |

## Phase 1 — Design & Contracts

### data-model.md

Not generated — the change adds one transient `bool parity_mode_` to `SharedEncoder` (used only for logging / introspection; the core `video_*` handles already encode the operating mode by virtue of being null in parity mode). No new entities, no persisted state.

### contracts/

Not generated — no new public interfaces. The audit document itself ([research.md](./research.md)) is the FR-008 / FR-009 deliverable.

### quickstart.md

Benchmark + memory test procedure for FR-001 through FR-007 and SC-001 through SC-007. See [quickstart.md](./quickstart.md).

### Agent context update

The `<!-- SPECKIT START -->` block in repo-root `CLAUDE.md` is updated to point at this plan file (replaces the pointer to feature 002).

## Re-check Constitution after Phase 1

No new resources, no new threads, no new locks. The `SharedEncoder` lifecycle is unchanged at the manager level; only the internals of `build` and the dtor branch on the parity-mode flag. All five principles remain satisfied. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
