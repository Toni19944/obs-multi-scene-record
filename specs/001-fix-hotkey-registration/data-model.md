# Data Model: Fix Hotkey Registration

This change does not introduce new persisted fields or new file-format keys. It adds **one in-memory resource per slot** (the sentinel `obs_output_t*`) and **removes one shared in-memory resource** (the `hotkey_group_source_` on `SlotManager`).

## New / changed in-memory state

### `SceneSlot::hotkey_out_` (new)

| Field | Type | Purpose | Lifetime | Guarded by |
|---|---|---|---|---|
| `hotkey_out_` | `obs_output_t*` | Inert sentinel output created solely so its name becomes the Settings > Hotkeys group label for this slot's two hotkeys. Type is `"ffmpeg_muxer"`. Never started. | Created in `register_hotkeys()` if currently null; released in `unregister_hotkeys()` and again in `~SceneSlot`. Always nulled after release. | Main thread only — same threading discipline as `hotkey_record_` / `hotkey_replay_`. No mutex needed beyond the existing pattern. |

Naming: the output's name (passed at `obs_output_create` time) is the string `"Multi-Scene Record: " + cfg_.name`. This is the literal group label users see in Settings > Hotkeys.

### `SceneSlot::hotkey_record_`, `hotkey_replay_` (unchanged)

Both `obs_hotkey_id` fields stay. Their only change is the registration call that initializes them (`obs_hotkey_register_output(hotkey_out_, …)` instead of `obs_hotkey_register_source(grp, …)` / `obs_hotkey_register_frontend(…)`).

### `SceneSlot::pending_hk_record_`, `pending_hk_replay_` (unchanged)

The binding-restoration machinery is unchanged. `register_hotkeys()` still calls `obs_hotkey_load(id, pending_*)` immediately after each registration and releases the array. Mechanism-agnostic — works identically whether the hotkey is output- or frontend-registered.

### `SlotManager::hotkey_group_source_` (removed)

| Field | Type | Disposition |
|---|---|---|
| `hotkey_group_source_` | `obs_source_t*` | **Removed.** Along with `SlotManager::hotkey_group_source()` and the shutdown teardown at `manager.cpp:28-31`. |

## State transitions

### Slot creation (user adds a slot at runtime, or `load_from` rebuilds slots)

```
SceneSlot ctor
  └─ hotkey_out_ = nullptr  (default)
  └─ hotkey_record_ = OBS_INVALID_HOTKEY_ID
  └─ hotkey_replay_ = OBS_INVALID_HOTKEY_ID
  └─ pending_hk_* set by SlotManager::load_from (if restoring) or left null (fresh add)
```

```
SlotManager::add_slot
  └─ slots_.emplace_back(...)
  └─ slots_.back()->register_hotkeys()   ← creates hotkey_out_ here
```

```
SlotManager::load_from (subsequent FINISHED_LOADING / SCENE_COLLECTION_CHANGED)
  └─ register_all_hotkeys
       └─ each SceneSlot::register_hotkeys()  ← creates hotkey_out_ here
```

### `register_hotkeys()` (idempotent fast-return if already registered)

```
if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) return;

hotkey_out_ = obs_output_create(
    "ffmpeg_muxer",
    ("Multi-Scene Record: " + cfg_.name).c_str(),
    nullptr,            // settings
    nullptr);           // hotkey_data — we restore via pending_* instead (R6)

if (hotkey_out_) {
    hotkey_record_ = obs_hotkey_register_output(
        hotkey_out_,
        ("multi_scene_rec.record." + cfg_.id).c_str(),
        ("Toggle Recording: " + cfg_.name).c_str(),
        &SceneSlot::on_record_hotkey, this);
    hotkey_replay_ = obs_hotkey_register_output(
        hotkey_out_,
        ("multi_scene_rec.save_replay." + cfg_.id).c_str(),
        ("Save Replay: " + cfg_.name).c_str(),
        &SceneSlot::on_save_hotkey, this);
} else {
    // Defensive fallback if obs_output_create failed (OOM or libobs not ready).
    // Hotkeys still work; they just land in the Front-End list, ungrouped.
    blog(LOG_WARNING, "[multi-scene-rec] sentinel output creation failed; "
         "registering hotkeys under Front-End instead");
    hotkey_record_ = obs_hotkey_register_frontend(
        ("multi_scene_rec.record." + cfg_.id).c_str(),
        ("Toggle Recording: " + cfg_.name).c_str(),
        &SceneSlot::on_record_hotkey, this);
    hotkey_replay_ = obs_hotkey_register_frontend(
        ("multi_scene_rec.save_replay." + cfg_.id).c_str(),
        ("Save Replay: " + cfg_.name).c_str(),
        &SceneSlot::on_save_hotkey, this);
}

// Existing pending-binding restore — unchanged.
if (pending_hk_record_) { obs_hotkey_load(hotkey_record_, pending_hk_record_); … }
if (pending_hk_replay_) { obs_hotkey_load(hotkey_replay_, pending_hk_replay_); … }
```

### `unregister_hotkeys()`

```
if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) {
    obs_hotkey_unregister(hotkey_record_);
    hotkey_record_ = OBS_INVALID_HOTKEY_ID;
}
if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID) {
    obs_hotkey_unregister(hotkey_replay_);
    hotkey_replay_ = OBS_INVALID_HOTKEY_ID;
}
if (hotkey_out_) {
    obs_output_release(hotkey_out_);
    hotkey_out_ = nullptr;
}
```

Order matters: both hotkey ids are unregistered first (their internal weak refs to `hotkey_out_` are released), then the strong ref `hotkey_out_` is released. This mirrors the order the current code uses for the group source in `SlotManager::shutdown()`.

### Slot rename (via `update_config` with a new `cfg_.name`)

The existing cycle in `update_config` already covers this with no new code in `update_config` itself — the destroy/create of `hotkey_out_` happens inside `unregister_hotkeys()` / `register_hotkeys()`:

```
update_config:
  capture_hotkey_bindings();   // save current key combos to pending_*
  unregister_hotkeys();         // destroys old hotkey_out_ (named with OLD name)
  register_hotkeys();           // creates new hotkey_out_ (named with NEW name),
                                // registers fresh hotkeys against it,
                                // re-applies bindings from pending_*
```

The user's bindings ride through unchanged. The Settings > Hotkeys label updates on the next refresh of the Settings dialog (the `hotkey_register` signal that `OBSBasicSettings` listens to at `:709` triggers a reload).

### Slot removal (`SlotManager::remove_slot`)

```
slot->stop();              // existing
slot->unregister_hotkeys(); // existing, now also releases hotkey_out_
slots_.erase(...);         // existing — ~SceneSlot runs, but hotkey_out_ is already null
```

### Plugin shutdown (`SlotManager::shutdown`)

```
obs_frontend_remove_event_callback(...)
obs_frontend_remove_save_callback(...)
stop_all();
unregister_all_hotkeys();   // each slot now also releases its own hotkey_out_
slots_.clear();              // ~SceneSlot is a no-op for hotkey_out_ at this point

// REMOVED: the obs_source_release(hotkey_group_source_) block
```

### Scene collection change

```
SCENE_COLLECTION_CHANGING:
  stop_all();
  unregister_all_hotkeys();     // hotkey_out_ released per slot

  // (libobs then tears down the old scene collection's sources, including
  // the canvas; that no longer affects us since we don't hold any source.)

SCENE_COLLECTION_CHANGED / FINISHED_LOADING:
  // load_from has rebuilt slots_ by this point (via save_cb).
  register_all_hotkeys();        // each slot creates a fresh hotkey_out_
                                 // and registers both hotkeys against it.
                                 // pending_* bindings (set in load_from) are re-applied.
```

### Save (`SlotManager::save_to`)

Unchanged. `SceneSlot::save_hotkey_bindings(d)` reads from the live `hotkey_record_` / `hotkey_replay_` (or pending arrays if the hotkeys aren't registered at the moment of save) and writes the `hk_record` / `hk_save_replay` arrays into the slot's `obs_data_t`. The new output-registered hotkeys are saved by exactly the same code path.

### Load (`SlotManager::load_from`)

Unchanged. `set_pending_hotkey_bindings(hkr, hkp)` stashes the arrays on each newly-constructed slot; `register_hotkeys()` applies them when the frontend-event fires.

## Persisted format

**No change.** The keys `hk_record` and `hk_save_replay` inside each slot's per-slot `obs_data_t` continue to hold `obs_data_array_t` payloads from `obs_hotkey_save`. The internal `multi_scene_rec.record.<id>` / `multi_scene_rec.save_replay.<id>` hotkey names continue to embed `cfg_.id` and are preserved across migrations.

Backward compatibility with the current branch's save data: identical. The bug-fix change is invisible to the on-disk format.
