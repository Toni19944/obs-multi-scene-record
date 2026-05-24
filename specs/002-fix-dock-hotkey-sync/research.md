# Phase 0 Research: Fix Dock UI sync after hotkey-triggered recording

The fix is small — most of the "research" is confirming the existing patterns in this codebase and verifying that re-using them satisfies every spec requirement. Each finding is anchored to a specific file:line in `src/`.

## R1: Which callback(s) need the refresh?

**Decision**: Only `SceneSlot::on_record_hotkey` (`src/slot.cpp:909-920`). `SceneSlot::on_save_hotkey` (`src/slot.cpp:954-960`) is unchanged.

**Evidence**:

- `on_record_hotkey` calls `start()` or `stop()` — both flip `running_` and therefore change the visible state of the slot. The dock state column is rendered from `is_running()`, so it MUST refresh.
- `on_save_hotkey` calls `save_replay()`, which writes a clip but does NOT change `running_`. The dock state column has no reason to redraw. FR-008 explicitly excludes this path.
- The external-stop signal handlers (`on_rec_output_stop` at `:922-937`, `on_replay_output_stop` at `:939-952`) already call `refresh()` because they also change `running_`. They are unchanged.

## R2: What refresh mechanism?

**Decision**: `if (auto* dock = get_dock()) QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);`

**Evidence**:

- This exact two-line pattern is used in `src/slot.cpp:965-967` and `:980-982` for the external-stop case. It is the codebase's idiomatic way to notify the dock from non-UI threads.
- `get_dock()` is declared in `src/plugin-main.hpp:9` and defined in `src/plugin-main.cpp:19`. It returns the global `g_dock` pointer (set during dock creation by OBS's frontend). When the dock has not been created yet — e.g., during early plugin init or after a dock teardown — `g_dock` is null and the `if` guard makes the call a no-op. This satisfies FR-004 ("safe when dock is closed/hidden").
- `MultiSceneRecordDock::refresh()` (declared in `src/ui-dock.hpp:23`) is a public Qt slot. `QMetaObject::invokeMethod(... , "refresh", Qt::QueuedConnection)` finds it by name; no header changes needed in slot.cpp (it already includes `<QMetaObject>` per `src/slot.cpp:10`).
- `Qt::QueuedConnection` posts the invocation to the receiver's event loop (the UI thread, since `MultiSceneRecordDock` is a QWidget). This satisfies Constitution Principle III (the hotkey callback's libobs thread never touches Qt widgets) and Principle IV (slot.cpp does not call dock methods directly).

## R3: Refresh before or after the start/stop call?

**Decision**: After.

**Evidence**:

- `start()` returns false and resets `running_` to false on a failed start (e.g., missing output path, encoder failure). This is documented in `src/ui-dock.cpp:356-360` for the dock-click path: `// start() returns false and resets running_ on an empty/missing output path, so a failed start leaves the row showing "off" rather than a fake running state.`
- The refresh queues onto the UI thread; by the time `refresh()` runs there, the `start()` call has returned and `is_running()` reports the final state. Refreshing AFTER (rather than during or before) the call guarantees FR-003 ("no transient 'active' flicker on failed start").

## R4: Coalescing / debouncing for rapid hotkey presses?

**Decision**: None. Let `Qt::QueuedConnection` serialize naturally.

**Evidence**:

- Queued invocations are processed in FIFO order on the receiver's event loop. If the user presses the hotkey N times quickly, N refreshes queue up; the last refresh always sees the slot's final `is_running()` state and reaches the correct outcome.
- The cost of an extra `refresh()` is rebuilding the table rows from `SlotManager` — already done routinely on click-toggle and on stats refresh; non-issue for the N < ~10 presses/second a human can produce.
- Adding debouncing logic would introduce its own state and timing complications without observable benefit. Reject.

## R5: Why isn't this already handled by `on_rec_output_stop`?

**Decision**: Because `on_rec_output_stop` deliberately ignores intentional stops (code 0 = `OBS_OUTPUT_SUCCESS`).

**Evidence**:

- `src/slot.cpp:927-929`: `if (code == OBS_OUTPUT_SUCCESS) return;` — the handler returns early on an intentional stop, including the stop triggered by our own `obs_output_stop()` in `teardown()`. This is correct behavior (avoids double-teardown), but it means the dock-refresh fallback at `:966-967` only fires for EXTERNAL stops. For a hotkey-initiated stop, the refresh never gets posted.
- This is why the bug exists in the first place: the dock-refresh path only covers the click case and the external-failure case. The hotkey-initiated path was never wired up. This change closes that gap.

## R6: Are there any race conditions to worry about?

**Decision**: No new races are introduced; the existing pattern is race-safe.

**Evidence**:

- The hotkey callback can run on libobs's hotkey worker thread. By the time `start()` / `stop()` returns, the slot's `running_` atomic is in its final value (those calls update it before returning).
- `QMetaObject::invokeMethod` with `Qt::QueuedConnection` is documented as thread-safe — it posts an event to the receiver's thread queue.
- `refresh()` on the UI thread reads `is_running()` via `SlotManager::slot_at(i)->is_running()`. `is_running()` is `running_.load()` (atomic, lock-free). Worst case: if the user toggles fast enough that two refreshes are queued and a third toggle happens before the queue drains, each `refresh()` still reads a consistent snapshot — the final refresh wins. No corruption.
- Slot removal during a queued refresh: `SlotManager::remove_slot` takes `mtx_` and erases the slot; the queued refresh later runs and reads the now-shorter slots vector. `MultiSceneRecordDock::refresh` already handles this (it reads `slot_count()` fresh per iteration) — the row for a removed slot just doesn't appear. Same behavior as today's external-stop refresh path.

## Alternatives considered (all rejected)

- **Make `MultiSceneRecordDock` subscribe to a Qt signal emitted from `SceneSlot::start()` / `stop()`**: cleaner in principle, but requires adding signal/slot machinery to `SceneSlot`, which currently has no Qt types in its header. Constitution-tax (introducing Qt into the non-UI side) for a problem that has a one-line solution. Reject.
- **Hold a strong ref to the dock and call `dock->refresh()` directly**: violates Constitution Principle IV (UI/Logic Separation) — slot.cpp would touch Qt widgets directly from a non-UI thread, which is also a use-after-free hazard if the dock is destroyed mid-call. Reject.
- **Have `MultiSceneRecordDock::refresh_stats` (the 1Hz polling refresh) pick up the state change**: the 1-second polling delay would violate FR-001 / FR-002 ("within 1 second"). Possible but jankier than just posting an immediate refresh. Reject.
