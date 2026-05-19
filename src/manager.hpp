#pragma once

#include "slot.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include <obs.h>
#include <obs-frontend-api.h>

// Standalone, refcounted shared video-encoder context. Owned exclusively by
// SlotManager and decoupled from any slot's start/stop: it is built on the
// first consumer's acquire and destroyed when the last consumer releases.
// Owner and sharing slots are symmetric consumers — stopping one never tears
// down another's pipeline, and the owner is not required to be running for a
// sharing slot to run.
//
// The whole scene/view/video/encoder pipeline is parameterized ENTIRELY by
// the group-key slot's Config (scene_name, width/height/fps, encoder id and
// settings). Settings are fixed at creation: a slot joining an existing
// context uses that context's (possibly older) settings until use_count
// returns to 0 and the context is rebuilt.
struct SharedEncoder {
    // Sole owner of all four handles below.
    obs_source_t*  scene_src_        = nullptr;
    obs_view_t*    view_             = nullptr;
    video_t*       video_            = nullptr;
    obs_encoder_t* venc_             = nullptr;
    // Was per-slot; now per shared context. Mirrors the old setup_encoders()
    // x264/CBR fallback flag so the "[CBR fallback]" UI indicator works for
    // the owner and every sharer.
    bool           encoder_fallback_ = false;
    // Number of currently-running consumer slots in this group. Guarded by
    // SlotManager::shared_mtx_.
    int            use_count_        = 0;

    SharedEncoder() = default;
    // Tears down in the mandatory order: encoder -> view -> scene.
    ~SharedEncoder();
    SharedEncoder(const SharedEncoder&) = delete;
    SharedEncoder& operator=(const SharedEncoder&) = delete;

    // Builds scene source (+inc_showing), per-group view/video at the owner's
    // resolution/fps, and the video encoder (with the same x264/CBR fallback
    // path as the former owner branch of setup_encoders()), then binds the
    // encoder to this group's video_t. Returns false and leaves *this safe to
    // destroy on failure.
    bool build(const SceneSlot::Config& owner_cfg);
};

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

    std::string slot_name_by_id(const std::string& slot_id) const;

    // Copies the Config of the slot whose id == slot_id into `out`. Takes
    // mtx_ briefly and releases it before returning. Returns false if no
    // such slot exists. Used by SceneSlot::start() to resolve a sharing
    // slot's owner Config before acquiring the shared encoder.
    bool config_by_slot_id(const std::string& slot_id,
                           SceneSlot::Config& out) const;

    // Shared-encoder registry. Both operate under shared_mtx_ ONLY (strict
    // leaf lock — see LOCKING in manager.cpp). They must never take mtx_ or
    // slot_mtx_, hence acquire takes the owner Config by value (resolved by
    // the caller beforehand).
    //
    // acquire_shared_encoder: if no context exists for group_key, builds one
    // from owner_cfg; then hands the caller a strong encoder ref and bumps
    // use_count. Returns nullptr on build failure.
    SharedEncoder* acquire_shared_encoder(const std::string& group_key,
                                          const SceneSlot::Config& owner_cfg);
    // release_shared_encoder: releases the caller's encoder ref and decrements
    // use_count; when use_count hits 0 destroys the context in the mandatory
    // encoder -> view -> scene order and erases it from the registry.
    void release_shared_encoder(const std::string& group_key);

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

    // Shared-encoder registry, keyed by encoder-group key (the group-key
    // slot's id). Guarded by its OWN dedicated leaf mutex so acquire/release
    // can run while mtx_ and/or slot_mtx_ are held without inverting the
    // global order mtx_ -> slot_mtx_ -> shared_mtx_.
    std::map<std::string, std::unique_ptr<SharedEncoder>> shared_;
    mutable std::mutex shared_mtx_;
};
