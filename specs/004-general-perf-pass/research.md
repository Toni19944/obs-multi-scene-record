# Phase 0 Research: General performance pass (non-recording subsystems)

This document is the audit deliverable for User Story 4 (P3) and FR-006 / FR-007. Every subsystem in scope was read; every finding has a disposition. Anchors point to specific file/line numbers in `src/` as of the start of this feature (post feature-003 commit `56bcc1e`).

**Audit reference**: this codebase under `src/`. Comparison baseline: "the minimum work required for correctness" — i.e., if a behavior runs without contributing to a correctness guarantee, it's a candidate for closure.

**Disposition legend** (per spec FR-007):

- **CLOSE** — fixed in this feature.
- **KEEP / at parity** — no divergence to close; the subsystem already does the minimum work.

---

## Subsystem 1: Stats poll (1 Hz QTimer)

**Code**: `src/ui-dock.cpp:106-108` (timer creation), `:117` (initial start), `:196-265` (`refresh_stats()`).

### F1 — Timer runs continuously when no slots are recording

`stats_timer_` is started at dock-init time whenever the user's `stats_enabled_` preference is true (line 117). It then ticks once per second and invokes `refresh_stats()`. There is **no gate** on "any slot is actually running" — the timer keeps ticking even when every slot is idle.

`refresh_stats()` itself does early-out at the `s->is_running()` checks per row (lines 247, 249, 255 for COL_FRAMES / COL_DROPPED / COL_KBPS), and `SceneSlot::stats()` at `src/slot.cpp:1029-1033` also early-returns on `!running_.load()`. So each tick is cheap when nothing is running — but it's still N rows × a few atomic loads + ~5 Qt cell-text "no-op" updates per row per second, indefinitely, just for *having the dock open*. That violates FR-002 ("background work that runs while no slot is recording MUST be reduced to the minimum required for correctness").

**Cost characterisation**: small per-tick (<100 µs for 10 stopped slots), but unbounded over time. With OBS open all day and the plugin loaded, this adds up. More importantly, it's clearly avoidable — there's nothing the user can possibly *learn* from a stats refresh when no recordings are active.

**Disposition**: **CLOSE**.

**Fix**:

1. Add a public method `bool SlotManager::any_running() const` that takes `mtx_` and scans `slots_`, returning true if any has `is_running()`. Cheap (atomic load per slot under one lock acquisition).
2. In `MultiSceneRecordDock::refresh()`, after the existing `mgr.generation()` / `mgr.slot_count()` work, decide whether the stats timer should be active:
   - If `stats_enabled_` is true AND `mgr.any_running()` is true: start the timer if not already running.
   - Otherwise: stop the timer if running.
3. This logic lives in `refresh()` (not in `refresh_stats()`) because `refresh()` is called after every state transition (add, remove, edit, start, stop, hotkey toggle, external stop). The timer state stays in sync with the slot state without needing a separate callback.
4. `on_stats_toggled` keeps its existing user-preference logic but also calls the same any-running check when enabling — so toggling "Show stats" on while no slot is running doesn't start a pointless timer.

**Side benefit**: paused timer = zero Qt timer-event delivery cost on the UI thread, slightly improving worst-case latency for unrelated UI work.

---

## Subsystem 2: Dock refresh paths

**Code**: `src/ui-dock.cpp:140-194` (`refresh()`), `:196-265` (`refresh_stats()`), `:267-326` (action handlers).

### F2 — `refresh()` recreates cell-widget buttons on every call

`refresh()` (line 140) is called after every user/state action: `on_add`, `on_edit`, `on_remove`, `on_start_all`, `on_stop_all`, `on_state_clicked`, plus the cross-thread paths from slot.cpp (`on_record_hotkey`, `on_rec_output_stop`, `on_replay_output_stop`) via `QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection)`.

Inside `refresh()`, the per-row loop at line 149-192 unconditionally:

```cpp
auto* sb = new QPushButton(state_btn_text(running, c.replay_only));
sb->setFlat(true);
sb->setCursor(Qt::PointingHandCursor);
sb->setStyleSheet(state_btn_style(running));
connect(sb, &QPushButton::clicked, this, [this, i]() { on_state_clicked(i); });
table_->setCellWidget(i, COL_STATE, sb);
```

`setCellWidget` deletes the previous widget at that cell and installs the new one. So a single-row state change (e.g., user presses the bound hotkey for slot 0 with 10 slots configured) destroys and reallocates 10 `QPushButton` instances on the UI thread. That's per-button: an allocation, a parent re-parent, a style recompute, a signal-slot disconnect+reconnect.

`refresh_stats()` already has the right pattern at lines 218-223 — it mutates the existing cell widget in place via `qobject_cast<QPushButton*>(table_->cellWidget(...))` and `setText` / `setStyleSheet`. `refresh()` should do the same.

This violates FR-005 ("if a stats refresh sees only one slot has changed state, the per-row update MUST NOT trigger a full table rebuild unless required for layout correctness"). Strictly the FR is about `refresh_stats()` — which is fine — but the spirit applies to `refresh()` too: a state-only change should not force a full button rebuild.

**Disposition**: **CLOSE**.

**Fix**: in `refresh()`'s row loop, look up the existing state-button widget before creating a new one:

```cpp
QPushButton* sb = qobject_cast<QPushButton*>(table_->cellWidget(i, COL_STATE));
if (!sb) {
    sb = new QPushButton(...);
    sb->setFlat(true);
    sb->setCursor(Qt::PointingHandCursor);
    connect(sb, &QPushButton::clicked, this, [this, i]() { on_state_clicked(i); });
    table_->setCellWidget(i, COL_STATE, sb);
}
sb->setText(state_btn_text(running, c.replay_only));
sb->setStyleSheet(state_btn_style(running));
```

This naturally handles both cases: row-count growth (new rows have null cell widgets and get fresh buttons), and unchanged-row state transitions (existing buttons are mutated in place). When `setRowCount(n)` shrinks the table, Qt automatically destroys cell widgets for removed rows; no leak.

**Lambda capture concern**: the lambda captures `i` by value. When a row index changes (e.g., user removes slot 2 in a 5-slot list, rows shift), the existing button at "row 4" still has a lambda capturing `4` — but `refresh()` after a remove also calls `setRowCount(n-1)` and the iteration rewrites every remaining row with a fresh button via the `if (!sb)` path... wait, no — `setRowCount(n-1)` only deletes the removed row's cell widget; it does NOT touch the others. So existing buttons in rows 0..3 keep their original lambdas capturing 0..3 — which still refer to the correct (now-current) slot indices because rows 0..3 didn't shift. The captured indices match the current row positions. ✓ No issue. The lambda is correct because rows below a deletion all retain their original positions; only rows ABOVE the deletion shift up (i.e., rows at indices > removed_index slide down by one) — but in a `QTableWidget::removeRow` operation that's true, whereas `setRowCount(n-1)` just truncates from the bottom. The `setRowCount(n)` pattern preserves rows 0..n-1.

Actually wait — `setRowCount(n)` to a smaller `n` discards bottom rows. So if the user removes slot 2 from a 5-slot list, the dock currently calls `remove_slot(2)` (which erases slots_[2] and shifts 3→2, 4→3), then `refresh()`. In refresh(), `setRowCount(4)` truncates the bottom row (was row 4) and keeps rows 0..3. The button widget at row 4 is destroyed. Buttons at rows 0..3 are kept, but their lambdas still capture i=0..3 respectively. Slot manager now reports `slots_[2]` as the OLD slots_[3] (shifted up). The button at row 2 still has a lambda with `i=2`, which when clicked calls `on_state_clicked(2)` — which calls `slot_at(2)` — which gets the new slot at index 2. **Correct behavior**, no extra work needed. The lambda's captured index refers to its row position, and that's what `on_state_clicked` uses to look up the current slot via `slot_at(row)`.

So F2's fix is safe under the existing row-shifting semantics. The reuse pattern preserves correctness.

### F3 — `refresh_stats()` sets identical text on stopped-slot stat cells

For a stopped slot, `refresh_stats()` calls `setText("--")` on COL_FRAMES, COL_DROPPED, COL_KBPS each tick (lines 246-255). Even when those cells have been "--" since the last tick, the calls execute.

**Cost**: `QTableWidgetItem::setText` early-outs internally if the text is identical (Qt source-level optimization). The per-call cost is a couple of branches and a string comparison. Per stopped slot per tick: <1 µs. With 10 stopped slots that's ~10 µs/tick.

**Disposition**: **KEEP**. The cost is below the noise floor and avoiding it requires either (a) tracking per-cell last-value state in the dock (more code than the cost it saves) or (b) skipping the setText calls entirely for stopped slots (would skip the "active → stopped" transition update unless explicitly handled). Not worth it.

### F4 — `refresh()` calls `slot_at()` N times under separate lock acquisitions

`refresh()`'s row loop at line 150 calls `mgr.slot_at(i)` per iteration. Each call takes `SlotManager::mtx_` briefly and releases it. For N=10 slots, that's 10 lock acquisitions.

**Cost**: an uncontended `std::mutex` lock/unlock pair is ~20-50 ns on modern hardware. ~500 ns total for 10 slots. Below measurable.

**Disposition**: **KEEP**. Restructuring to a single locked snapshot would either complicate the lock-order story (holding `mtx_` across UI work is undesirable) or require allocating a vector copy of slot pointers (more work than the lock-release-reacquire it saves). Not worth it.

### F5 — `on_start_all` path-validation loop

`on_start_all()` (line 300-325) iterates `mgr.slot_count()` and calls `mgr.slot_at(i)` per iteration to check `c.path.empty()`. Same pattern as F4. **KEEP**.

---

## Subsystem 3: Slot lifecycle (SlotManager, SceneSlot non-pipeline state)

**Code**: `src/manager.cpp` (CRUD operations, frontend callbacks, save/load), `src/slot.cpp` (lifecycle hooks, hotkey handlers).

### F8 — Frontend event callback only does work on relevant events

`SlotManager::frontend_event_cb` (`src/manager.cpp:529-556`) switches on the `obs_frontend_event` enum and only handles `FINISHED_LOADING`, `SCENE_COLLECTION_CHANGED`, `SCENE_COLLECTION_CHANGING`, `PROFILE_CHANGING`, `EXIT`. All other events hit the default `break;` with zero work. OBS fires plenty of events (`SCENE_CHANGED`, `RECORDING_STARTED`, etc.) that the plugin correctly ignores.

**Disposition**: **KEEP / at parity**. Already minimum work.

### F9 — Save/load callback latency

`SlotManager::save_to` (`src/manager.cpp:465-486`) iterates `slots_` once, writing each slot's `obs_data_t` (one `slot_to_data` call per slot + one `save_hotkey_bindings` call). The `slot_to_data` function (`:295-350`) does ~50 `obs_data_set_*` calls per slot — these are hash-table-of-strings inserts inside libobs's `obs_data_t`. `save_hotkey_bindings` writes two `obs_data_array_t` arrays.

For 10 slots: ~500 hash-table inserts + 20 array writes. libobs's `obs_data_t` is a simple hash table; per-insert cost is sub-microsecond. Total `save_to`: well under 1 ms for 10 slots.

`load_from` is symmetric: ~50 `obs_data_get_*` reads per slot + 2 array reads + `SceneSlot` construction. Same order of magnitude.

**Disposition**: **KEEP / at parity**. Well inside FR-004's "<16 ms for ≤10 slots" budget.

### F10 — Lifecycle CRUD operations

`add_slot` / `remove_slot` / `update_slot` (`src/manager.cpp:68-116`) all take `mtx_` briefly, perform their mutation, release the lock, and let the UI refresh. No expensive work inside the locked section. The CRUD work is dominated by the subsequent UI refresh (which F2 addresses).

**Disposition**: **KEEP / at parity**.

---

## Subsystem 4: SlotEditor

**Code**: `src/ui-slot-editor.cpp`.

### F6 — Editor populates combos on every open

Opening the editor (via Add or Edit in the dock) constructs a new `SlotEditor` dialog. The constructor calls (at minimum): `populate_scene_combo` (line 83), `populate_video_encoder_combo` (130), `populate_audio_encoder_combo` (177), `populate_rate_control_combo` (191), plus encoder-specific UI updates.

`populate_video_encoder_combo` enumerates OBS encoder types via `obs_enum_encoder_types` (line 447) — ~10-20 entries in a typical install. Each entry's display name comes from `obs_encoder_get_display_name`. Then it appends shared-encoder entries by scanning `SlotManager`'s slots.

`update_encoder_specific_ui` (called from `populate_video_encoder_combo`'s selection path) introspects the currently-chosen encoder's properties via `obs_get_encoder_properties` and queries multiple property keys — this allocates an `obs_properties_t*` for the encoder and traverses it.

**Cost**: probably 20-80 ms total in the typical case. Opening a dialog is a user-initiated event with FR-001's ~100 ms budget; this fits.

**Disposition**: **KEEP**. Within budget. If quickstart benchmarks show it exceeding 100 ms in practice, revisit — but the audit-by-inspection doesn't predict that.

---

## Subsystem 5: Memory baseline

### F7 — Per-stopped-slot static cost

Each stopped slot holds:

- The `SceneSlot` object itself (a few hundred bytes — Config struct + mutex + atomics + std::vector of audio encoders empty + handles set to null + cached hotkey IDs).
- Two `obs_hotkey_id` registrations (≤8 bytes each, payload in libobs's hotkey registry).
- One `hotkey_out_` sentinel output (`obs_output_t*`, ~1 KB allocation from libobs).

Roughly **~1.5 KB per stopped slot** in plugin-attributable memory.

**Disposition**: **KEEP** — already minimal. The number is recorded in [quickstart.md](./quickstart.md) as institutional memory.

---

## Subsystem 6: Idle behavior summary

Combining all of the above:

- **Plugin loaded, 0 slots, stats disabled**: zero recurring work. Dock exists but does nothing per-frame.
- **Plugin loaded, 0 slots, stats enabled (default)**: timer fires 1×/sec → `refresh_stats()` early-returns on `mgr.slot_count() != table_->rowCount()` mismatch path (line 208)... actually, no — both are 0, so it doesn't early-return; the for-loop just iterates 0 times. Effectively a no-op tick. But the Qt timer-event delivery itself has cost (kernel timer interrupt → Qt event loop dispatch). After F1's fix, no timer runs in this state at all.
- **Plugin loaded, N stopped slots, stats enabled**: pre-fix: timer fires 1×/sec, iterates N rows, all `is_running()` checks return false, cheap "--" setText calls. Post-fix: timer paused; zero per-second work.
- **Plugin loaded, K running + (N-K) stopped slots, stats enabled**: pre- and post-fix identical here — timer runs because K > 0.

The F1 fix is exactly aligned with this idle-cost taxonomy.

---

## Items considered and ruled out

- **Coalescing rapid refresh() invocations**: if a user spams Start All / Stop All, multiple `refresh()` calls queue. Qt's event loop processes each. With F2's cell-widget reuse, each is cheap; coalescing would save little. **Not worth a separate fix.**
- **Static caching of encoder enumeration in SlotEditor**: cache `obs_enum_encoder_types` result once at plugin init. **Skipped** — F6 is already within budget; caching adds invalidation complexity.
- **Pre-allocating QPushButton instances for max-likely slot count**: a "pool" of buttons reused as rows come and go. **Skipped** — Qt's setRowCount + setCellWidget already handles this naturally; manual pooling adds complexity for negligible gain over F2's fix.
- **Background thread for stats sampling**: move `refresh_stats` work off the UI thread. **Skipped** — the stats work is already cheap (per-slot atomic loads + early-out); the only meaningful cost is the timer-event delivery and Qt cell updates, both of which must be on the UI thread anyway.
