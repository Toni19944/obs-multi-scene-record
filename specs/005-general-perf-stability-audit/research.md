# Research / Audit: General performance and stability across all source files

**Feature**: 005-general-perf-stability-audit
**Status**: Phase 0 deliverable — the audit document itself (US5 in spec.md)
**Date**: 2026-05-20

This document is the deliverable for User Story 5 of [spec.md](./spec.md). Each of the ten in-scope files has its own section listing findings on three axes (performance / stability / cleanup) with a disposition per FR-008 — either **(a) close in this feature** or **(b) accept with documented evidence**.

The audit is conducted as a multi-pass walk per FR-001. The auditor chose a **bottom-up order** (most depended-on first) for Pass 1: plugin-main → manager → slot → ui-dock → ui-slot-editor. The same order is used for every pass.

## Summary table

| ID | File | Axis | Finding | Disposition |
|---|---|---|---|---|
| F-PM1 | plugin-main.cpp | Stability | `obs_module_unload` removes the frontend event callback *after* `SlotManager::shutdown()`. A theoretical late-firing event between the two could re-create the dock against a shut-down manager. | **KEEP (b)** — frontend events and unload both run on the OBS UI thread; the race is not reachable. Documented. |
| F-M1 | manager.cpp | Perf+Cleanup | `stop_all()` holds `mtx_` across `s->stop()`, which can block up to 5 s per slot inside `wait_for_output_stop()`. `start_all()` already uses a snapshot pattern that releases `mtx_` first. The asymmetry is avoidable. | **CLOSE (a)** — mirror `start_all`'s snapshot pattern in `stop_all`. No signature change. |
| F-M2 | manager.cpp | Stability (latent) | `save_to`/`load_from` (under `mtx_`) reads hotkey-related fields (`hotkey_record_`, `hotkey_replay_`, `pending_hk_*`) that `SceneSlot::update_config` writes *without* `slot_mtx_`. | **KEEP (b)** — both code paths run on the OBS UI thread (save callback + dialog accept); no concurrent reach in practice. Documented. |
| F-S1 | slot.cpp | Cleanup | `SceneSlot::start()` redundantly stores `running_=true` at the bottom (line 551) after the CAS at the top already set it. | **CLOSE (a)** — remove the redundant store. |
| F-S2 | slot.cpp | Stability (latent) | Concurrent `stop()` during a `start()` setup phase can leave the slot in an inconsistent state. Both `start()` and `stop()` lock-step through `slot_mtx_`, but the CAS in `start()` and the `exchange()` in `stop()` race on `running_` itself. After F-S1's removal of the redundant `running_.store(true)` at the bottom of `start()`, the post-race state is `running_=false` with no outputs (the dock shows "off" — consistent with reality). Before F-S1, the post-race state was `running_=true` with no outputs (the dock fake-shows "REC"). | **KEEP (b)** — observed callers (UI thread for dock/state-toggle; OBS save / hotkey threads for callbacks) do not actually overlap; the timing window requires cross-thread near-simultaneous toggle. F-S1's fix tightens the post-race symptom but does not close the race window itself. Documented as a known latent race with a now-self-consistent symptom. |
| F-S3 | slot.cpp | Cleanup | `wait_for_output_stop` polls `obs_output_active` in 10 ms increments instead of waiting on a condition / signal. | **KEEP (b)** — bounded (5 s max), called only on teardown, low frequency. Closing would require a signal/condvar with no measurable benefit. Documented. |
| F-UD1 | ui-dock.cpp | Perf+Cleanup | `MultiSceneRecordDock::refresh()` allocates fresh `QTableWidgetItem`s for every text column on every call (8 allocations per row per refresh — feature 004 already addressed the cell-widget button via F2 but did not generalise the pattern to items). | **CLOSE (a)** — reuse the existing items via `table_->item(i, col)->setText(...)`; only allocate new on row insertion. No signature change. |
| F-UD2 | ui-dock.cpp | Cleanup | Per-row `mgr.slot_name_by_id()` in both `refresh()` and `refresh_stats()` takes `mtx_` an extra time per shared-encoder row. | **KEEP (b)** — `mtx_` is uncontended in this path, the lookup is O(slots) on a small N (≤10 typical), and the lookup is unconditionally cheap (string compare). Closing it would require building a name index snapshot first; complexity outweighs gain. Documented. |
| F-USE1 | ui-slot-editor.cpp | Perf+Cleanup | `update_encoder_specific_ui()` indirectly calls `obs_get_encoder_properties(enc_id)` four+ times for the same encoder in one invocation (each `populate_combo_from_encoder_property` call fetches and destroys its own properties object). | **CLOSE (a)** — fetch `obs_properties_t*` once at the top of `update_encoder_specific_ui` and pass it into the helper. Signature of the static helper changes — but it is a `static private` of `SlotEditor` and is not called from any other translation unit, so the change is NOT contract-affecting per FR-001 (no upstream revisit triggered). |
| F-USE2 | ui-slot-editor.cpp | Stability (theoretical) | A dialog `accept()` returns to `MultiSceneRecordDock::on_edit`, which then calls `mgr.update_slot(row, ...)`. Between dialog accept and `update_slot`, the row index could theoretically refer to a different slot (e.g., scene-collection load on another thread). | **KEEP (b)** — `load_from` runs from `save_cb`, which fires on the OBS UI thread. The dialog accept also runs on the UI thread. The two paths cannot interleave. Documented. |

**Net code change**: F-M1 + F-S1 + F-UD1 + F-USE1. Four closeable findings; six accepted-with-rationale.

## Pass log

### Pass 1 — bottom-up order (plugin-main → manager → slot → ui-dock → ui-slot-editor)

Executed during `/speckit-implement` (T003 / T004 / T005 / T006). Edits applied per the predicted plan; no surprises.

| File | Edits in this pass | Contract-affecting? | Triggers revisit? |
|---|---|---|---|
| `plugin-main.hpp` | none | — | no |
| `plugin-main.cpp` | none (F-PM1 = KEEP) | — | no |
| `manager.hpp` | none | — | no |
| `manager.cpp` | **F-M1** (`stop_all` snapshot refactor; T003) | no — function body only; signature unchanged | no |
| `slot.hpp` | none | — | no |
| `slot.cpp` | **F-S1** (remove redundant `running_.store(true)`; T006) | no — single-line removal inside `start()` | no |
| `ui-dock.hpp` | none | — | no |
| `ui-dock.cpp` | **F-UD1** (`refresh()` item-reuse refactor; T004) | no — function body only | no |
| `ui-slot-editor.hpp` | F-USE1 helper signature widened (`obs_properties_t*` replaces `const std::string& enc_id`); `static private` of `SlotEditor`, used only within `ui-slot-editor.cpp` | no — see Pass-2 grep result below | no |
| `ui-slot-editor.cpp` | **F-USE1** (`update_encoder_specific_ui` hoists `obs_get_encoder_properties` once; helper signature widened; T005) | no — see Pass-2 grep result below | no |

**Pass 1 produced 4 edits, zero contract-affecting** → convergence reached.

### Pass 2 — grep verification (T009)

Per FR-012, a verification grep scan was run across all ten files looking for stale references to each Pass-1 contract change. Result: **zero stale references**.

| Pass-1 edit | Symbol grepped | Where found | Stale? |
|---|---|---|---|
| F-M1 (`stop_all` body) | `SlotManager::stop_all` / `stop_all\(\)` | `manager.hpp:70` (decl), `manager.cpp:19/529/534` (self-call from shutdown / frontend handlers), `ui-dock.cpp:357` (dock's `on_stop_all`) — all callers see unchanged signature | no |
| F-S1 (removed store) | `running_\.store\(true\)` | only the F-S1 explanatory comment in `slot.cpp` — no other code site touched | no |
| F-UD1 (item reuse) | `mk_item` | still used at `ui-dock.cpp:158` as the allocate-on-empty fallback — same helper, fewer calls | no |
| F-USE1 (helper signature) | `populate_combo_from_encoder_property` | only `ui-slot-editor.hpp` (decl) + `ui-slot-editor.cpp` (def + 4 internal callers) — no other TU references the symbol | no |

**Pass 2 not required — Pass-2 grep scan (T009) found zero stale references; convergence confirmed.**

### Convergence rationale (per FR-001 + Clarifications 2026-05-20)

Per the Clarifications session, a pass is "non-empty" (triggers another pass) only when it produces a **contract-affecting edit** — a removed/renamed exported symbol, narrowed signature, removed branch / public condition, or tightened invariant. All four Pass-1 edits are internal to a function body or change a `static private` helper not used from any other translation unit. None of them rewrites a public contract the other files rely on. The methodology therefore converges in one pass on this code base.

This is the honest result — features 003 and 004 have left the codebase tight; this nitpicky pass picks up four small wins without disturbing any cross-file contract. The multi-pass framework remains in spec.md / FR-001 so any FUTURE audit on this file set has a defined convergence loop available; absence of revisits in this particular audit is not a methodology failure.

---

## Per-file findings detail

### plugin-main.hpp

- **Surface**: declares `get_dock()` (returns the live dock instance) and forward-declares `MultiSceneRecordDock`.
- **Perf axis**: nothing to audit; declarations only.
- **Stability axis**: forward-declaration pattern is the textbook way to avoid pulling Qt headers into worker-thread translation units. Correct.
- **Cleanup axis**: comment header explains the lifetime contract; up to date. No edits.

**Findings**: none.

### plugin-main.cpp

- **Surface**: `OBS_DECLARE_MODULE`, `obs_module_name`, `obs_module_description`, the `dock_create_cb` named function, `obs_module_load`, `obs_module_unload`.
- **Perf axis**: 56 LOC, no hot paths. Load and unload happen once. No work done in steady state.
- **Stability axis**:
  - `dock_create_cb` is a named function (not a lambda) so its address is stable for `obs_frontend_remove_event_callback`. Correct.
  - `g_dock = nullptr` at unload end is a defensive write — any worker thread that has already passed a `get_dock()` check but not yet invoked `QMetaObject::invokeMethod` will see `nullptr` and skip. Correct.
  - **F-PM1**: `obs_module_unload` calls `SlotManager::instance().shutdown()` BEFORE `obs_frontend_remove_event_callback(&dock_create_cb, nullptr)`. A theoretical `FINISHED_LOADING` event firing between the two would land in `dock_create_cb` which would call `obs_frontend_get_main_window()` and try to `new MultiSceneRecordDock(main_window)` against a shut-down manager. **Disposition (b)**: frontend events fire on the OBS UI thread; module unload also runs on the UI thread. The two cannot interleave on the same thread. Documented as an institutional-memory note; no edit.
- **Cleanup axis**: comments are current; the unload-order comment correctly explains why `g_dock` is not deleted here.

**Findings**: F-PM1 (stability, **KEEP**).

### manager.hpp

- **Surface**: declares `SharedEncoder` and `SlotManager`. Constitution Principle II (ownership) is encoded by the access patterns documented here.
- **Perf axis**: nothing to audit; declarations only.
- **Stability axis**: comments correctly document the lock-order story (`mtx_ → slot_mtx_ → stats_mtx_ → shared_mtx_` leaf). The `SharedEncoder` ownership contract — owner exclusively `SlotManager`, leased via refcount — is clear and matches Principle II.
- **Cleanup axis**: comments are up to date.

**Findings**: none.

### manager.cpp

- **Surface**: `SlotManager` implementation, frontend / save callbacks, shared-encoder registry, persistence (`save_to`/`load_from`).
- **Perf axis**:
  - **F-M1**: `stop_all()` holds `mtx_` for the entire iteration. Inside, `s->stop()` → `teardown()` → `slot_mtx_` → ... → `wait_for_output_stop` (slot.cpp), which can block up to 5 s per slot. With ≤10 slots, that's a worst-case ≤50 s hold on `mtx_`. Any concurrent reader of `mtx_` (e.g., the dock refresh polling `slot_count`/`slot_at`) blocks for the duration. `start_all()` already uses a snapshot pattern that releases `mtx_` before iterating. The asymmetry is avoidable. **Disposition (a) CLOSE**: refactor `stop_all` to mirror `start_all`'s snapshot pattern. No signature change.
  - Other paths — `slot_count`, `slot_at`, `any_running`, `add_slot`, `remove_slot`, `update_slot`, `register_all_hotkeys`, `unregister_all_hotkeys` — hold `mtx_` for short, bounded work. No findings.
- **Stability axis**:
  - **F-M2** (latent): `save_to` reads each slot's hotkey-related fields via `s->save_hotkey_bindings(d)`. Those fields (`hotkey_record_`, `hotkey_replay_`, `pending_hk_*`) are written by `SceneSlot::register_hotkeys` / `unregister_hotkeys` / `capture_hotkey_bindings` / `set_pending_hotkey_bindings`. None of those mutations are under `slot_mtx_`. `save_to` holds `SlotManager::mtx_` during the read, but the writers (`update_config` → `register_hotkeys`/`unregister_hotkeys`) run WITHOUT `mtx_` (`update_slot` releases `mtx_` before calling `s->update_config`). So `save_to` and `update_config` could in principle race on the same hotkey fields. **Disposition (b)**: both `save_cb` and editor-accept-driven `update_slot` are dispatched on the OBS UI thread; the two cannot run concurrently. Documented as a latent race that is not reachable on the supported OBS thread model. If a future OBS version moves save callbacks off the UI thread, this finding becomes (a).
  - Shared-encoder acquire / release: `shared_mtx_` is a strict leaf, never held with another plugin lock. Verified in code; consistent with constitution Principle III and the comment block above `acquire_shared_encoder`.
  - Shutdown order: `shutdown()` does `stop_all()` first, then `unregister_all_hotkeys()`, then clears `slots_`, then under leaf `shared_mtx_` logs any surviving shared context and clears it. Correct lifecycle order.
- **Cleanup axis**:
  - `slot_to_data` / `slot_from_data` are intentionally verbose to keep persistence explicit and back-compat clear. The `obs_data_has_user_value` distinction for default-true bools (`psycho_aq`, `cabac`, `mbtree`) and "do-not-set" sentinels (`b_frames`, `gpu_index`, `max_qp`, ...) is correct. Comments explain *why* — not removable as dead-code.

**Findings**: F-M1 (perf+cleanup, **CLOSE**), F-M2 (stability latent, **KEEP**).

### slot.hpp

- **Surface**: `SceneSlot::Config` (large but flat), `Stats`, the public lifecycle (`start`, `stop`, `is_running`, `save_replay`, `update_config`, hotkey lifecycle), and the persistence helpers for hotkey bindings.
- **Perf axis**: declarations only.
- **Stability axis**: comments correctly document the lock-order role of `slot_mtx_` and `stats_mtx_`. The `pending_hk_*` ownership-transfer semantics for `set_pending_hotkey_bindings` are precisely described.
- **Cleanup axis**: encoder-configuration fields are well-documented with their semantics (sentinel values, encoder family applicability). No stale comments.

**Findings**: none.

### slot.cpp

- **Surface**: rate-control helpers, encoder-settings helpers, `SharedEncoder::build` / `~SharedEncoder`, `SceneSlot` lifecycle (`start`, `stop`, `teardown_locked`, `setup_encoders`, `setup_outputs`), hotkeys, stop-signal handlers, `save_replay`, `stats`.
- **Perf axis**:
  - The pipeline already takes the main video info's non-resolution / non-fps fields (output_format, scale_type, gpu_conversion, colorspace, range) — fix from feature 003 D2/D3/D4. Verified at lines 286–290.
  - `SharedEncoder::build` is called once per group acquire — not a hot path.
  - `apply_family_presets` runs once per encoder create. Not a hot path.
  - `stats()` runs at 1 Hz per running slot (gated by the dock's QTimer per feature 004 F1). Per-call work is small (a couple of OBS API reads + a bitrate sample). No finding.
- **Stability axis**:
  - **F-S1 (cleanup)**: line 551 `running_.store(true)` is redundant. The CAS at line 428 (`running_.compare_exchange_strong(expected, true)`) already set the value to true. Every failure path between the CAS and line 551 explicitly resets it to false. Line 551 is unreachable as a "newly making the slot running" — it's the steady-state. **Disposition (a) CLOSE**: delete the line.
  - **F-S2 (stability latent)**: concurrent `stop()` during `start()`'s setup can leave the slot in an inconsistent state. Walkthrough (POST-F-S1):
    1. Thread A calls `start()` — CAS sets `running_=true`. Continues, takes `slot_mtx_`, builds outputs.
    2. Thread B calls `stop()` — `running_.exchange(false)` returns `true` (success). Calls `teardown()`, blocks on `slot_mtx_`.
    3. Thread A finishes start successfully. Returns, releases `slot_mtx_`. (With F-S1, no second `running_.store(true)` happens — A's last touch to `running_` was the top-of-function CAS.)
    4. Thread B unblocks, takes `slot_mtx_`, runs `teardown_locked()` — releases the outputs A just created.
    5. End state: `running_=false` (B wrote it via the exchange), all outputs are `nullptr`. `is_running()` returns false; the dock shows "off". This is **self-consistent**: the slot is not running and reports it.

    Before F-S1, step 3's redundant `running_.store(true)` overwrote B's `false` write, leaving `running_=true` with no outputs (the dock fake-showed "REC"). F-S1 fixes the SYMPTOM by removing that overwrite; the race WINDOW itself is unchanged.
    
    **Disposition (b)**: the race window remains because the CAS in `start()` and the `exchange()` in `stop()` operate on `running_` outside of `slot_mtx_`. In practice, observed callers do not overlap. Dock state-toggle and "Start All"/"Stop All" run on the UI thread sequentially. Hotkey callbacks fire on a libobs hotkey thread, but a user cannot physically hit the start hotkey and click the stop button at the literally same instant; even if they did, OBS serialises hotkey events. Output-stop signal handlers (which call `stop()`) fire on libobs worker threads but only after signal-connect, which happens AFTER outputs are created in `setup_outputs` — so a successful start() never has an outstanding signal that could fire during its own setup. Documented as a latent race that requires near-simultaneous cross-thread `stop()` during another thread's in-flight `start()` setup; not reachable on observed call paths. Post-F-S1 the symptom is benign (state machine is self-consistent).
  - **F-S3 (cleanup)**: `wait_for_output_stop` polls `obs_output_active` in 10 ms increments up to 5 s. A condition-variable + libobs stop signal would be tighter but materially more code. **Disposition (b)**: bounded, infrequent (only on teardown), no observable cost. Documented.
  - `SharedEncoder::~SharedEncoder` strictly observes encoder → view → scene order (FR-003-equivalent). Verified in code.
  - `teardown_locked` disconnects the output stop signal handlers BEFORE calling `obs_output_stop` (which can trigger the same handler). Correct — prevents handler reentry during half-destroyed slot state.
  - Output-stop signal handlers (`on_rec_output_stop`, `on_replay_output_stop`) test `code == OBS_OUTPUT_SUCCESS` and return early to avoid double-teardown on intentional stops. Correct.
- **Cleanup axis**:
  - `update_config` line 397 (`if (cfg_.id.empty()) cfg_.id = keep_id.empty() ? generate_slot_id() : keep_id`) is defensive — in practice the editor preserves `cfg_.id`. Kept as defensive code for programmatic callers (a future API). Not dead.
  - Logging strings are consistent and contain enough context to grep.

**Findings**: F-S1 (cleanup, **CLOSE**), F-S2 (stability latent, **KEEP**), F-S3 (cleanup, **KEEP**).

### ui-dock.hpp

- **Surface**: `MultiSceneRecordDock` widget interface.
- All axes: declarations only; current.

**Findings**: none.

### ui-dock.cpp

- **Surface**: dock construction, `refresh()`, `refresh_stats()`, action handlers, `on_state_clicked`, stats-toggle logic.
- **Perf axis**:
  - Feature 004 F1: stats QTimer gating on `mgr.any_running()` is in place (verified at lines 200–209). Correct.
  - Feature 004 F2: cell-widget reuse for the state-toggle button is in place (lines 161–177). Correct.
  - **F-UD1**: `refresh()` allocates new `QTableWidgetItem`s for every text column (`COL_NAME`, `COL_SCENE`, `COL_RES`, `COL_ENC`, `COL_FRAMES`, `COL_DROPPED`, `COL_KBPS`, `COL_REPLAY`) on every call. With 10 rows that's 80 allocations per `refresh()` — and `refresh()` runs on every state transition, plus every 1 Hz tick that detects a generation mismatch. The button cell reuse pattern (F2) didn't extend to text items. **Disposition (a) CLOSE**: mutate existing items via `table_->item(i, col)->setText(...)` when the row already exists; only allocate fresh `QTableWidgetItem` when the row was just added (i.e., when `table_->item(i, col)` returns `nullptr`). Mirrors the cell-widget-reuse pattern of F2.
  - **F-UD2** (cleanup): `mgr.slot_name_by_id(c.shared_encoder_slot_id)` runs once per shared-encoder row in `refresh()` and again in `refresh_stats()` — each call takes `mtx_`. **Disposition (b)**: `mtx_` is uncontended in the dock's path, the call is O(slots) for a small N (≤10 typical), and the work is a single string compare. Closing it would require snapshotting a name map first. Complexity outweighs gain. Documented.
- **Stability axis**:
  - `slot_at` returns a raw pointer; `refresh_stats` uses `mgr.generation()` to detect a stale rebuild. Correct.
  - The captured-by-value `i` in the per-row state-toggle button lambda is consistent with `setRowCount()`'s truncate-from-end semantics — rows above a deletion keep their indices, and `remove_slot` shifts `slots_` so that the slot now AT index `i` is the one the user clicks for. Verified via code walkthrough.
  - Stats refresh checks `gen != last_generation_` and `table_->rowCount() != n` — defensive against concurrent rebuild. Correct.
- **Cleanup axis**:
  - `on_stop_all` is a one-liner that doesn't validate paths (unlike `on_start_all`). That's intentional — stop has no precondition.
  - `apply_stats_visibility` hides only the stats columns. State + Replay stay visible. Consistent with the QTimer gating.

**Findings**: F-UD1 (perf+cleanup, **CLOSE**), F-UD2 (cleanup, **KEEP**).

### ui-slot-editor.hpp

- **Surface**: `SlotEditor` widget interface.
- All axes: declarations only; current.

**Findings**: none.

### ui-slot-editor.cpp

- **Surface**: SlotEditor constructor, combo population, rate-control / quality field updates, on_accept, encoder-specific visibility helpers.
- **Perf axis**:
  - **F-USE1**: `update_encoder_specific_ui()` invokes `populate_combo_from_encoder_property()` four times (preset / profile / tune / multipass) for the same encoder. Each call independently does `obs_get_encoder_properties(enc_id)` and `obs_properties_destroy(props)`. **Disposition (a) CLOSE**: hoist the `obs_get_encoder_properties` call to the top of `update_encoder_specific_ui`, pass `obs_properties_t*` into the helper, destroy once at the end. The helper's signature widens (an extra parameter), but it is a `static private` of `SlotEditor` declared in `ui-slot-editor.hpp` and used only inside `ui-slot-editor.cpp` — no other translation unit calls it. Per FR-001 / the Clarifications definition of "contract-affecting", this is NOT an exported-symbol change and does NOT trigger an upstream revisit.
  - Dialog open populates the scene combo, video encoder combo, audio encoder combo, rate-control combo, plus encoder-specific introspection. Each obs_get_encoder_properties call is amortised across user interaction; opens on user action only. Not a hot path beyond F-USE1.
- **Stability axis**:
  - **F-USE2** (theoretical): the dialog's `accept()` returns to `MultiSceneRecordDock::on_edit`, which then calls `mgr.update_slot(row, dlg.result())`. Between accept() returning and update_slot taking mtx_, another thread could theoretically rebuild slots_. **Disposition (b)**: `load_from` runs from `save_cb` which fires on the OBS UI thread; the editor accept is also UI-thread; the two cannot interleave. Documented.
  - `populate_video_encoder_combo` iterates slots with `mgr.slot_at(si)` — each takes `mtx_`. Same pattern as feature 004 F4 (KEEP).
- **Cleanup axis**:
  - `on_accept`'s visibility-gated reads (`if (encoder_preset_combo_->isVisible()) cfg_.encoder_preset = ...`) preserve last-set values on hidden combos. Documented intent.
  - The "[missing] scene" placeholder pattern in `populate_scene_combo` + `on_accept` is well-documented and is necessary for the case where the saved scene name was renamed in OBS.
  - Comments at `update_encoder_specific_ui` accurately enumerate which encoder family gets which advanced widget. Up to date.

**Findings**: F-USE1 (perf+cleanup, **CLOSE**), F-USE2 (stability theoretical, **KEEP**).

---

## Verification posture (carried forward from FR-014 / spec)

Numeric measurements (idle-CPU baseline, memory plateau, action latencies) live in [quickstart.md](./quickstart.md) as institutional memory for future regression detection. They are not strict pass/fail gates — the SC bars in spec.md are process-oriented (every finding has a disposition; every (a)-disposition is closed; final pass is empty). All four (a)-disposition findings above are intended to be closed in this feature's commits during `/speckit-implement`. The six (b)-disposition findings carry their documented rationale and are not closed.
