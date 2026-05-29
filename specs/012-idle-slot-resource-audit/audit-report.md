# Codebase Performance & Stability Audit Report

**Date**: 2026-05-29  
**Branch**: `012-idle-slot-resource-audit`  
**Scope**: All compiled source files (`src/`) and build configuration (`CMakeLists.txt`)  
**Type**: Audit only — no source code changes

---

## Table of Contents

1. [Summary](#summary)
2. [Files Audited](#files-audited)
3. [Stability / Safety Findings](#stability--safety-findings)
4. [Performance Findings](#performance-findings)
5. [Correctness Findings](#correctness-findings)
6. [Code Quality Findings](#code-quality-findings)

---

## Summary

| Category | Critical | High | Medium | Low | Total |
|----------|----------|------|--------|-----|-------|
| Stability/Safety | 1 | 3 | 4 | 1 | 9 |
| Performance | 0 | 1 | 3 | 3 | 7 |
| Correctness | 0 | 1 | 1 | 3 | 5 |
| Code Quality | 0 | 0 | 2 | 4 | 6 |
| **Total** | **1** | **5** | **10** | **11** | **27** |

---

## Files Audited

All source files were examined. Files with no findings are listed under "Clean files."

| File | Findings |
|------|----------|
| `CMakeLists.txt` | Q-001, Q-002 |
| `src/plugin-main.hpp` | — (clean) |
| `src/plugin-main.cpp` | S-001, Q-003 |
| `src/manager.hpp` | — (clean) |
| `src/manager.cpp` | S-003, P-001, P-005, C-002 |
| `src/slot.hpp` | — (clean) |
| `src/slot.cpp` | S-002, S-004, S-005, S-006, S-007, S-008, P-002, P-006, P-007, C-001, C-003, C-004, Q-006 |
| `src/ui-dock.hpp` | — (clean) |
| `src/ui-dock.cpp` | P-003, P-004 |
| `src/ui-slot-editor.hpp` | — (clean) |
| `src/ui-slot-editor.cpp` | C-005, Q-005 |

**Clean files** (examined, no findings): `src/plugin-main.hpp`, `src/manager.hpp`, `src/slot.hpp`, `src/ui-dock.hpp`, `src/ui-slot-editor.hpp`

---

## Stability / Safety Findings

### S-001: `g_dock` dangling pointer during Qt destruction cascade — Critical

**File**: `src/plugin-main.cpp`, lines 17–19, 45–56

**Issue**: `g_dock` is a raw global pointer to a Qt widget parented to OBS's main window. Qt may destroy the dock widget (as a child of the main window) before `obs_module_unload` runs. Between the time Qt destroys the widget and `obs_module_unload` sets `g_dock = nullptr`, any code calling `get_dock()` (e.g., signal callbacks in `slot.cpp`) receives a dangling pointer. The comment on line 51 acknowledges this risk but the only mitigation is nulling the pointer at the end of unload.

**Proposed fix**:

Connect to the dock widget's `QObject::destroyed` signal to null `g_dock` as soon as Qt destroys it:

```cpp
// In dock_create_cb, after creating g_dock (line 29):
g_dock = new MultiSceneRecordDock(main_window);
QObject::connect(g_dock, &QObject::destroyed, [](){ g_dock = nullptr; });
```

All callers already null-check `get_dock()`, so this change makes the null-check sufficient to prevent use-after-free.

**Rationale**: Eliminates a use-after-free crash window during OBS shutdown or scene-collection changes when Qt tears down children before the plugin's unload hook fires. Constitution Principle III (Thread Safety) — data race on a pointer is undefined behaviour.

---

### S-002: Hotkey and signal callbacks hold raw `SceneSlot*` with no lifetime guarantee — High

**File**: `src/slot.cpp`, lines 1242–1262, 1264–1296, 1302–1308, 1385–1392

**Issue**: `on_record_hotkey`, `on_save_hotkey`, `on_rec_output_stop`, `on_replay_output_stop`, and `on_replay_saved` all receive a raw `void* data` cast to `SceneSlot*`. If the slot is removed (UI thread) while one of these callbacks is in flight (hotkey thread or libobs worker thread), `self` becomes a dangling pointer. `obs_hotkey_unregister` is documented to wait for in-flight callbacks, but between dispatch and callback entry there is a window where the slot can be destroyed.

**Proposed fix**:

Switch `slots_` from `std::vector<std::unique_ptr<SceneSlot>>` to `std::vector<std::shared_ptr<SceneSlot>>` and use weak pointers in callbacks:

Step 1 — Change `slots_` type in `manager.hpp`:
```cpp
std::vector<std::shared_ptr<SceneSlot>> slots_;
```

Step 2 — Create a weak-ptr handle for callbacks:
```cpp
struct HotkeyHandle { std::weak_ptr<SceneSlot> wp; };
```

Step 3 — In `register_hotkeys`, allocate a handle and pass it as `void* data`:
```cpp
auto* h = new HotkeyHandle{shared_from_this()};
hotkey_record_ = obs_hotkey_register_output(
    hotkey_out_, rec_name.c_str(), rec_desc.c_str(),
    &SceneSlot::on_record_hotkey, h);
```

Step 4 — In the callback, lock the weak_ptr:
```cpp
void SceneSlot::on_record_hotkey(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed)
{
    if (!pressed) return;
    auto* h = static_cast<HotkeyHandle*>(data);
    auto sp = h->wp.lock();
    if (!sp) return;
    if (sp->is_running()) sp->stop();
    else                  sp->start();
    if (auto* dock = get_dock())
        QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);
}
```

Step 5 — Free the handle in `unregister_hotkeys`.

Apply the same pattern to signal handlers (`on_rec_output_stop`, etc.).

**Rationale**: OBS callbacks are dispatched from arbitrary threads. A removal on the UI thread and a callback dispatch can race. `weak_ptr` is the standard C++ solution for callbacks referencing objects with independent lifetimes.

---

### S-003: `stop_all`/`start_all` snapshot can hold stale pointers if `remove_slot` runs concurrently — Medium

**File**: `src/manager.cpp`, lines 124–138, 141–157

**Issue**: Both methods snapshot raw `SceneSlot*` pointers under `mtx_`, release `mtx_`, then iterate calling `s->start()` / `s->stop()`. If `remove_slot` runs on another thread between snapshot and iteration, it erases and destroys a slot whose raw pointer is still in the snapshot. The `stop()` call then operates on a freed `SceneSlot`.

In practice both are called from the UI thread, but the locking contract does not enforce this.

**Proposed fix**:

Option A (minimal): Assert the UI-thread contract:
```cpp
void SlotManager::start_all()
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());
    // ... existing code ...
}
```

Option B (robust): With the `shared_ptr` migration from S-002, snapshot `shared_ptr` copies instead of raw pointers. The snapshot keeps slots alive for iteration duration regardless of concurrent removes:
```cpp
std::vector<std::shared_ptr<SceneSlot>> snapshot;
{
    std::lock_guard<std::mutex> lk(mtx_);
    started_ = true;
    snapshot = slots_;  // copies shared_ptrs
}
for (auto& s : snapshot) s->start();
```

**Rationale**: Clarifying or enforcing the threading contract prevents a class of use-after-free bugs that are hard to reproduce and diagnose.

---

### S-004: Recording filename not sanitized for filesystem safety — Medium

**File**: `src/slot.cpp`, lines 269–291

**Issue**: `build_output_filename()` inserts `cfg.name` directly into the file path (line 285) without sanitization. A slot name containing path separators (`/`, `\`), colons (`:`), or other filesystem-illegal characters will cause the output file creation to fail or create files in unexpected directories. The replay filename uses `replay_util::sanitize_for_filename()`, but the recording filename does not.

**Proposed fix**:

Reuse the existing sanitization:

```cpp
// Line 285 — BEFORE:
path += cfg.name.empty() ? std::string("slot") : cfg.name;

// AFTER:
std::string safe_name = replay_util::sanitize_for_filename(cfg.name);
path += safe_name.empty() ? std::string("slot") : safe_name;
```

**Rationale**: A user who names a slot "Game/Replay" or "Stream: Main" gets recording failures or files in wrong directories. One-line fix reusing existing infrastructure. Constitution Principle VII requires recordings to work per configured parameters.

---

### S-005: `wait_for_output_stop` busy-waits holding `slot_mtx_` for up to 5 seconds — Medium

**File**: `src/slot.cpp`, lines 814–831 (called from `teardown_locked()` lines 869–870)

**Issue**: `wait_for_output_stop()` blocks for up to 5 seconds in a 10 ms poll loop. It runs inside `teardown_locked()`, which holds `slot_mtx_`. While this slot is blocked, any other thread calling `stats()`, `save_replay()`, or `update_config()` on the SAME slot will block on `slot_mtx_` for up to 5 seconds. For `stats()`, this means the dock's 1 Hz refresh stalls, freezing the entire dock UI.

**Proposed fix**:

Restructure `teardown()` to release `slot_mtx_` before the wait:

```cpp
void SceneSlot::teardown()
{
    obs_output_t* local_rec = nullptr;
    obs_output_t* local_replay = nullptr;
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        // Phase 1: disconnect signals, request stop, grab handles
        if (rec_out_) {
            signal_handler_disconnect(obs_output_get_signal_handler(rec_out_),
                                      "stop", &SceneSlot::on_rec_output_stop, this);
            obs_output_stop(rec_out_);
        }
        if (replay_out_) {
            signal_handler_disconnect(obs_output_get_signal_handler(replay_out_),
                                      "saved", &SceneSlot::on_replay_saved, this);
            signal_handler_disconnect(obs_output_get_signal_handler(replay_out_),
                                      "stop", &SceneSlot::on_replay_output_stop, this);
            obs_output_stop(replay_out_);
        }
        local_rec = rec_out_;
        local_replay = replay_out_;
        rec_out_ = nullptr;
        replay_out_ = nullptr;
    }
    // Phase 2: wait WITHOUT holding slot_mtx_
    wait_for_output_stop(local_rec);
    wait_for_output_stop(local_replay);
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        // Phase 3: release outputs, audio encoders, shared context
        if (local_rec) obs_output_release(local_rec);
        if (local_replay) obs_output_release(local_replay);
        for (auto* aenc : aencs_) if (aenc) obs_encoder_release(aenc);
        aencs_.clear();
        selected_tracks_.clear();
        if (venc_) {
            SlotManager::instance().release_shared_encoder(group_key_);
            venc_ = nullptr;
        }
        group_key_.clear();
        encoder_fallback_ = false;
        start_time_ns_.store(0, std::memory_order_release);
        resolved_max_size_mb_.store(0, std::memory_order_release);
        was_clamped_at_start_.store(false, std::memory_order_release);
        replay_seconds_at_start_.store(0, std::memory_order_release);
        assumed_kbps_at_start_.store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> slk(stats_mtx_);
        observed_kbps_ewma_ = 0.0;
    }
}
```

This also requires adapting `start()`'s in-lock failure paths to use the same multi-phase approach instead of calling `teardown_locked()` directly.

**Rationale**: Holding a mutex during a 5-second busy wait freezes all `slot_mtx_` consumers. The stats timer, save-replay hotkey, and config updates all contend on this lock. Constitution Product Quality Bar: "Plugin overhead in idle... MUST be negligible."

---

### S-006: `log_replay_saved` reads `cfg_.name` and `replay_out_` without holding `slot_mtx_` — Medium

**File**: `src/slot.cpp`, lines 1310–1383

**Issue**: `log_replay_saved()` runs on the OBS mux worker thread (via the `on_replay_saved` signal callback). It reads `cfg_.name` (lines 1313, 1341, 1345, 1354, 1367, 1372, 1379) and `replay_out_` (lines 1312, 1316) without holding `slot_mtx_`. While the atomics are safe, `cfg_.name` is a `std::string` and a concurrent `update_config()` on the UI thread could reallocate its buffer — a data race.

**Proposed fix**:

Snapshot the needed fields under `slot_mtx_` at function entry:

```cpp
void SceneSlot::log_replay_saved()
{
    std::string name;
    obs_output_t* replay = nullptr;
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        name = cfg_.name;
        replay = replay_out_;
    }
    // Use `name` and `replay` instead of cfg_.name / replay_out_ below
    if (!replay) {
        blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '<unknown>'", name.c_str());
        return;
    }
    // ... rest of function using `name` and `replay` ...
}
```

Lock order is safe: `log_replay_saved` takes `slot_mtx_` then `stats_mtx_` (line 1335), matching the declared order.

**Rationale**: Data race on `std::string` is undefined behaviour — constitution Principle III violation. The fix is minimal: one lock acquisition at function entry.

---

### S-007: Signal callbacks `on_rec_output_stop` / `on_replay_output_stop` call `stop()` which re-enters the signal handler system — Medium

**File**: `src/slot.cpp`, lines 1264–1296

**Issue**: When an external stop occurs (disk full, encoder failure), the signal handler calls `self->stop()` → `teardown()` → `signal_handler_disconnect` on the SAME signal handler currently dispatching. OBS's `signal.c` serializes dispatch and disconnect on the signal's internal mutex. If `signal_handler_disconnect` is called from within a dispatch of the same signal, it will attempt to acquire a mutex already held by the dispatch — a deadlock.

In practice, OBS may handle this case internally (the "stop" signal is one-shot), but relying on this is fragile.

**Proposed fix**:

Defer `stop()` to the UI thread:

```cpp
void SceneSlot::on_rec_output_stop(void* data, calldata_t* cd)
{
    auto* self = static_cast<SceneSlot*>(data);
    if (!self) return;
    long long code = calldata_int(cd, "code");
    if (code == OBS_OUTPUT_SUCCESS) return;

    blog(LOG_WARNING,
         "[multi-scene-rec] '%s': recording output stopped externally (code %lld)",
         self->config().name.c_str(), code);

    // Defer to UI thread to avoid re-entering signal handler system
    obs_queue_task(OBS_TASK_UI, [](void* d) {
        auto* s = static_cast<SceneSlot*>(d);
        s->stop();
        if (auto* dock = get_dock())
            dock->refresh();
    }, self, false);
}
```

Apply the same pattern to `on_replay_output_stop`.

**Rationale**: Calling `signal_handler_disconnect` from within a dispatch is a potential deadlock. Deferring the stop to the UI thread avoids re-entrant signal handler calls entirely.

---

### S-008: `available_physical_mb()` on macOS returns total memory, not available — Low

**File**: `src/slot.cpp`, lines 109–114

**Issue**: The macOS branch uses `sysctlbyname("hw.memsize", ...)` which returns **total** physical RAM, not **available** (free) physical RAM. The Windows branch correctly uses `msex.ullAvailPhys` (available). This means the replay buffer memory clamp on macOS effectively never fires, defeating the host-resource protection.

**Proposed fix**:

Replace with Mach VM statistics:

```cpp
#elif defined(__APPLE__)
#include <mach/mach.h>
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS)
        return 0;
    uint64_t avail_bytes = ((uint64_t)stats.free_count
                          + (uint64_t)stats.inactive_count) * vm_page_size;
    return avail_bytes / (1024 * 1024);
```

**Rationale**: Total-RAM-as-available means a macOS user under memory pressure could allocate a replay buffer that causes OBS to swap or be OOM-killed. The clamp exists to prevent this but is ineffective when the input is wrong.

---

### S-009: `start()` reads `cfg_` fields without `slot_mtx_` before the lock is acquired — Low

**File**: `src/slot.cpp`, lines 648–663

**Issue**: In `SceneSlot::start()`, `cfg_.shared_encoder_slot_id`, `cfg_.id`, and `cfg_.name` are read before `slot_mtx_` is acquired (lines 650–661). The comment acknowledges this is "Brief unlocked read of cfg_ identity fields only." However, `cfg_` is guarded by `slot_mtx_`, and `update_config()` writes to `cfg_` under `slot_mtx_` on the UI thread. If `update_config()` runs concurrently with a hotkey-triggered `start()`, the reads are a data race.

The race window is extremely narrow (the hotkey thread reads `cfg_` just before taking `slot_mtx_`), and `update_config()` calls `stop()` before modifying `cfg_`, which would make `start()`'s initial `compare_exchange_strong` fail. So in practice the race is unexploitable — but it is still undefined behaviour.

**Proposed fix**:

Take `slot_mtx_` briefly to snapshot identity fields, release before calling into `SlotManager`:

```cpp
std::string dep_id;
std::string my_id;
SceneSlot::Config my_cfg_copy;
{
    std::lock_guard<std::mutex> pre_lk(slot_mtx_);
    dep_id = cfg_.shared_encoder_slot_id;
    my_id = cfg_.id;
    if (dep_id.empty())
        my_cfg_copy = cfg_;
}
// ... resolve group_key using dep_id / my_id / my_cfg_copy ...
```

Then the existing `std::lock_guard<std::mutex> lk(slot_mtx_)` on line 684 guards the rest of `start()`. Lock is released and re-acquired (non-recursive mutex), which is the existing pattern.

**Rationale**: Formally undefined behaviour. The practical risk is mitigated by `update_config()`'s stop-before-write pattern, but fixing it makes the code correct by construction.

---

## Performance Findings

### P-001: `slot_count()` and `slot_at()` acquire `mtx_` independently — O(N) lock acquisitions per dock refresh — High

**File**: `src/manager.cpp`, lines 43–57; `src/ui-dock.cpp`, lines 140–224, 226–295

**Issue**: `refresh()` calls `mgr.slot_count()` (one lock acquisition), then `mgr.slot_at(i)` in a loop (N lock acquisitions). `refresh_stats()` does the same. For N slots, this is N+1 lock acquisitions per 1 Hz tick. Each acquisition involves a kernel transition on Windows (std::mutex maps to SRWLOCK).

Additionally, between `slot_count()` and `slot_at(i)`, another thread could modify `slots_` (e.g., a `load_from` on the save callback thread), making the count stale. The `generation()` check mitigates this but introduces yet another lock acquisition.

**Proposed fix**:

Add a `snapshot_slots()` method that returns all needed data under a single lock:

```cpp
// In manager.hpp:
struct SlotSnapshot {
    std::vector<SceneSlot*> slots;
    size_t generation;
};
SlotSnapshot snapshot_slots() const;

// In manager.cpp:
SlotManager::SlotSnapshot SlotManager::snapshot_slots() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    SlotSnapshot snap;
    snap.generation = generation_;
    snap.slots.reserve(slots_.size());
    for (auto& s : slots_) snap.slots.push_back(s.get());
    return snap;
}
```

`refresh()` and `refresh_stats()` call `snapshot_slots()` once and iterate the vector without further locking. This reduces lock acquisitions from O(N) to O(1) per tick.

**Rationale**: With 10 slots, eliminates 11 mutex lock/unlock pairs per second from the 1 Hz stats timer. Also eliminates the TOCTOU between count and individual accesses. The `start_all` and `stop_all` methods already use this snapshot pattern.

---

### P-002: `stats()` holds `slot_mtx_` for the full function body including OBS queries — Medium

**File**: `src/slot.cpp`, lines 1424–1469

**Issue**: `stats()` acquires `slot_mtx_` for the full function body, then additionally acquires `stats_mtx_`. The OBS output query functions (`obs_output_get_total_frames`, `obs_output_active`, etc.) are thread-safe in libobs and don't need external locking. Holding `slot_mtx_` during these queries means `stats()` contends with `start()`, `stop()`, `save_replay()`, and `update_config()`.

Combined with S-005 (5-second `slot_mtx_` hold during teardown), a `stats()` call during slot stop can block for seconds.

**Proposed fix**:

Reduce `slot_mtx_` scope to just copy the needed pointers/flags:

```cpp
SceneSlot::Stats SceneSlot::stats()
{
    Stats out;
    if (!running_.load()) return out;

    obs_output_t* primary = nullptr;
    obs_output_t* replay = nullptr;
    bool fallback = false;
    {
        std::lock_guard<std::mutex> lk(slot_mtx_);
        primary = rec_out_ ? rec_out_ : replay_out_;
        replay = replay_out_;
        fallback = encoder_fallback_;
    }
    if (!primary) return out;

    // OBS output queries are thread-safe — no lock needed
    out.active = obs_output_active(primary);
    out.replay_active = replay ? obs_output_active(replay) : false;
    out.total_frames = obs_output_get_total_frames(primary);
    out.dropped_frames = obs_output_get_frames_dropped(primary);
    out.total_bytes = obs_output_get_total_bytes(primary);
    out.encoder_fallback = fallback;

    // Bitrate sampling under stats_mtx_ only
    uint64_t now_ns = os_gettime_ns();
    std::lock_guard<std::mutex> slk(stats_mtx_);
    // ... (existing bitrate delta logic) ...
    return out;
}
```

The output pointers can't become invalid while `running_` is true — teardown sets `running_` to false before destroying outputs.

**Rationale**: Reduces `slot_mtx_` hold time in the stats hot path. Eliminates contention between 1 Hz polling and start/stop operations.

---

### P-003: `refresh_stats` calls `s->config()` twice per row — Low

**File**: `src/ui-dock.cpp`, lines 240–294

**Issue**: Within the `refresh_stats` loop, `s->config()` is called on line 251 (for `replay_only`) and again on line 289 (for `replay_enabled` / `replay_seconds`). `config()` returns a const reference, so this is safe, but the compiler cannot hoist the call because `s->stats()` on line 243 takes `slot_mtx_`.

**Proposed fix**:

Cache the config reference once at the top of the loop body:

```cpp
for (int i = 0; i < n; ++i) {
    SceneSlot* s = mgr.slot_at((size_t)i);
    if (!s) continue;
    const auto& c = s->config();
    SceneSlot::Stats st = s->stats();
    // ... use c instead of s->config() everywhere ...
}
```

**Rationale**: Minor optimization that also improves readability. The current pattern creates a redundant pointer dereference.

---

### P-004: `on_replay_max_size_inputs_changed` calls `available_physical_mb()` (syscall) on every keystroke — Medium

**File**: `src/ui-slot-editor.cpp`, lines 1093–1141

**Issue**: Every spinbox value change triggers `on_replay_max_size_inputs_changed()`, which calls `resolve_max_size_mb` → `available_physical_mb()` — a platform-specific syscall (`GlobalMemoryStatusEx` on Windows, `sysinfo` on Linux). The available RAM doesn't change meaningfully during a dialog's lifetime.

**Proposed fix**:

Cache the available-RAM value for the dialog lifetime:

```cpp
// In SlotEditor constructor:
cached_avail_mb_ = replay_buffer_util::available_physical_mb();

// In on_replay_max_size_inputs_changed, use the cached value
// (requires a resolve_max_size_mb overload accepting pre-fetched avail_mb).
```

Alternatively, debounce with `QTimer::singleShot(150, ...)`.

**Rationale**: Eliminates a per-keystroke syscall. The improvement is small but the fix is trivial.

---

### P-005: `config_by_slot_id` and `slot_name_by_id` do linear scans — Low

**File**: `src/manager.cpp`, lines 171–185, 295–303

**Issue**: Both functions iterate `slots_` comparing `s->config().id` against the target. With N slots, each is O(N) string comparisons. Called from `start()` (once per slot), `effective_rate_control()`, and dock refresh.

**Proposed fix**:

Add a `std::unordered_map<std::string, size_t>` index:

```cpp
// In manager.hpp private:
std::unordered_map<std::string, size_t> id_index_;

// Rebuild in load_from, add_slot, remove_slot.
// In config_by_slot_id:
auto it = id_index_.find(slot_id);
if (it == id_index_.end()) return false;
out = slots_[it->second]->config();
return true;
```

**Rationale**: O(1) hash lookup vs O(N) linear scan. The index maintenance cost is negligible (CRUD operations only).

---

### P-006: `popcount32` uses a manual bit-counting loop — Low

**File**: `src/slot.cpp`, lines 72–78

**Issue**: `replay_buffer_util::popcount32()` counts bits with a shift-and-add loop. All three target compilers support hardware popcount intrinsics that compile to a single instruction.

**Proposed fix**:

```cpp
static uint32_t popcount32(uint32_t v)
{
#if defined(_MSC_VER)
    return __popcnt(v);
#else
    return (uint32_t)__builtin_popcount(v);
#endif
}
```

**Rationale**: Single-instruction vs. 6-iteration loop. Called on estimated_kbps computations. Negligible in absolute terms but the intrinsic is simpler.

---

### P-007: `populate_rate_control_combo` and `update_rc_value_field` each fetch encoder properties independently — Medium

**File**: `src/ui-slot-editor.cpp`, lines 577–614, 620–674

**Issue**: `populate_rate_control_combo()` calls `obs_get_encoder_properties(enc_id.c_str())` on line 586. `update_rc_value_field()` calls it again on line 641. `update_encoder_specific_ui()` calls it a third time on line 976. Each call creates a new `obs_properties_t`, which involves enumerating all the encoder's properties. The `F-USE1` optimization consolidated calls within `update_encoder_specific_ui`, but these two functions were not included.

**Proposed fix**:

Hoist a single `obs_get_encoder_properties` call in `on_encoder_changed` and pass the `obs_properties_t*` to all three functions:

```cpp
void SlotEditor::on_encoder_changed()
{
    if (loading_) return;
    // ... (existing shared-encoder logic) ...
    if (!is_shared) {
        const std::string enc_id = venc_combo_->currentData().toString().toStdString();
        obs_properties_t* props = enc_id.empty() ? nullptr
            : obs_get_encoder_properties(enc_id.c_str());

        populate_rate_control_combo(props);     // add obs_properties_t* param
        update_rc_value_field(props);           // add obs_properties_t* param
        update_encoder_specific_ui(props);      // already refactored by F-USE1

        if (props) obs_properties_destroy(props);
    }
    update_shared_encoder_visibility();
}
```

Adjust function signatures to accept `obs_properties_t*` (with nullptr default for the constructor's initial call).

**Rationale**: Reduces three `obs_get_encoder_properties` calls to one per encoder change. For NVENC encoders with ~40 properties, this is measurable.

---

## Correctness Findings

### C-001: Recording filename can collide across slots sharing an output directory — High

**File**: `src/slot.cpp`, lines 269–291

**Issue**: `build_output_filename()` constructs the recording filename as `<name>_<timestamp>.<ext>`. Two slots with different names but the same output directory that start recording within the same second get different filenames (because names differ). However, if two slots have the SAME name (which is allowed — `name` is user-editable with no uniqueness constraint), they produce identical filenames and one overwrites the other.

The replay filename solved this in feature 007 by embedding a 6-hex-char ID suffix via `build_replay_format`. The recording filename was not updated to match.

**Proposed fix**:

Embed the same ID suffix in the recording filename:

```cpp
static std::string build_output_filename(const SceneSlot::Config& cfg)
{
    // ... (ts formatting unchanged) ...
    std::string safe_name = replay_util::sanitize_for_filename(cfg.name);
    if (safe_name.empty()) safe_name = "slot";

    std::string id6;
    if (cfg.id.size() >= 6)
        id6 = cfg.id.substr(cfg.id.size() - 6);
    else
        id6 = cfg.id;

    std::string path = cfg.path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    path += safe_name;
    path += '_';
    path += id6;
    path += '_';
    path += ts;
    path += '.';
    path += cfg.container.empty() ? std::string("mp4") : cfg.container;
    return path;
}
```

This also fixes S-004 (unsanitized name) as a side effect.

**Rationale**: Constitution Principle VII — a multi-scene recorder that silently overwrites recordings has failed at its primary job. The replay path already solved this; the recording path should match.

---

### C-002: `slot_from_data` does not validate `fps_num` / `fps_den` ranges — Low

**File**: `src/manager.cpp`, lines 409–410

**Issue**: Loaded `fps_num` and `fps_den` are only defaulted when 0. A manually corrupted save file with `fps_den = -1` wraps to `UINT32_MAX` via the `(uint32_t)` cast, producing near-zero FPS that could hang the video pipeline. Similarly, `fps_num = 999999999` would create an absurdly high framerate.

**Proposed fix**:

Add range clamping after loading (matching the editor's spinbox ranges):

```cpp
if (c.fps_den == 0 || c.fps_den > 1001) c.fps_den = 1;
if (c.fps_num == 0 || c.fps_num > 240000) c.fps_num = 60;
```

**Rationale**: Defensive validation of persisted data prevents a corrupted save file from creating a slot with pathological parameters.

---

### C-003: `generate_slot_id` can produce ambiguous IDs — Low

**File**: `src/slot.cpp`, lines 247–255

**Issue**: The format `"%llx%x"` concatenates hex timestamp and counter without a separator. Timestamp `0x1234` + counter `0x56` produces the same ID as timestamp `0x123456` + counter `0x0`. The probability is vanishingly small but the fix is trivial.

**Proposed fix**:

Add a separator:

```cpp
std::snprintf(buf, sizeof(buf), "%llx-%x", (unsigned long long)t, c);
```

**Rationale**: A collision would cause hotkey registration conflicts and shared-encoder group key collisions. The separator eliminates theoretical ambiguity at zero cost.

---

### C-004: `available_physical_mb()` Linux branch returns `freeram`, not "available" — Low

**File**: `src/slot.cpp`, lines 116–119

**Issue**: The Linux branch uses `sysinfo(&si)` and returns `si.freeram * si.mem_unit`. On modern Linux, `freeram` is truly free memory — it excludes the page cache and reclaimable slab memory. The kernel's "available" memory (`MemAvailable` in `/proc/meminfo`) is typically much larger. Using `freeram` means the clamp fires too aggressively on Linux.

**Proposed fix**:

Read `/proc/meminfo` for `MemAvailable` (available since Linux 3.14):

```cpp
#else
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long kb;
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                fclose(f);
                return (uint64_t)(kb / 1024);
            }
        }
        fclose(f);
    }
    // Fallback to sysinfo
    struct sysinfo si {};
    if (sysinfo(&si) != 0) return 0;
    return (uint64_t)si.freeram * si.mem_unit / (1024 * 1024);
#endif
```

**Rationale**: On a machine with 32 GB RAM and 28 GB used as page cache, `freeram` shows ~1 GB while `MemAvailable` shows ~29 GB. The overly aggressive clamp could decline valid replay buffers.

---

### C-005: `on_replay_max_size_inputs_changed` preview Config missing `replay_enabled` — Low

**File**: `src/ui-slot-editor.cpp`, lines 1093–1142

**Issue**: The preview `Config` on line 1095 is default-constructed with `replay_enabled = false`. Currently harmless (replay_enabled is not checked by `resolve_max_size_mb`), but if `resolve_max_size_mb` ever gates on it, the preview would produce incorrect results.

**Proposed fix**:

Set the field for correctness-by-construction:

```cpp
preview.replay_enabled = replay_check_->isChecked();
```

**Rationale**: The preview should mirror editor state as closely as possible.

---

## Code Quality Findings

### Q-001: `CMakeLists.txt` has duplicate `find_package` and `target_link_libraries` — Medium

**File**: `CMakeLists.txt`, lines 25–35 vs 52–63

**Issue**: Qt6 and `obs-frontend-api` are found and linked twice. The duplicate block at lines 51–63 has a comment "Force Qt + frontend-api linkage (template's ENABLE_QT block is missing)" but the conditional block IS present and functional with `ENABLE_QT` defaulting to ON.

**Proposed fix**:

Delete lines 51–63 entirely:

```cmake
# DELETE these lines:
# --- Force Qt + frontend-api linkage (template's ENABLE_QT block is missing) ---
find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Core OBS::obs-frontend-api)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
```

**Rationale**: Duplicate build configuration is confusing and adds ~200ms to CMake configure. Duplicate `target_link_libraries` produces duplicate link-line entries.

---

### Q-002: `plugin-main.hpp` not listed in `target_sources` — Low

**File**: `CMakeLists.txt`, lines 37–47

**Issue**: `target_sources` lists `plugin-main.cpp` but not `plugin-main.hpp`. Listing headers ensures IDE visibility and AUTOMOC coverage.

**Proposed fix**:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/plugin-main.hpp
    src/slot.cpp
    # ...
)
```

**Rationale**: IDE usability and completeness. No functional impact.

---

### Q-003: `plugin-main.cpp` hardcodes version "1.0.0" — Medium

**File**: `src/plugin-main.cpp`, line 36

**Issue**: The version logged at module load is hardcoded. The constitution says `buildspec.json` is the canonical version source. The log line will always show "1.0.0" regardless of actual version.

**Proposed fix**:

```cmake
# In CMakeLists.txt, after project():
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    PLUGIN_VERSION="${CMAKE_PROJECT_VERSION}")
```

```cpp
// In plugin-main.cpp, line 36:
blog(LOG_INFO, "[multi-scene-rec] loading v%s", PLUGIN_VERSION);
```

**Rationale**: Log lines with wrong version numbers waste time during debugging. `CMAKE_PROJECT_VERSION` comes from `buildspec.json` via the bootstrap script.

---

### Q-004: Magic numbers in `wait_for_output_stop` — Low

**File**: `src/slot.cpp`, lines 821–822

**Issue**: `max_iters = 500` and `os_sleep_ms(10)` encode a 5-second timeout as two magic numbers.

**Proposed fix**:

```cpp
static constexpr int kStopTimeoutMs = 5000;
static constexpr int kStopPollMs = 10;
const int max_iters = kStopTimeoutMs / kStopPollMs;
```

**Rationale**: Named constants are self-documenting and can be tuned without reading comments.

---

### Q-005: `fmt_bytes_rate` formatting creates visual jump between kbps and Mbps — Low

**File**: `src/ui-dock.cpp`, lines 39–43

**Issue**: Values at 999 kbps show as "999 kbps" (0 decimals), then jump to "1.00 Mbps" (2 decimals). The visual discontinuity is minor but jarring.

**Proposed fix**:

```cpp
static QString fmt_bytes_rate(double kbps)
{
    if (kbps <= 0.0) return "--";
    if (kbps >= 1000.0) return QString::number(kbps / 1000.0, 'f', 2) + " Mbps";
    if (kbps >= 100.0)  return QString::number(kbps, 'f', 0) + " kbps";
    return QString::number(kbps, 'f', 1) + " kbps";
}
```

**Rationale**: Minor UX polish.

---

### Q-006: Several `static` helper functions in `slot.cpp` could use anonymous namespaces — Low

**File**: `src/slot.cpp`, lines 247, 257, 269, 296, 329, 436

**Issue**: Functions like `generate_slot_id`, `fetch_scene_source`, `build_output_filename`, `set_quality_value`, `apply_family_presets`, `apply_encoder_settings` are `static` (C-style linkage). The project uses anonymous namespaces elsewhere (`ui-slot-editor.cpp` lines 28–69). Mixing styles is a code-quality concern.

**Proposed fix**:

Wrap in an anonymous namespace and remove `static`:

```cpp
namespace {
std::string generate_slot_id() { ... }
obs_source_t* fetch_scene_source(const std::string& name) { ... }
// ...
} // namespace
```

**Rationale**: Style consistency. `static` and anonymous namespace are functionally equivalent for functions.
