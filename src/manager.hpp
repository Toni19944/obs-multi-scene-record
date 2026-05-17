#pragma once

#include "slot.hpp"

#include <memory>
#include <vector>
#include <mutex>

#include <obs.h>
#include <obs-frontend-api.h>

class SlotManager {
public:
    static SlotManager& instance();

    void init();
    void shutdown();

    // CRUD
    size_t slot_count() const;
    SceneSlot* slot_at(size_t i) const;
    void add_slot(const SceneSlot::Config& cfg);
    void remove_slot(size_t i);
    void update_slot(size_t i, const SceneSlot::Config& cfg);

    // Start / stop all
    void start_all();
    void stop_all();

    // Stop every running slot that borrows the given primary's encoder.
    void stop_dependents_of(const std::string& primary_slot_id);

    // Encoder sharing lookup
    obs_encoder_t* find_encoder_by_slot_id_unlocked(const std::string& slot_id) const;
    obs_encoder_t* find_encoder_by_slot_id_locked(const std::string& slot_id) const;
    std::string slot_name_by_id(const std::string& slot_id) const;

    // Hotkey lifecycle for every slot. Each locks internally.
    void register_all_hotkeys();
    void unregister_all_hotkeys();

    // Persistence (via obs_frontend save callback)
    void save_to(obs_data_t* save_data);
    void load_from(obs_data_t* save_data);

    // Monotonic counter bumped whenever slots_ is rebuilt (load_from). UI
    // uses it to detect that raw SceneSlot* from slot_at() may be stale.
    size_t generation() const;

private:
    SlotManager() = default;
    ~SlotManager() = default;
    SlotManager(const SlotManager&) = delete;
    SlotManager& operator=(const SlotManager&) = delete;

    static void frontend_event_cb(enum obs_frontend_event event, void* ptr);
    static void save_cb(obs_data_t* save_data, bool saving, void* ptr);

    mutable std::mutex mtx_;
    std::vector<std::unique_ptr<SceneSlot>> slots_;
    bool started_ = false;
    // Bumped under mtx_ each time slots_ is rebuilt by load_from().
    size_t generation_ = 0;
};
