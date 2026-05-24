# Implementation Plan: Fix Hotkey Registration

**Branch**: `001-fix-hotkey-registration` | **Date**: 2026-05-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/001-fix-hotkey-registration/spec.md`

## Summary

Replace the current "private scene source" hotkey-grouping workaround with `obs_hotkey_register_output(...)` against a per-slot, long-lived, inert sentinel `obs_output_t*`. Each slot owns one sentinel output created at slot construction (named `Multi-Scene Record: <slot name>`), against which both its hotkeys are registered. The sentinel is never started — it exists solely so OBS Settings > Hotkeys groups the slot's two hotkeys under that label. Slot rename triggers a destroy+recreate of the sentinel (and the registered hotkeys against it), reusing the existing `capture_hotkey_bindings()` cycle in `SceneSlot::update_config()` to preserve user bindings.

This change is confined to two files (`src/slot.cpp`, `src/manager.cpp`) and their headers. The shared-encoder pipeline, the per-slot recording/replay outputs (`rec_out_`, `replay_out_`), the save/load format for hotkey bindings (`hk_record`, `hk_save_replay`), and the frontend-event-driven registration timing are all unchanged.

## Technical Context

**Language/Version**: C++17 (constitution: C++17 minimum).

**Primary Dependencies**: `libobs` (OBS Studio 31.1.1+), `obs-frontend-api`, Qt 6 Widgets + Core (UI dock — not touched by this change).

**Storage**: scene-collection-scoped `obs_data_t` save blob registered via `obs_frontend_add_save_callback`. Per-slot hotkey bindings live in the `hk_record` and `hk_save_replay` keys inside each slot's `obs_data_t` (format unchanged).

**Testing**: manual verification via OBS GUI (Settings > Hotkeys, dock interaction). The plugin has no automated test harness; verification follows the quickstart procedure.

**Target Platform**: Windows x64 (MSVC 17 2022), macOS (Xcode 16), Ubuntu 24.04 (gcc). The hotkey API path is identical across all three.

**Project Type**: Native C++ OBS Studio plugin (single shared library).

**Performance Goals**: N/A — hotkey registration is a one-shot per-slot setup; dispatch is libobs's responsibility.

**Constraints**: No `obs_output_set_name` exists in libobs (verified in `obs.h:1914`), so slot rename requires destroy+recreate of the sentinel output. The output's `OBS_HOTKEY_REGISTERER_OUTPUT`-grouped hotkeys are torn down with it; rename must therefore route through the existing capture→unregister→re-register cycle in `SceneSlot::update_config()`.

**Scale/Scope**: typical 1–10 slots per user; each slot adds one inert `obs_output_t*` plus two `obs_hotkey_id`. Negligible memory and zero runtime cost when no key is pressed.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-checked after Phase 1 design.*

| Principle | Status | Notes |
|---|---|---|
| I. Native OBS API Compliance | ✅ | Uses only public `obs_hotkey_register_output`, `obs_output_create`, `obs_output_release`, `obs_hotkey_unregister`, `obs_hotkey_save`, `obs_hotkey_load` — all exported in `libobs/obs.h` and `libobs/obs-hotkey.h`. Removes the existing reliance on `obs_source_create_private("scene", …)` which the spec identifies as a fragile workaround. |
| II. Clear Ownership & Minimal Shared State | ✅ | Each slot owns exactly one new resource (its sentinel `obs_output_t*`). No cross-slot references; no new shared state on `SlotManager`. Removes shared state (`hotkey_group_source_` on `SlotManager`). |
| III. Thread Safety (NON-NEGOTIABLE) | ✅ | Sentinel-output lifecycle runs on the main thread (slot construction, `register_hotkeys` from `OBS_FRONTEND_EVENT_FINISHED_LOADING`, `~SceneSlot` from `SlotManager::shutdown` / `remove_slot`). The hotkey callback (`on_record_hotkey`, `on_save_hotkey`) thread-safety contract is unchanged — both already enter through `start()` / `stop()` / `save_replay()` which take `slot_mtx_`. No new lock-order interactions. The global lock order `mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` is unaffected. |
| IV. UI / Logic Separation | ✅ | No UI changes. All Qt code paths untouched. |
| V. Encoder Robustness & Graceful Fallback | ✅ | Unrelated to encoder selection. The sentinel output is never started, never has an encoder bound; no fallback path needed. A defensive fallback to `obs_hotkey_register_frontend` is added for the case where `obs_output_create` of the sentinel returns null (parallels the existing fallback the current code has against a null group source). |

**Result**: PASS, no Complexity Tracking entries required.

## Project Structure

### Documentation (this feature)

```text
specs/001-fix-hotkey-registration/
├── plan.md              # This file
├── spec.md              # Feature spec
├── research.md          # Phase 0: API trade-off research, decisions
├── data-model.md        # Phase 1: lifecycle of the sentinel output + hotkey ids
├── quickstart.md        # Phase 1: manual verification procedure
├── contracts/
│   └── libobs-hotkey-api.md  # Phase 1: libobs APIs this plugin consumes
└── checklists/
    └── requirements.md  # Created by /speckit-specify; spec-quality checklist
```

### Source Code (repository root)

```text
src/
├── plugin-main.cpp     # OBS module entry (unchanged)
├── plugin-main.hpp
├── plugin-support.c.in # OBS module support boilerplate (unchanged)
├── plugin-support.h
├── manager.cpp         # SlotManager — TOUCHED: remove hotkey_group_source*
├── manager.hpp         #                 — TOUCHED: remove hotkey_group_source decls
├── slot.cpp            # SceneSlot — TOUCHED: rewrite register_hotkeys / unregister_hotkeys;
│                       #             add sentinel hotkey_out_ creation in ctor / dtor / update_config
├── slot.hpp            #             — TOUCHED: add obs_output_t* hotkey_out_, helpers
├── ui-dock.cpp         # (unchanged)
├── ui-dock.hpp
├── ui-slot-editor.cpp  # (unchanged)
└── ui-slot-editor.hpp

CMakeLists.txt           # (unchanged — no new files)
data/locale/en-US.ini    # (unchanged — hotkey descriptions are constructed at runtime)
buildspec.json           # (unchanged)
```

**Structure Decision**: Single-project OBS plugin. The change touches `src/slot.{hpp,cpp}` (per-slot hotkey ownership) and `src/manager.{hpp,cpp}` (remove the obsolete group-source helper and its shutdown teardown). No new translation units, no CMake changes, no resource changes.

## Phase 0 — Research

The full research is in [research.md](./research.md). One-line summary of each resolved question:

| Question | Decision |
|---|---|
| Is `obs_hotkey_register_output` feasible against a non-running output? | Yes — outputs are ref-counted shells; dispatch is unconditional; Settings groups by `obs_output_get_name`. |
| Would `obs_hotkey_register_frontend` be cleaner instead? | Simpler but cannot deliver FR-010 (per-slot group label). Rejected. |
| Reuse `rec_out_` or add a dedicated sentinel output? | Dedicated sentinel. `rec_out_` is created/destroyed per recording run with full encoder/audio attachment; mixing the hotkey-grouping role into it would entangle this fix with the encoder lifecycle. |
| What output type for the sentinel? | `"ffmpeg_muxer"` (already used by `rec_out_` — known-present in every OBS build; never started here, so type effectively doesn't matter). |
| Keep the existing `set_pending_hotkey_bindings` / `obs_hotkey_load` path, or migrate to `obs_output_create`'s `hotkey_data` arg? | Keep existing path. It's mechanism-agnostic and already correct; migration would add risk for no functional gain. |

## Phase 1 — Design & Contracts

### data-model.md

Captures the lifecycle of the new per-slot sentinel output and the two `obs_hotkey_id` fields, plus the rename / save / load state transitions. See [data-model.md](./data-model.md).

### contracts/libobs-hotkey-api.md

Lists the libobs API surface this plugin depends on, with the exact behavior the plugin relies on (so a future libobs upgrade can be vetted against this list). See [contracts/libobs-hotkey-api.md](./contracts/libobs-hotkey-api.md).

### quickstart.md

Manual verification procedure covering all four user stories from the spec. See [quickstart.md](./quickstart.md).

### Agent context update

Per the workflow, the `<!-- SPECKIT START --> ... <!-- SPECKIT END -->` block in the repo-root `CLAUDE.md` is updated to point to this plan file.

## Re-check Constitution after Phase 1

The Phase 1 artifacts confirm the gate evaluation: no new threads, no new shared state, no new locks, no new UI dependencies. The change is strictly local to the hotkey registration path. No Complexity Tracking entries.

## Complexity Tracking

> Empty — Constitution Check passed.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| — | — | — |
