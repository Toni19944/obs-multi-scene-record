# Contract: libobs APIs consumed by hotkey registration

This document enumerates the exact libobs API surface this feature depends on. A future OBS Studio upgrade can be vetted by running through this list. Every entry cites the canonical header where it is declared.

## Used by this feature (post-change)

| API | Header | Behavior the plugin relies on |
|---|---|---|
| `obs_output_create(id, name, settings, hotkey_data) -> obs_output_t*` | `libobs/obs.h:1897` | Returns a refcounted output shell. Plugin passes `id = "ffmpeg_muxer"`, `name = "Multi-Scene Record: <slot name>"`, `settings = NULL`, `hotkey_data = NULL`. The output is **never started**; only its name and refcount matter. Returns `NULL` on failure → fallback path engages. |
| `obs_output_release(output)` | `libobs/obs.h:1899` | Releases the plugin's strong ref. Plugin holds exactly one strong ref per slot's `hotkey_out_`. |
| `obs_output_get_name(output) -> const char*` | `libobs/obs.h:1914` | Used by the Settings > Hotkeys UI (`frontend/settings/OBSBasicSettings.cpp:2892`) as the group section header. The plugin never calls this directly — OBS does — but it relies on the name being the literal string passed at create time. |
| `obs_hotkey_register_output(output, name, description, func, data) -> obs_hotkey_id` | `libobs/obs-hotkey.h:139` | Registers a hotkey with `OBS_HOTKEY_REGISTERER_OUTPUT`. Stores a weak ref to `output` internally. `func` is invoked unconditionally on every key-press transition (no active-output gate). Returns `OBS_INVALID_HOTKEY_ID` on failure. |
| `obs_hotkey_unregister(id)` | `libobs/obs-hotkey.h:236` | Releases the registerer weak ref and frees the hotkey row. Idempotent against `OBS_INVALID_HOTKEY_ID` — the plugin still guards with an explicit check before calling. |
| `obs_hotkey_register_frontend(name, description, func, data) -> obs_hotkey_id` | `libobs/obs-hotkey.h:198` | **Defensive fallback only** when `obs_output_create` returns null. Registers under `OBS_HOTKEY_REGISTERER_FRONTEND`. Same dispatch contract as `obs_hotkey_register_output`. |
| `obs_hotkey_save(id) -> obs_data_array_t*` | `libobs/obs-hotkey.h:259` | Snapshots the current key bindings on the live hotkey. Used by `SceneSlot::capture_hotkey_bindings` and `save_hotkey_bindings`. Mechanism-agnostic (works regardless of registerer type). Returns a new array the caller must release. |
| `obs_hotkey_load(id, data) -> void` | `libobs/obs-hotkey.h:258` | Applies a saved binding array to a live hotkey id. Used by `register_hotkeys` to consume `pending_hk_*`. The plugin retains ownership of `data` and releases it afterwards. |
| `obs_data_array_release` / `obs_data_get_array` / `obs_data_set_array` | `libobs/obs-data.h` | Standard reference handling for the `hk_record` / `hk_save_replay` arrays in scene-collection save data. Unchanged from current code. |

## Removed by this feature

| API | Header | Why removed |
|---|---|---|
| `obs_source_create_private(id, name, settings)` (for `id = "scene"`) | `libobs/obs.h` | The "private scene" workaround for hotkey grouping is replaced; no other code path in this plugin uses it. |
| `obs_source_release` (against `hotkey_group_source_`) | `libobs/obs.h` | The group source it accompanied is gone. (`obs_source_release` is still used elsewhere in this codebase against the shared encoder's scene source; that's a different call site.) |
| `obs_hotkey_register_source(source, …)` | `libobs/obs-hotkey.h` | Replaced by `obs_hotkey_register_output`. |

## Behavioral guarantees the plugin assumes

These are the contracts the plugin's correctness rests on. If a future OBS release changes any of them, this code must be revisited.

1. **Hotkey dispatch is unconditional on registerer state.** A hotkey registered against an output fires when its key binding is pressed regardless of whether the output is currently active/started. Evidence: `libobs/obs-hotkey.c:1010-1018` and `:1025-1033`. The slot's "Toggle Recording" hotkey starts a stopped slot — this only works because libobs does not gate on output activity.
2. **Output refcount keeps the output alive across the Settings > Hotkeys UI lookup.** OBS holds a weak ref via the hotkey registry; the plugin's strong ref (`hotkey_out_`) is what keeps the weak ref upgradable when the user opens the Settings dialog. Evidence: `obs-hotkey.c:215-227` (stores weak ref) and `OBSBasicSettings.cpp:2758-2767` (upgrades weak ref).
3. **Settings > Hotkeys groups output-registered hotkeys by `obs_output_get_name`.** Evidence: `OBSBasicSettings.cpp:2892` calls `AddHotkeys(layout, obs_output_get_name, outputs)`. The plugin's per-slot name `"Multi-Scene Record: <slot name>"` is what becomes the user-visible section header.
4. **`obs_output_get_name` returns the literal name passed at `obs_output_create` time and never changes.** No `obs_output_set_name` exists in `libobs/obs.h`. The plugin relies on this to mean "destroy + recreate is the only way to change the group label" — and that's what `update_config` does via the existing capture→unregister→register cycle.
5. **`obs_hotkey_load` is mechanism-agnostic.** A binding array produced by `obs_hotkey_save` against an output-registered hotkey can be applied via `obs_hotkey_load` to a freshly-registered hotkey of the same name, regardless of registerer type. This is what lets the plugin's existing pending-binding mechanism survive the source→output migration without any save-format change.
