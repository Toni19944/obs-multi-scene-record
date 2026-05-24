# Phase 0 Research: Fix Hotkey Registration

This research grounds the implementation choices made in [plan.md](./plan.md). Every claim is anchored to a specific file/line in `libobs` (OBS Studio source tree at `D:\Programs\Tools\obs-dev-kit\obs-studio`) or in this plugin's current `src/`.

## R1: Is `obs_hotkey_register_output(...)` viable against a long-lived, never-started output?

**Decision**: Yes.

**Evidence**:

- `obs_output_create(id, name, settings, hotkey_data)` (`libobs/obs.h:1897`, `libobs/obs-output.c:196`) returns a refcounted output shell. None of its later operations are pre-required: no encoder must be attached, no audio mixer set, no `obs_output_start` called. The output is valid as soon as its handlers are initialized.
- `obs_hotkey_register_output(output, name, description, func, data)` (`libobs/obs-hotkey.c:215-227`) takes a strong `obs_output_t*`, stores a weak ref, and tags the registered hotkey with `OBS_HOTKEY_REGISTERER_OUTPUT`. It performs **no check** on whether the output is active, has settings, or has encoders bound.
- Hotkey dispatch (`libobs/obs-hotkey.c:1010-1018` and `:1025-1033`) invokes `hotkey->func(hotkey->data, hotkey->id, hotkey, pressed)` unconditionally whenever a bound key transitions. There is **no gating** on registerer activity, scene visibility, or output state. The function fires whether the slot is recording or idle.
- The Settings > Hotkeys dialog upgrades the output's weak ref via `OBSGetStrongRef(weak_output)` (`frontend/settings/OBSBasicSettings.cpp:2758-2767`) and uses `obs_output_get_name(output)` as the section header (`:2892`). It iterates **all** hotkeys via `obs_enum_hotkeys` (`:2847-2854`); long-lived outputs created by a plugin are not filtered out.

**Practical implication**: a sentinel `obs_output_t*` per slot is sound. Holding one for the slot's full lifetime costs ~one output struct + signal handler init; pressing a hotkey costs the same single function call it costs today.

## R2: Why is the current `obs_source_create_private("scene", …)` approach fragile?

**Decision**: Replace it. The user's diagnosis ("libobs doesn't natively support custom hotkey groups") is essentially correct — the workaround swims upstream of libobs's contracts.

**Evidence**:

- The current code (`src/manager.cpp:158-184`) calls `obs_source_create_private("scene", "Multi-Scene Record", nullptr)` and registers per-slot hotkeys against that private scene. The Settings UI does dispatch source-registered hotkeys to the `scenes` vector (`frontend/settings/OBSBasicSettings.cpp:2780-2793`) and label them via `obs_source_get_name`, so on paper the grouping should appear.
- In OBS 31+, `scene_info.output_flags` (`libobs/obs-scene.c:1742-1759`) carries `OBS_SOURCE_REQUIRES_CANVAS`. Scenes are designed to be created via `obs_scene_create` against a canvas owned by the active scene collection — not via `obs_source_create_private` outside that lifecycle. Behavior of a canvas-less private scene is undefined territory; future OBS releases are free to tighten this.
- OBS's source save loop (`libobs/obs.c:2437-2472`) iterates the public source registry. A private source is invisible to it, so the plugin already has to save/load bindings manually (`SceneSlot::save_hotkey_bindings`, `set_pending_hotkey_bindings`). The custom path works today but is one race away from losing a binding across `SCENE_COLLECTION_CHANGING/CHANGED`.
- Net: the workaround relies on undocumented behavior (private scene without a canvas) plus a manual save/load shadow path. The `obs_hotkey_register_output` route uses only documented contracts and lets libobs's normal weak-ref/enumeration logic do the work.

## R3: Is `obs_hotkey_register_frontend(...)` a cleaner alternative?

**Decision**: Cleaner for the registration code itself, but **rejected** for this feature.

**Evidence**:

- `obs_hotkey_register_frontend` (`libobs/obs-hotkey.h:198`) requires no backing object; the hotkey is filed under `OBS_HOTKEY_REGISTERER_FRONTEND` and the Settings UI just `addRow(label, hw)`s it directly (`OBSBasicSettings.cpp:2808-2810`). No sentinel object, no rename-handling.
- However, every frontend-registered hotkey lives flat under the single "Front-End" section. The grouping FR-010 demands (`Multi-Scene Record: <slot name>` per slot) is **impossible** with this API — there is no per-hotkey group label, only the registerer-type bucket.
- Rename handling complexity is **not** actually a differentiator: both `obs_output_get_name` and the hotkey's description string are set at registration time and immutable thereafter (no `obs_output_set_name`, no `obs_hotkey_set_description` in `libobs/obs.h` or `obs-hotkey.h`). Either approach must unregister+re-register on rename. So the simplicity argument for `register_frontend` reduces to "fewer lines for the sentinel-output plumbing" — a few dozen LOC.

**Rationale for rejection**: Spec FR-010 was confirmed by the user; flattening would silently regress the explicitly-chosen UX.

## R4: Reuse `rec_out_` as the hotkey-grouping output, or add a dedicated sentinel?

**Decision**: Add a dedicated `hotkey_out_` per slot. Do not touch `rec_out_`.

**Evidence and rationale**:

- `rec_out_` is created in `SceneSlot::setup_outputs()` (`src/slot.cpp:702-…`) on every `start()` and destroyed in `teardown_locked()`. Lifting it to slot-lifetime would entangle this hotkey fix with the encoder/audio binding lifecycle (see also `setup_encoders`).
- A replay-disabled slot has no `replay_out_` at all, yet must still register both hotkeys (FR-011). A sentinel output is uniform across slot configurations — `replay_enabled` becomes irrelevant to the hotkey path.
- Constitution Principle II ("Clear Ownership"): a sentinel output has one job and one job only (hotkey grouping); `rec_out_` has its own job (recording). Mixing them blurs ownership and obscures the lifetime contract.
- Cost of the sentinel is trivial: one `obs_output_create("ffmpeg_muxer", name, NULL, NULL)` per slot, never started, plus one `obs_output_release` at slot destruction.

## R5: Output type for the sentinel?

**Decision**: `"ffmpeg_muxer"`.

**Rationale**: it's already used elsewhere in this plugin (`src/slot.cpp:710-…` for `rec_out_`), so it's known to be registered in every OBS build the plugin targets. The output is never started, so the type's actual behavior is irrelevant — only its registered presence matters. Choosing a type already used by this plugin avoids introducing a new dependency on a different output id that might not exist in some build (e.g., `"null_output"` is not guaranteed across all OBS versions).

## R6: Should we use `obs_output_create`'s `hotkey_data` arg to atomically restore bindings, or keep the existing pending-binding mechanism?

**Decision**: Keep the existing mechanism (`set_pending_hotkey_bindings` → `obs_hotkey_load` in `register_hotkeys`).

**Evidence**:

- The plugin already implements binding persistence end-to-end (`src/slot.cpp:842-895`, `src/manager.cpp:473-477`, `:510-512`). It works correctly for the source-registration path and is mechanism-agnostic — `obs_hotkey_load` takes an `obs_hotkey_id` regardless of registerer type.
- `obs_output_create`'s `hotkey_data` arg (`libobs/obs-output.c:187-218`) routes through `obs_context_data_init`, which stashes the data on `output->context.hotkey_data`. The bindings only attach when a hotkey is registered against the output AND when `obs_hotkeys_load_*` resolves them. Wiring this is *cleaner* in isolation but introduces new ordering: the bindings would only apply to hotkeys created in the SAME create→register sequence, which is exactly what we already get from `set_pending_hotkey_bindings` + `obs_hotkey_load` immediately after the register call.
- Net: equivalent correctness, more churn, more risk. Keep the existing pending path. The change is mechanism-local.

## R7: Rename handling — when does the sentinel get destroyed and recreated?

**Decision**: On every `SceneSlot::update_config()` invocation, unconditionally, paired with the existing capture/unregister/register cycle.

**Evidence**:

- Current `update_config` (`src/slot.cpp:387-418`) already triggers `capture_hotkey_bindings()` → `unregister_hotkeys()` → `register_hotkeys()` when `had_hotkeys` is true. The reason today is to keep the hotkey description ("Toggle Recording: <slot name>") in sync after a rename.
- With sentinel outputs, the same cycle must additionally destroy the old sentinel output and create a new one with the new name. The ordering inside `unregister_hotkeys()` and `register_hotkeys()` becomes:
  - `unregister_hotkeys()`: `obs_hotkey_unregister(both ids)` → `obs_output_release(hotkey_out_)` → `hotkey_out_ = nullptr`.
  - `register_hotkeys()`: `hotkey_out_ = obs_output_create("ffmpeg_muxer", group_name, NULL, NULL)` → `obs_hotkey_register_output(hotkey_out_, …)` for each hotkey.
- Unconditional destroy+recreate (rather than "only when name changed") keeps the path uniform and avoids a name-equality check; the work is cheap.

## R8: Lifetime ordering at shutdown / scene-collection-change

**Decision**: Sentinel output is released in `~SceneSlot` (or in `unregister_hotkeys()` when called from `update_config` / `remove_slot` / frontend `*_CHANGING`), after both `obs_hotkey_unregister` calls.

**Evidence**:

- `obs_hotkey_register_output` stores a weak ref to the output (`libobs/obs-hotkey.c:222`). The weak ref is released in `release_registerer` (`obs-hotkey.c:820-841`) when the hotkey is unregistered. The strong ref held by the plugin (`hotkey_out_`) keeps the output alive for the weak-ref upgrades the Settings UI performs.
- Releasing `hotkey_out_` before `obs_hotkey_unregister` would leave the weak ref dangling momentarily. The risk is low (Settings UI does the upgrade under libobs's hotkey lock), but ordering the unregister first matches the pattern the current code already uses for the source case (`src/manager.cpp:20`-`:31`).
- Sequence at module shutdown: `obs_frontend_remove_event_callback` → `stop_all` → `unregister_all_hotkeys` → `slots_.clear()` (each `~SceneSlot` releases its `hotkey_out_`).
- Sequence at `SCENE_COLLECTION_CHANGING`: `stop_all` → `unregister_all_hotkeys` (which now also releases per-slot `hotkey_out_`). When `load_from` rebuilds `slots_`, each new `SceneSlot` constructor creates a fresh `hotkey_out_`, and `FINISHED_LOADING` / `SCENE_COLLECTION_CHANGED` then calls `register_all_hotkeys`. Note: the spec preserves "frontend-event-driven registration timing" — so `register_hotkeys()` must still tolerate being called when `hotkey_out_` already exists (idempotent fast-return, just like today).

## Alternatives considered (all rejected)

- **Use a single shared sentinel output across all slots** (Q1 option C from the spec). User chose plugin-prefixed per-slot grouping. Even setting that aside, a single shared output would tag every slot's hotkey with the same group label and obscure which row belongs to which slot in Settings.
- **Use `obs_hotkey_register_source` against the slot's actual scene** (the one named in `cfg_.scene_name`). The scene is a public source the user already sees in the OBS Scenes panel, so the hotkey would appear nested under a regular scene. Cross-contaminates UI semantics (one of the user's scenes suddenly carries this plugin's hotkeys) and breaks when the scene is renamed/deleted from outside the plugin.
- **Use the `hotkey_data` parameter of `obs_output_create`** to atomically restore bindings: see R6 — equivalent correctness for more churn.
