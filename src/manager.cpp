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

    // After stop_all() every consumer released, so the registry must already
    // be empty. Defensively clear it AFTER stop_all() and flag any leak:
    // erasing runs ~SharedEncoder for any surviving context (encoder->view->
    // scene order) so nothing leaks across module unload / OBS exit.
    // shared_mtx_ is a strict leaf; taking it while holding mtx_ keeps the
    // global order (mtx_ -> ... -> shared_mtx_) and never inverts it.
    std::lock_guard<std::mutex> slk(shared_mtx_);
    for (auto& kv : shared_) {
        blog(LOG_ERROR,
             "[multi-scene-rec] leaked shared encoder context for group '%s' at shutdown",
             kv.first.c_str());
    }
    shared_.clear();
}

// ----------------------------------------------------------------------------
// CRUD
// ----------------------------------------------------------------------------

void SlotManager::rebuild_id_index()
{
    id_index_.clear();
    for (size_t i = 0; i < slots_.size(); ++i)
        id_index_[slots_[i]->config().id] = i;
}

SlotManager::SlotSnapshot SlotManager::snapshot_slots() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return {slots_, generation_};  // copies shared_ptrs under mtx_
}

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

bool SlotManager::any_running() const
{
    // SceneSlot::is_running() is a lock-free atomic load on running_, so the
    // scan never nests under slot_mtx_. Returns false for an empty slots_.
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& s : slots_) {
        if (s->is_running()) return true;
    }
    return false;
}

void SlotManager::add_slot(const SceneSlot::Config& cfg)
{
    SceneSlot* added = nullptr;
    bool start_it = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        slots_.emplace_back(std::make_shared<SceneSlot>(cfg));
        slots_.back()->register_hotkeys();
        added    = slots_.back().get();
        start_it = started_;
        rebuild_id_index();
    }
    // start() resolves its group key and owner Config under mtx_ itself
    // (briefly) before acquiring the shared encoder, so it must be called
    // WITHOUT holding mtx_ (std::mutex is non-recursive). No pre-resolution:
    // a sharing slot whose owner is not running still starts — the shared
    // context is built from the owner's persisted Config.
    if (start_it && added) added->start();
}

void SlotManager::remove_slot(size_t i)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (i >= slots_.size()) return;

    // Stopping this slot no longer cascades to other slots: a sharing slot
    // keeps its own reference to the shared encoder context, which survives
    // until its own last consumer releases. stop() -> teardown_locked() ->
    // release_shared_encoder() takes slot_mtx_ then shared_mtx_ (leaf),
    // preserving mtx_ -> slot_mtx_ -> shared_mtx_.
    slots_[i]->stop();
    slots_[i]->unregister_hotkeys();
    slots_.erase(slots_.begin() + i);
    rebuild_id_index();
}

void SlotManager::update_slot(size_t i, const SceneSlot::Config& cfg)
{
    SceneSlot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (i >= slots_.size()) return;
        s = slots_[i].get();
    }
    // update_config() may stop() then start(); start() resolves its group
    // key / owner Config under mtx_ internally, so update_config must run
    // WITHOUT holding mtx_ (non-recursive). The acquire path replaces the
    // former pre-resolved encoder argument entirely.
    s->update_config(cfg);
}

// ----------------------------------------------------------------------------
// start / stop / hotkeys for all
// ----------------------------------------------------------------------------

void SlotManager::start_all()
{
    std::vector<std::shared_ptr<SceneSlot>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        started_ = true;
        snapshot = slots_;
    }
    for (auto& s : snapshot) s->start();
}

void SlotManager::stop_all()
{
    std::vector<std::shared_ptr<SceneSlot>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        started_ = false;
        snapshot = slots_;
    }
    for (auto& s : snapshot) s->stop();
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

bool SlotManager::config_by_slot_id(const std::string& slot_id,
                                    SceneSlot::Config& out) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = id_index_.find(slot_id);
    if (it == id_index_.end() || it->second >= slots_.size())
        return false;
    out = slots_[it->second]->config();
    return true;
}

EffectiveRC SlotManager::effective_rate_control(const SceneSlot::Config& c) const
{
    // (a) Owner slot: pass through its own fields.
    if (c.shared_encoder_slot_id.empty())
        return {c.rate_control, c.rc_value, false, ""};

    // (b) Consumer: resolve owner Config via existing path (takes mtx_ briefly
    //     and releases before we touch shared_mtx_). On lookup failure (e.g.
    //     orphan consumer: owner deleted while consumer survives) return safe
    //     last-resort values matching slot_from_data's CBR/6000 defaults. The
    //     empty owner_slot_name signals the orphan case to the editor for the
    //     "(inherited — owner missing)" label shape.
    SceneSlot::Config owner;
    if (!config_by_slot_id(c.shared_encoder_slot_id, owner))
        return {"CBR", 6000, false, ""};

    EffectiveRC eff{owner.rate_control, owner.rc_value, false, owner.name};

    // (c) Overlay fallback ONLY when a SharedEncoder row exists for this
    //     group and was built under the obs_x264/CBR fallback path. Takes
    //     leaf shared_mtx_ briefly, never nested with mtx_ above.
    {
        std::lock_guard<std::mutex> slk(shared_mtx_);
        auto it = shared_.find(c.shared_encoder_slot_id);
        if (it != shared_.end() && it->second && it->second->encoder_fallback_) {
            eff.mode = "CBR";
            eff.value = rc_util::is_bitrate_based(owner.rate_control)
                            ? owner.rc_value
                            : 6000;
            eff.fallback = true;
        }
    }
    return eff;
}

// ----------------------------------------------------------------------------
// shared-encoder registry
//
// LOCKING: shared_mtx_ is a strict LEAF. acquire/release take ONLY
// shared_mtx_ and must never take mtx_ or slot_mtx_, and must not hold
// shared_mtx_ across any other lock acquisition. Callers resolve the owner
// Config beforehand (config_by_slot_id, under mtx_ briefly) and pass it by
// value. start() calls acquire AFTER releasing that mtx_ lookup and BEFORE
// taking slot_mtx_; teardown_locked() calls release while holding slot_mtx_
// (allowed: shared_mtx_ is a leaf, never held while taking slot_mtx_/mtx_).
// Global order therefore remains mtx_ -> slot_mtx_ -> shared_mtx_.
// ----------------------------------------------------------------------------

SharedEncoder* SlotManager::acquire_shared_encoder(
    const std::string& group_key, const SceneSlot::Config& owner_cfg)
{
    std::lock_guard<std::mutex> lk(shared_mtx_);

    auto it = shared_.find(group_key);
    if (it == shared_.end()) {
        // First consumer for this group builds the context from the owner
        // Config. Concurrent first-start of the same group is serialized by
        // shared_mtx_: the first builds, every other find-and-increments.
        // A slot joining an existing context uses that context's (possibly
        // older) settings until use_count returns to 0 and it is rebuilt.
        auto ctx = std::make_unique<SharedEncoder>();
        if (!ctx->build(owner_cfg)) {
            blog(LOG_ERROR,
                 "[multi-scene-rec] failed to build shared encoder for group '%s'",
                 group_key.c_str());
            return nullptr; // ctx destroyed here -> partial cleanup in dtor
        }
        it = shared_.emplace(group_key, std::move(ctx)).first;
    }

    SharedEncoder* ctx = it->second.get();
    // Hand the caller its own strong ref (the matching final release of the
    // creation ref happens in release_shared_encoder when use_count hits 0).
    obs_encoder_t* ref = obs_encoder_get_ref(ctx->venc_);
    if (!ref) {
        blog(LOG_ERROR,
             "[multi-scene-rec] shared encoder for group '%s' is being destroyed",
             group_key.c_str());
        if (ctx->use_count_ == 0)
            shared_.erase(it); // nothing else holds it; drop the dead context
        return nullptr;
    }
    ++ctx->use_count_;
    return ctx;
}

void SlotManager::release_shared_encoder(const std::string& group_key)
{
    std::lock_guard<std::mutex> lk(shared_mtx_);

    auto it = shared_.find(group_key);
    if (it == shared_.end()) return; // defensive: never built / already gone
    SharedEncoder* ctx = it->second.get();

    // Release the caller's consumer ref (taken via obs_encoder_get_ref in
    // acquire_shared_encoder) and decrement the group's use-count.
    if (ctx->venc_) obs_encoder_release(ctx->venc_);
    if (ctx->use_count_ > 0) --ctx->use_count_;

    // Last consumer: destroy the context. ~SharedEncoder performs the
    // mandatory order — final encoder release (matching create), then view
    // (set_source null / remove / destroy), then scene (dec_showing /
    // release). Erasing under shared_mtx_ runs that dtor synchronously, so
    // a subsequent rebuild for the same key cannot collide.
    if (ctx->use_count_ == 0)
        shared_.erase(it);
}

std::string SlotManager::slot_name_by_id(const std::string& slot_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = id_index_.find(slot_id);
    if (it == id_index_.end() || it->second >= slots_.size())
        return {};
    return slots_[it->second]->config().name;
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
    obs_data_set_int(d, "replay_max_size_mb", c.replay_max_size_mb);

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
    c.replay_max_size_mb = (uint32_t)obs_data_get_int(d, "replay_max_size_mb");

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
    if (c.fps_num == 0 || c.fps_num > 240000) c.fps_num = 60;
    if (c.fps_den == 0 || c.fps_den > 1001)  c.fps_den = 1;
    if (c.container.empty())  c.container = "mp4";
    if (c.audio_bitrate == 0) c.audio_bitrate = 160;
    if (c.replay_seconds == 0) c.replay_seconds = 30;

    // Feature 006 — consumer-side normalization (Decision 2) and standalone
    // load-time validation (Decisions 3 and 4). Explicit if/else branching
    // keeps the consumer-only and standalone-only paths unambiguous.
    if (!c.shared_encoder_slot_id.empty()) {
        // T010 / FR-006 / FR-012 — consumer slots must never carry standalone
        // rate-control values. The sentinel + 0 are what every read site
        // (editor, log, replay-buffer estimate) recognises as "ask the helper
        // for the owner's effective values". The orphan-consumer warning
        // (T010b) runs in load_from's post-pass for the subset whose
        // shared_encoder_slot_id reference does not resolve.
        c.rate_control = "<inherited>";
        c.rc_value     = 0;
    } else {
        // Standalone slot: introspect the selected encoder once and validate
        // the persisted mode + value. Same property-accessor pattern the
        // editor uses, so the editor's range source and the load-time clamp
        // source agree by construction.
        obs_properties_t* props =
            c.video_encoder_id.empty()
                ? nullptr
                : obs_get_encoder_properties(c.video_encoder_id.c_str());
        if (props) {
            // T011 / Decision 4 / FR-015 — mode-substitute-and-warn.
            obs_property_t* rc_prop = obs_properties_get(props, "rate_control");
            if (rc_prop && obs_property_get_type(rc_prop) == OBS_PROPERTY_LIST) {
                size_t count = obs_property_list_item_count(rc_prop);
                bool in_list = false;
                std::string first_mode;
                for (size_t i = 0; i < count; ++i) {
                    const char* val = obs_property_list_item_string(rc_prop, i);
                    if (!val || !*val) continue;
                    if (first_mode.empty()) first_mode = val;
                    if (c.rate_control == val) { in_list = true; break; }
                }
                if (!in_list && !first_mode.empty()) {
                    std::string original_rc = c.rate_control;
                    c.rate_control = first_mode;
                    blog(LOG_WARNING,
                         "[multi-scene-rec] '%s': rate-control '%s' not supported by %s; substituted '%s'",
                         c.name.c_str(), original_rc.c_str(),
                         c.video_encoder_id.c_str(), c.rate_control.c_str());
                }
            }

            // T012 / Decision 3 / FR-013 — value-clamp-and-warn against the
            // (possibly substituted) mode. Bitrate-based modes use "bitrate";
            // quality-based modes walk rc_util::quality_keys() then
            // quality_split_keys() (first match wins) — same order as
            // set_quality_value's write site so the introspected range and
            // the write target are derived from the same list.
            if (!rc_util::is_lossless(c.rate_control)) {
                obs_property_t* range_p = nullptr;
                if (rc_util::is_bitrate_based(c.rate_control)) {
                    range_p = obs_properties_get(props, "bitrate");
                } else {
                    for (const char* const* k = rc_util::quality_keys(); *k; ++k) {
                        obs_property_t* p = obs_properties_get(props, *k);
                        if (p) { range_p = p; break; }
                    }
                    if (!range_p) {
                        for (const char* const* k = rc_util::quality_split_keys(); *k; ++k) {
                            obs_property_t* p = obs_properties_get(props, *k);
                            if (p) { range_p = p; break; }
                        }
                    }
                }
                if (range_p && obs_property_get_type(range_p) == OBS_PROPERTY_INT) {
                    int rmin = obs_property_int_min(range_p);
                    int rmax = obs_property_int_max(range_p);
                    if ((int)c.rc_value < rmin || (int)c.rc_value > rmax) {
                        uint32_t original_value = c.rc_value;
                        c.rc_value = (int)c.rc_value < rmin
                                         ? (uint32_t)rmin
                                         : (uint32_t)rmax;
                        blog(LOG_WARNING,
                             "[multi-scene-rec] '%s': rc value %u out of range for %s on %s [%d, %d]; clamped to %u",
                             c.name.c_str(), original_value, c.rate_control.c_str(),
                             c.video_encoder_id.c_str(), rmin, rmax, c.rc_value);
                    }
                }
            }
            obs_properties_destroy(props);
        }
    }

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
            // ITEM A: also persist this slot's two hotkey bindings into the
            // same per-slot obs_data under stable keys. Done here (not in
            // slot_to_data, which only sees a Config) because it needs the
            // live SceneSlot. slot_to_data's contract is unchanged.
            s->save_hotkey_bindings(d);
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
            slots_.emplace_back(std::make_shared<SceneSlot>(c));
            // ITEM A: hotkeys are registered later (FINISHED_LOADING /
            // SCENE_COLLECTION_CHANGED), so stash the saved bindings on the
            // new slot now; register_hotkeys() applies+releases them. Each
            // obs_data_get_array returns a ref this loop owns; ownership is
            // transferred to the slot (do not release here). Absent keys
            // (older saves / freshly added slots) -> nullptr -> normal
            // registration with no binding to restore.
            obs_data_array_t* hkr = obs_data_get_array(d, "hk_record");
            obs_data_array_t* hkp = obs_data_get_array(d, "hk_save_replay");
            slots_.back()->set_pending_hotkey_bindings(hkr, hkp);
            obs_data_release(d);
        }

        // T010b — orphan-consumer warning: any consumer whose
        // shared_encoder_slot_id does NOT resolve to a sibling owner is left
        // as an orphan. effective_rate_control returns safe last-resort
        // values for orphans and the editor displays "(inherited — owner
        // missing)" so the dangling reference is surfaced without crashing.
        // This second pass runs while still holding mtx_, so we iterate
        // slots_ directly rather than calling config_by_slot_id (which would
        // re-take mtx_).
        for (auto& consumer : slots_) {
            const auto& cc = consumer->config();
            if (cc.shared_encoder_slot_id.empty()) continue;
            bool resolved = false;
            for (auto& owner_s : slots_) {
                if (owner_s->config().id == cc.shared_encoder_slot_id) {
                    resolved = true;
                    break;
                }
            }
            if (!resolved) {
                blog(LOG_WARNING,
                     "[multi-scene-rec] '%s': shared_encoder_slot_id '%s' does not resolve "
                     "— orphan consumer; reads will return safe last-resort values until "
                     "the user re-points the slot or deletes it",
                     cc.name.c_str(), cc.shared_encoder_slot_id.c_str());
            }
        }

        rebuild_id_index();
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
