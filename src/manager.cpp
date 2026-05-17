#include "manager.hpp"

SlotManager& SlotManager::instance()
{
    static SlotManager m;
    return m;
}

void SlotManager::init()
{
    obs_frontend_add_event_callback(&SlotManager::frontend_event_cb, this);
    obs_frontend_add_save_callback(&SlotManager::save_cb, this);
}

void SlotManager::shutdown()
{
    obs_frontend_remove_event_callback(&SlotManager::frontend_event_cb, this);
    obs_frontend_remove_save_callback(&SlotManager::save_cb, this);
    stop_all();
    unregister_all_hotkeys();
    std::lock_guard<std::mutex> lk(mtx_);
    slots_.clear();
}

// ----------------------------------------------------------------------------
// CRUD
// ----------------------------------------------------------------------------

size_t SlotManager::slot_count() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return slots_.size();
}

SceneSlot* SlotManager::slot_at(size_t i) const
{
    // The returned pointer is only valid until the next slot mutation
    // (add/remove/load_from). load_from() can rebuild slots_ from a
    // different thread, so callers that cache this pointer across calls
    // MUST check generation() for consistency (see refresh_stats()).
    std::lock_guard<std::mutex> lk(mtx_);
    return i < slots_.size() ? slots_[i].get() : nullptr;
}

void SlotManager::add_slot(const SceneSlot::Config& cfg)
{
    std::lock_guard<std::mutex> lk(mtx_);
    slots_.emplace_back(std::make_unique<SceneSlot>(cfg));
    // User-initiated add: the frontend is up, so register hotkeys immediately.
    slots_.back()->register_hotkeys();
    if (started_) {
        const auto& sid = cfg.shared_encoder_slot_id;
        if (sid.empty()) {
            slots_.back()->start();
        } else {
            // Resolve the borrowed encoder under mtx_. If null (primary not
            // running) DO NOT call start() with null: start() would re-enter
            // find_encoder_by_slot_id_locked() and deadlock on mtx_.
            obs_encoder_t* venc = find_encoder_by_slot_id_unlocked(sid);
            if (venc) {
                slots_.back()->start(venc);
            } else {
                blog(LOG_ERROR,
                     "[multi-scene-rec] '%s': primary slot '%s' not running, dependent not started",
                     cfg.name.c_str(), sid.c_str());
            }
        }
    }
}

void SlotManager::remove_slot(size_t i)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (i >= slots_.size()) return;

    const std::string removed_id = slots_[i]->config().id;

    // Stop any dependent slots that borrow this slot's encoder.
    for (auto& s : slots_) {
        if (s->config().shared_encoder_slot_id == removed_id && s->is_running())
            s->stop();
    }

    slots_[i]->stop();
    slots_[i]->unregister_hotkeys();
    slots_.erase(slots_.begin() + i);
}

void SlotManager::update_slot(size_t i, const SceneSlot::Config& cfg)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (i >= slots_.size()) return;
    // Resolve the borrowed encoder while still under mtx_ so update_config()
    // (and the start() it triggers) never has to take the locked lookup
    // path, which would re-enter mtx_ on this thread (deadlock).
    obs_encoder_t* venc = nullptr;
    if (!cfg.shared_encoder_slot_id.empty())
        venc = find_encoder_by_slot_id_unlocked(cfg.shared_encoder_slot_id);
    slots_[i]->update_config(cfg, venc);
}

// ----------------------------------------------------------------------------
// start / stop / hotkeys for all
// ----------------------------------------------------------------------------

void SlotManager::start_all()
{
    std::lock_guard<std::mutex> lk(mtx_);
    started_ = true;

    // Phase 1: start primary slots (those that own their encoder).
    for (auto& s : slots_) {
        if (s->config().shared_encoder_slot_id.empty())
            s->start();
    }

    // Phase 2: start dependent slots (those that borrow an encoder).
    for (auto& s : slots_) {
        const auto& sid = s->config().shared_encoder_slot_id;
        if (!sid.empty()) {
            obs_encoder_t* venc = find_encoder_by_slot_id_unlocked(sid);
            if (venc) {
                s->start(venc);
            } else {
                // Never pass null for a dependent from this locked context:
                // start() would re-enter mtx_ via the locked lookup.
                blog(LOG_ERROR,
                     "[multi-scene-rec] '%s': primary slot '%s' not running, dependent not started",
                     s->config().name.c_str(), sid.c_str());
            }
        }
    }
}

void SlotManager::stop_all()
{
    std::lock_guard<std::mutex> lk(mtx_);
    started_ = false;
    for (auto& s : slots_) s->stop();
}

void SlotManager::stop_dependents_of(const std::string& primary_slot_id)
{
    // mtx_ guards slots_. Each s->stop() -> teardown() takes slot_mtx_,
    // preserving the mtx_ -> slot_mtx_ order.
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) {
        if (s->config().shared_encoder_slot_id == primary_slot_id &&
            s->is_running())
            s->stop();
    }
}

void SlotManager::register_all_hotkeys()
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) s->register_hotkeys();
}

void SlotManager::unregister_all_hotkeys()
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) s->unregister_hotkeys();
}

obs_encoder_t* SlotManager::find_encoder_by_slot_id_unlocked(const std::string& slot_id) const
{
    for (auto& s : slots_) {
        if (s->config().id == slot_id && s->is_running())
            return s->video_encoder();
    }
    return nullptr;
}

obs_encoder_t* SlotManager::find_encoder_by_slot_id_locked(const std::string& slot_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return find_encoder_by_slot_id_unlocked(slot_id);
}

std::string SlotManager::slot_name_by_id(const std::string& slot_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) {
        if (s->config().id == slot_id)
            return s->config().name;
    }
    return {};
}

size_t SlotManager::generation() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return generation_;
}

// ----------------------------------------------------------------------------
// persistence
// ----------------------------------------------------------------------------

static obs_data_t* slot_to_data(const SceneSlot::Config& c)
{
    obs_data_t* d = obs_data_create();
    obs_data_set_string(d, "id",         c.id.c_str());
    obs_data_set_string(d, "name",       c.name.c_str());
    obs_data_set_string(d, "scene_name", c.scene_name.c_str());
    obs_data_set_int(d, "width",  c.width);
    obs_data_set_int(d, "height", c.height);
    obs_data_set_int(d, "fps_num", c.fps_num);
    obs_data_set_int(d, "fps_den", c.fps_den);
    obs_data_set_string(d, "path",      c.path.c_str());
    obs_data_set_string(d, "container", c.container.c_str());
    obs_data_set_string(d, "video_encoder_id", c.video_encoder_id.c_str());
    obs_data_set_string(d, "shared_encoder_slot_id", c.shared_encoder_slot_id.c_str());
    obs_data_set_string(d, "audio_encoder_id", c.audio_encoder_id.c_str());
    obs_data_set_string(d, "rate_control", c.rate_control.c_str());
    obs_data_set_int(d, "rc_value", c.rc_value);
    obs_data_set_int(d, "audio_bitrate", c.audio_bitrate);
    obs_data_set_int(d, "audio_tracks", c.audio_tracks);
    obs_data_set_bool(d, "replay_enabled", c.replay_enabled);
    obs_data_set_bool(d, "replay_only", c.replay_only);
    obs_data_set_int(d, "replay_seconds",  c.replay_seconds);

    // New encoder settings fields
    obs_data_set_int   (d, "keyframe_interval_sec",   c.keyframe_interval_sec);
    obs_data_set_string(d, "encoder_preset",           c.encoder_preset.c_str());
    obs_data_set_string(d, "encoder_profile",          c.encoder_profile.c_str());
    obs_data_set_string(d, "encoder_tune",             c.encoder_tune.c_str());
    obs_data_set_string(d, "multipass",                c.multipass.c_str());
    obs_data_set_bool  (d, "lookahead",                c.lookahead);
    obs_data_set_bool  (d, "psycho_aq",                c.psycho_aq);
    obs_data_set_int   (d, "b_frames",                 c.b_frames);
    obs_data_set_int   (d, "gpu_index",                c.gpu_index);
    obs_data_set_bool  (d, "advanced_settings",        c.advanced_settings);
    obs_data_set_int   (d, "max_qp",                   c.max_qp);
    obs_data_set_int   (d, "min_qp",                   c.min_qp);
    obs_data_set_bool  (d, "cabac",                    c.cabac);
    obs_data_set_string(d, "x264opts",                 c.x264opts.c_str());
    obs_data_set_bool  (d, "mbtree",                   c.mbtree);
    obs_data_set_int   (d, "aq_mode",                  c.aq_mode);
    obs_data_set_bool  (d, "nvenc_repeat_headers",     c.nvenc_repeat_headers);
    obs_data_set_bool  (d, "nvenc_force_idr",          c.nvenc_force_idr);
    obs_data_set_bool  (d, "nvenc_dyn_bitrate",        c.nvenc_dyn_bitrate);
    obs_data_set_bool  (d, "amf_enforce_hrd",          c.amf_enforce_hrd);
    obs_data_set_bool  (d, "amf_vbaq",                 c.amf_vbaq);
    obs_data_set_bool  (d, "amf_pre_analysis",         c.amf_pre_analysis);
    obs_data_set_bool  (d, "amf_enable_throughput",    c.amf_enable_throughput);
    obs_data_set_int   (d, "qsv_async_depth",          c.qsv_async_depth);
    obs_data_set_string(d, "qsv_latency",              c.qsv_latency.c_str());
    obs_data_set_bool  (d, "vt_realtime",              c.vt_realtime);
    obs_data_set_int   (d, "vt_frames_before_start",   c.vt_frames_before_start);
    obs_data_set_string(d, "hevc_tier",                c.hevc_tier.c_str());
    obs_data_set_int   (d, "av1_tile_cols",            c.av1_tile_cols);
    obs_data_set_int   (d, "av1_tile_rows",            c.av1_tile_rows);
    return d;
}

static SceneSlot::Config slot_from_data(obs_data_t* d)
{
    SceneSlot::Config c;
    c.id              = obs_data_get_string(d, "id");
    c.name            = obs_data_get_string(d, "name");
    c.scene_name      = obs_data_get_string(d, "scene_name");
    c.width           = (uint32_t)obs_data_get_int(d, "width");
    c.height          = (uint32_t)obs_data_get_int(d, "height");
    c.fps_num         = (uint32_t)obs_data_get_int(d, "fps_num");
    c.fps_den         = (uint32_t)obs_data_get_int(d, "fps_den");
    c.path            = obs_data_get_string(d, "path");
    c.container       = obs_data_get_string(d, "container");
    c.video_encoder_id = obs_data_get_string(d, "video_encoder_id");
    c.shared_encoder_slot_id = obs_data_get_string(d, "shared_encoder_slot_id");
    c.audio_encoder_id = obs_data_get_string(d, "audio_encoder_id");
    c.rate_control    = obs_data_get_string(d, "rate_control");
    c.rc_value        = (uint32_t)obs_data_get_int(d, "rc_value");
    c.audio_bitrate   = (uint32_t)obs_data_get_int(d, "audio_bitrate");
    c.audio_tracks    = (uint32_t)obs_data_get_int(d, "audio_tracks");
    c.replay_enabled  = obs_data_get_bool(d, "replay_enabled");
    c.replay_only     = obs_data_get_bool(d, "replay_only");
    c.replay_seconds  = (uint32_t)obs_data_get_int(d, "replay_seconds");

    // Defaults + back-compat for older saved configs.
    if (c.video_encoder_id.empty()) c.video_encoder_id = "obs_x264";
    if (c.audio_encoder_id.empty()) c.audio_encoder_id = "ffmpeg_aac";
    if (c.rate_control.empty())     c.rate_control = "CBR";
    if (c.rc_value == 0) {
        // Older saves stored "video_bitrate" instead of rate_control/rc_value.
        long long old_br = obs_data_get_int(d, "video_bitrate");
        c.rc_value = old_br > 0 ? (uint32_t)old_br : 6000;
    }
    if (c.audio_tracks == 0)  c.audio_tracks = 0x01;
    if (c.width == 0)         c.width = 1920;
    if (c.height == 0)        c.height = 1080;
    if (c.fps_num == 0)       c.fps_num = 60;
    if (c.fps_den == 0)       c.fps_den = 1;
    if (c.container.empty())  c.container = "mp4";
    if (c.audio_bitrate == 0) c.audio_bitrate = 160;
    if (c.replay_seconds == 0) c.replay_seconds = 30;

    // ---- New encoder settings (absent in old saves) ----

    // keyframe_interval_sec: 0 if absent → use former hardcoded default 2.
    c.keyframe_interval_sec = (uint32_t)obs_data_get_int(d, "keyframe_interval_sec");
    if (c.keyframe_interval_sec == 0) c.keyframe_interval_sec = 2;

    // String fields: obs_data_get_string returns "" when key is absent.
    // Empty string → apply_family_presets uses former hardcoded default. Correct.
    c.encoder_preset  = obs_data_get_string(d, "encoder_preset");
    c.encoder_profile = obs_data_get_string(d, "encoder_profile");
    c.encoder_tune    = obs_data_get_string(d, "encoder_tune");
    c.multipass       = obs_data_get_string(d, "multipass");
    c.x264opts        = obs_data_get_string(d, "x264opts");
    c.qsv_latency     = obs_data_get_string(d, "qsv_latency");
    c.hevc_tier       = obs_data_get_string(d, "hevc_tier");

    // lookahead: former hardcoded default was false; obs_data missing-key → false. OK.
    c.lookahead         = obs_data_get_bool(d, "lookahead");
    c.advanced_settings = obs_data_get_bool(d, "advanced_settings");
    c.nvenc_repeat_headers  = obs_data_get_bool(d, "nvenc_repeat_headers");
    c.nvenc_force_idr       = obs_data_get_bool(d, "nvenc_force_idr");
    c.nvenc_dyn_bitrate     = obs_data_get_bool(d, "nvenc_dyn_bitrate");
    c.amf_enforce_hrd       = obs_data_get_bool(d, "amf_enforce_hrd");
    c.amf_vbaq              = obs_data_get_bool(d, "amf_vbaq");
    c.amf_pre_analysis      = obs_data_get_bool(d, "amf_pre_analysis");
    c.amf_enable_throughput = obs_data_get_bool(d, "amf_enable_throughput");
    c.vt_realtime           = obs_data_get_bool(d, "vt_realtime");

    // psycho_aq: former NVENC hardcoded default was TRUE.
    // obs_data missing-key → false → would change behavior for existing NVENC users.
    // Use obs_data_has_user_value to detect absent key and default to true.
    c.psycho_aq = obs_data_has_user_value(d, "psycho_aq")
                      ? obs_data_get_bool(d, "psycho_aq")
                      : true;

    // cabac: x264 default is ON; struct default is true.
    // Absent key → false from obs_data, but advanced_settings=false for old saves
    // so this value is never applied anyway. Use has_user_value for correctness.
    c.cabac = obs_data_has_user_value(d, "cabac")
                  ? obs_data_get_bool(d, "cabac")
                  : true;

    // mbtree: same reasoning as cabac.
    c.mbtree = obs_data_has_user_value(d, "mbtree")
                   ? obs_data_get_bool(d, "mbtree")
                   : true;

    // Int fields where -1 = "do not set" but 0 is a valid value.
    // obs_data_get_int returns 0 for absent keys. Use has_user_value to distinguish.
    c.b_frames = obs_data_has_user_value(d, "b_frames")
                     ? (int)obs_data_get_int(d, "b_frames") : -1;
    c.gpu_index = obs_data_has_user_value(d, "gpu_index")
                      ? (int)obs_data_get_int(d, "gpu_index") : -1;
    c.max_qp = obs_data_has_user_value(d, "max_qp")
                   ? (int)obs_data_get_int(d, "max_qp") : -1;
    c.min_qp = obs_data_has_user_value(d, "min_qp")
                   ? (int)obs_data_get_int(d, "min_qp") : -1;
    c.aq_mode = obs_data_has_user_value(d, "aq_mode")
                    ? (int)obs_data_get_int(d, "aq_mode") : -1;
    c.qsv_async_depth = obs_data_has_user_value(d, "qsv_async_depth")
                            ? (int)obs_data_get_int(d, "qsv_async_depth") : -1;
    c.vt_frames_before_start = obs_data_has_user_value(d, "vt_frames_before_start")
                                   ? (int)obs_data_get_int(d, "vt_frames_before_start") : -1;
    c.av1_tile_cols = obs_data_has_user_value(d, "av1_tile_cols")
                          ? (int)obs_data_get_int(d, "av1_tile_cols") : -1;
    c.av1_tile_rows = obs_data_has_user_value(d, "av1_tile_rows")
                          ? (int)obs_data_get_int(d, "av1_tile_rows") : -1;

    // c.id left empty if absent -> SceneSlot ctor generates a fresh one.
    return c;
}

void SlotManager::save_to(obs_data_t* save_data)
{
    obs_data_t* root = obs_data_create();
    obs_data_array_t* arr = obs_data_array_create();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& s : slots_) {
            obs_data_t* d = slot_to_data(s->config());
            obs_data_array_push_back(arr, d);
            obs_data_release(d);
        }
    }
    obs_data_set_array(root, "slots", arr);
    obs_data_array_release(arr);
    obs_data_set_obj(save_data, "multi_scene_record", root);
    obs_data_release(root);
}

void SlotManager::load_from(obs_data_t* save_data)
{
    obs_data_t* root = obs_data_get_obj(save_data, "multi_scene_record");
    if (!root) return;
    obs_data_array_t* arr = obs_data_get_array(root, "slots");
    if (!arr) { obs_data_release(root); return; }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        slots_.clear();
        size_t n = obs_data_array_count(arr);
        for (size_t i = 0; i < n; ++i) {
            obs_data_t* d = obs_data_array_item(arr, i);
            SceneSlot::Config c = slot_from_data(d);
            slots_.emplace_back(std::make_unique<SceneSlot>(c));
            obs_data_release(d);
        }
        // slots_ was rebuilt: invalidate any cached raw SceneSlot* held by
        // the UI (see SlotManager::generation() / refresh_stats()).
        ++generation_;
    }
    obs_data_array_release(arr);
    obs_data_release(root);
    // Hotkeys are registered on FINISHED_LOADING / SCENE_COLLECTION_CHANGED,
    // once the frontend hotkey system is guaranteed to be available.
}

// ----------------------------------------------------------------------------
// frontend callbacks
// ----------------------------------------------------------------------------

void SlotManager::frontend_event_cb(enum obs_frontend_event event, void* ptr)
{
    auto* self = static_cast<SlotManager*>(ptr);
    if (!self) return;

    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        // Slots exist (loaded from save data); register their hotkeys now.
        self->register_all_hotkeys();
        break;

    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
    case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
        // Slots are about to be torn down / reloaded.
        self->stop_all();
        self->unregister_all_hotkeys();
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        self->stop_all();
        self->unregister_all_hotkeys();
        break;

    default:
        break;
    }
}

void SlotManager::save_cb(obs_data_t* save_data, bool saving, void* ptr)
{
    auto* self = static_cast<SlotManager*>(ptr);
    if (!self) return;
    if (saving) self->save_to(save_data);
    else        self->load_from(save_data);
}
