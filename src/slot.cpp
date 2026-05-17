#include "slot.hpp"
#include "manager.hpp"
#include "plugin-main.hpp"
#include "ui-dock.hpp"

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QMetaObject>

#include <cstdio>
#include <ctime>

// =============================================================================
// rate control helpers
// =============================================================================

namespace rc_util {

bool is_bitrate_based(const std::string& mode)
{
    // Bitrate-targeted modes across x264 / NVENC / AMF / QSV.
    static const char* bitrate_modes[] = {
        "CBR", "VBR", "ABR", "VBR_LAT", "VBR_HQ", "VBR_PEAK"
    };
    for (const char* m : bitrate_modes)
        if (mode == m) return true;
    return false; // CQP / CRF / CQ / ICQ / QVBR / Lossless -> not pure bitrate
}

bool is_lossless(const std::string& mode)
{
    return mode == "Lossless" || mode == "LOSSLESS" || mode == "lossless";
}

} // namespace rc_util

// =============================================================================
// helpers
// =============================================================================

static std::string generate_slot_id()
{
    static std::atomic<uint32_t> counter{0};
    uint64_t t = os_gettime_ns();
    uint32_t c = counter.fetch_add(1);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%llx%x", (unsigned long long)t, c);
    return buf;
}

static obs_source_t* fetch_scene_source(const std::string& name)
{
    obs_source_t* s = obs_get_source_by_name(name.c_str());
    if (!s) return nullptr;
    if (obs_source_get_type(s) != OBS_SOURCE_TYPE_SCENE) {
        obs_source_release(s);
        return nullptr;
    }
    return s;
}

static std::string build_output_filename(const SceneSlot::Config& cfg)
{
    char ts[64];
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    if (!strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", &tmv))
        std::snprintf(ts, sizeof(ts), "%lld", (long long)now);

    std::string path = cfg.path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    path += cfg.name.empty() ? std::string("slot") : cfg.name;
    path += "_";
    path += ts;
    path += ".";
    path += cfg.container.empty() ? std::string("mp4") : cfg.container;
    return path;
}

// Write the quality value into whichever quality-named setting the encoder
// actually exposes — discovered by introspecting its properties. Setting a key
// the encoder doesn't have is avoided; this keeps us encoder-agnostic.
static void set_quality_value(obs_data_t* settings, const char* enc_id, int value)
{
    obs_properties_t* props = obs_get_encoder_properties(enc_id);
    if (!props) {
        obs_data_set_int(settings, "cqp", value);
        return;
    }
    const char* single_keys[] = {
        "crf", "cqp", "cq_level", "qp", "icq_quality", "global_quality"
    };
    for (const char* k : single_keys) {
        if (obs_properties_get(props, k)) {
            obs_data_set_int(settings, k, value);
            obs_properties_destroy(props);
            return; // set only the first match
        }
    }
    // QSV split keys: set all three if present (they work as a unit)
    const char* split_keys[] = { "qpi", "qpp", "qpb" };
    bool any_split = false;
    for (const char* k : split_keys)
        if (obs_properties_get(props, k)) { obs_data_set_int(settings, k, value); any_split = true; }
    if (!any_split)
        obs_data_set_int(settings, "cqp", value); // last-resort fallback
    obs_properties_destroy(props);
}

// Encoder-family presets that are orthogonal to rate control: speed preset,
// h264 profile, tuning, multipass, etc. Matched by ID substring so newer ID
// variants still resolve.
static void apply_family_presets(obs_data_t* s, const std::string& enc_id,
                                  const SceneSlot::Config& cfg)
{
    obs_data_set_int(s, "keyint_sec", cfg.keyframe_interval_sec);

    auto has = [&](const char* needle) {
        return enc_id.find(needle) != std::string::npos;
    };

    // Resolves to `val` when non-empty, otherwise to `fallback`.
    // Preserves former hardcoded behavior for old saves that have empty strings.
    auto eff = [](const std::string& val, const char* fallback) -> const char* {
        return val.empty() ? fallback : val.c_str();
    };

    if (has("x264")) {
        obs_data_set_string(s, "preset",  eff(cfg.encoder_preset,  "veryfast"));
        obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));
        // tune: empty string is the valid x264 "no tune" value; set it directly.
        obs_data_set_string(s, "tune", cfg.encoder_tune.c_str());
        if (cfg.b_frames >= 0)
            obs_data_set_int(s, "bf", cfg.b_frames);

        if (cfg.advanced_settings) {
            obs_data_set_bool  (s, "cabac",   cfg.cabac);
            obs_data_set_bool  (s, "mbtree",  cfg.mbtree);
            if (!cfg.x264opts.empty())
                obs_data_set_string(s, "x264opts", cfg.x264opts.c_str());
            if (cfg.aq_mode >= 0)
                obs_data_set_int(s, "aq_mode", cfg.aq_mode);
            if (cfg.min_qp >= 0)
                obs_data_set_int(s, "qpmin",  cfg.min_qp);
            if (cfg.max_qp >= 0)
                obs_data_set_int(s, "qpmax",  cfg.max_qp);
        }
    } else if (has("nvenc")) {
        const char* eff_preset = eff(cfg.encoder_preset, "p5");
        obs_data_set_string(s, "preset",    eff_preset);   // ffmpeg nvenc compat
        obs_data_set_string(s, "preset2",   eff_preset);   // jim_nvenc
        obs_data_set_string(s, "profile",   eff(cfg.encoder_profile, "high"));
        obs_data_set_string(s, "tune",      eff(cfg.encoder_tune,    "hq"));
        obs_data_set_string(s, "multipass", eff(cfg.multipass,       "qres"));
        obs_data_set_bool  (s, "lookahead", cfg.lookahead);
        obs_data_set_bool  (s, "psycho_aq", cfg.psycho_aq);
        if (cfg.b_frames >= 0)
            obs_data_set_int(s, "bf", cfg.b_frames);
        if (cfg.gpu_index >= 0)
            obs_data_set_int(s, "gpu", cfg.gpu_index);

        if (cfg.advanced_settings) {
            obs_data_set_bool(s, "repeat_headers",  cfg.nvenc_repeat_headers);
            obs_data_set_bool(s, "force_idr",       cfg.nvenc_force_idr);
            obs_data_set_bool(s, "dynamic_bitrate", cfg.nvenc_dyn_bitrate);
            if (cfg.min_qp >= 0)
                obs_data_set_int(s, "min_qp", cfg.min_qp);
            if (cfg.max_qp >= 0)
                obs_data_set_int(s, "max_qp", cfg.max_qp);
        }
    } else if (has("amf")) {
        obs_data_set_string(s, "preset",  eff(cfg.encoder_preset,  "quality"));
        obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));
        if (cfg.gpu_index >= 0)
            obs_data_set_int(s, "gpu", cfg.gpu_index);

        if (cfg.advanced_settings) {
            obs_data_set_bool(s, "enforce_hrd",       cfg.amf_enforce_hrd);
            obs_data_set_bool(s, "vbaq",              cfg.amf_vbaq);
            obs_data_set_bool(s, "preanalysis",       cfg.amf_pre_analysis);
            obs_data_set_bool(s, "enable_throughput", cfg.amf_enable_throughput);
            if (cfg.min_qp >= 0)
                obs_data_set_int(s, "min_qp", cfg.min_qp);
            if (cfg.max_qp >= 0)
                obs_data_set_int(s, "max_qp", cfg.max_qp);
        }
    } else if (has("qsv")) {
        // QSV uses "target_usage" as its preset key.
        obs_data_set_string(s, "target_usage", eff(cfg.encoder_preset,  "balanced"));
        obs_data_set_string(s, "profile",       eff(cfg.encoder_profile, "high"));

        if (cfg.advanced_settings) {
            if (cfg.qsv_async_depth >= 0)
                obs_data_set_int(s, "async_depth", cfg.qsv_async_depth);
            if (!cfg.qsv_latency.empty())
                obs_data_set_string(s, "latency", cfg.qsv_latency.c_str());
        }
    } else if (has("videotoolbox") || has("apple")) {
        obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));

        if (cfg.advanced_settings) {
            obs_data_set_bool(s, "realtime", cfg.vt_realtime);
            if (cfg.vt_frames_before_start >= 0)
                obs_data_set_int(s, "frames_before_start",
                                 cfg.vt_frames_before_start);
        }
    }

    // HEVC/AV1 codec-specific: set unconditionally (non-HEVC/AV1 encoders ignore unknown keys).
    if (cfg.advanced_settings) {
        if (!cfg.hevc_tier.empty())
            obs_data_set_string(s, "tier", cfg.hevc_tier.c_str());
        if (cfg.av1_tile_cols >= 0)
            obs_data_set_int(s, "tile_cols", cfg.av1_tile_cols);
        if (cfg.av1_tile_rows >= 0)
            obs_data_set_int(s, "tile_rows", cfg.av1_tile_rows);
    }
}

// Full encoder settings: family presets + the configured rate control mode
// and its value, routed to the correct key by introspection.
static void apply_encoder_settings(obs_data_t* s, const std::string& enc_id,
                                   const SceneSlot::Config& cfg)
{
    apply_family_presets(s, enc_id, cfg);

    const std::string& rc = cfg.rate_control;
    obs_data_set_string(s, "rate_control", rc.c_str());

    if (rc_util::is_lossless(rc)) {
        // No numeric value for lossless.
    } else if (rc_util::is_bitrate_based(rc)) {
        obs_data_set_int(s, "bitrate", cfg.rc_value);
    } else {
        set_quality_value(s, enc_id.c_str(), (int)cfg.rc_value);
    }
}

// =============================================================================
// ctor/dtor
// =============================================================================

SceneSlot::SceneSlot(Config cfg) : cfg_(std::move(cfg))
{
    if (cfg_.id.empty()) cfg_.id = generate_slot_id();
}

SceneSlot::~SceneSlot()
{
    stop();
    unregister_hotkeys();
}

void SceneSlot::update_config(const Config& c, obs_encoder_t* resolved_venc)
{
    bool was_running = running_.load();
    bool had_hotkeys = (hotkey_record_ != OBS_INVALID_HOTKEY_ID);

    // stop() -> teardown() acquires slot_mtx_; must NOT be held here.
    if (was_running) stop();

    {
        // slot_mtx_ guards cfg_ (and all slot members). Held only for the
        // mutation and released before start(), which re-acquires it
        // (std::mutex is non-recursive).
        std::lock_guard<std::mutex> lk(slot_mtx_);
        std::string keep_id = cfg_.id;
        cfg_ = c;
        if (cfg_.id.empty())
            cfg_.id = keep_id.empty() ? generate_slot_id() : keep_id;
    }

    // Re-register so hotkey descriptions track the (possibly new) name.
    // Binding is keyed by the registration name string (which embeds the
    // stable id), so unregister+register preserves the user's binding.
    if (had_hotkeys) {
        unregister_hotkeys();
        register_hotkeys();
    }

    if (was_running) {
        // A dependent slot cannot run without its primary's encoder. The
        // caller (SlotManager::update_slot, holding mtx_) already resolved
        // it; if null the primary is not running. Skip the restart instead
        // of letting start() perform the locked lookup, which would
        // re-enter SlotManager::mtx_ on the caller's thread (deadlock).
        if (!cfg_.shared_encoder_slot_id.empty() && !resolved_venc) {
            blog(LOG_ERROR,
                 "[multi-scene-rec] '%s': primary slot '%s' not running, cannot restart dependent after edit",
                 cfg_.name.c_str(), cfg_.shared_encoder_slot_id.c_str());
        } else {
            start(resolved_venc);
        }
    }
}

// =============================================================================
// start / stop
// =============================================================================

bool SceneSlot::start(obs_encoder_t* borrowed_venc)
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return true;

    // Resolve a borrowed encoder for a dependent slot started standalone
    // (e.g. via hotkey) BEFORE taking slot_mtx_. The lookup acquires
    // SlotManager::mtx_, and the lock order is mtx_ -> slot_mtx_; doing it
    // while holding slot_mtx_ would invert the order. Callers inside
    // SlotManager (which hold mtx_) always pass a non-null encoder, so this
    // locked lookup only runs from the un-locked hotkey/standalone path.
    if (!cfg_.shared_encoder_slot_id.empty() && !borrowed_venc) {
        borrowed_venc = SlotManager::instance()
            .find_encoder_by_slot_id_locked(cfg_.shared_encoder_slot_id);
        if (!borrowed_venc) {
            blog(LOG_ERROR, "[multi-scene-rec] '%s': primary slot '%s' not running, cannot start",
                 cfg_.name.c_str(), cfg_.shared_encoder_slot_id.c_str());
            running_.store(false);
            return false;
        }
    }

    // slot_mtx_ guards all SceneSlot members written below (outputs,
    // encoders, view, scene source, cfg_). Held for the rest of start().
    // The atomic CAS above remains the fast early-out.
    std::lock_guard<std::mutex> lk(slot_mtx_);

    // Reset before (re)creating encoders; setup_encoders() sets it true on
    // the obs_x264/CBR fallback path.
    encoder_fallback_ = false;

    if (cfg_.audio_tracks == 0) {
        blog(LOG_WARNING, "[multi-scene-rec] '%s': no audio tracks selected; defaulting to track 1",
             cfg_.name.c_str());
        cfg_.audio_tracks = 0x01;
    }

    // replay-only is meaningless without the replay buffer; enforce it.
    if (cfg_.replay_only && !cfg_.replay_enabled) {
        blog(LOG_WARNING, "[multi-scene-rec] '%s': replay-only set but replay disabled; enabling replay",
             cfg_.name.c_str());
        cfg_.replay_enabled = true;
    }

    if (cfg_.path.empty()) {
        blog(LOG_ERROR, "[multi-scene-rec] '%s': output path is empty", cfg_.name.c_str());
        running_.store(false);
        return false;
    }
    if (!os_file_exists(cfg_.path.c_str())) {
        blog(LOG_ERROR, "[multi-scene-rec] '%s': output path does not exist: %s",
             cfg_.name.c_str(), cfg_.path.c_str());
        running_.store(false);
        return false;
    }

    const bool is_dependent = !cfg_.shared_encoder_slot_id.empty();

    if (is_dependent) {
        // Encoder was resolved before slot_mtx_ was taken (see top of
        // start()); borrowed_venc is guaranteed non-null here.
        owns_venc_ = false;
        // Hold a strong reference for the duration of our use. The public API
        // exposes obs_encoder_get_ref() for this; obs_encoder_addref() is
        // internal-only (obs-internal.h) and not exported.
        venc_ = obs_encoder_get_ref(borrowed_venc);
        if (!venc_) {
            blog(LOG_ERROR, "[multi-scene-rec] '%s': borrowed encoder is being destroyed",
                 cfg_.name.c_str());
            running_.store(false);
            return false;
        }
    } else {
        owns_venc_ = true;

        scene_src_ = fetch_scene_source(cfg_.scene_name);
        if (!scene_src_) {
            blog(LOG_WARNING, "[multi-scene-rec] '%s': scene '%s' not found",
                 cfg_.name.c_str(), cfg_.scene_name.c_str());
            running_.store(false);
            return false;
        }

        obs_source_inc_showing(scene_src_);
        showing_held_ = true;

        if (!setup_video()) { running_.store(false); teardown_locked(); return false; }
    }

    if (!setup_encoders()) { running_.store(false); teardown_locked(); return false; }

    if (!venc_) {
        blog(LOG_ERROR, "[multi-scene-rec] '%s': no video encoder available", cfg_.name.c_str());
        running_.store(false); teardown_locked(); return false;
    }

    if (!setup_outputs())  { running_.store(false); teardown_locked(); return false; }

    // Recording output is absent in replay-only mode.
    if (rec_out_ && !obs_output_start(rec_out_)) {
        blog(LOG_ERROR, "[multi-scene-rec] '%s' rec start failed: %s",
             cfg_.name.c_str(), obs_output_get_last_error(rec_out_));
        running_.store(false);
        teardown_locked();
        return false;
    }
    if (replay_out_ && !obs_output_start(replay_out_)) {
        blog(LOG_WARNING, "[multi-scene-rec] '%s' replay start failed: %s",
             cfg_.name.c_str(), obs_output_get_last_error(replay_out_));
        obs_output_release(replay_out_);
        replay_out_ = nullptr;
        if (cfg_.replay_only) {
            // The replay buffer was the only output -- nothing left to run.
            blog(LOG_ERROR, "[multi-scene-rec] '%s' replay-only but replay failed to start",
                 cfg_.name.c_str());
            running_.store(false);
            teardown_locked();
            return false;
        }
    }

    running_.store(true);

    char tracks_str[16] = {0};
    int  pos = 0;
    for (int i = 0; i < 6; ++i)
        if (cfg_.audio_tracks & (1u << i))
            pos += std::snprintf(tracks_str + pos, sizeof(tracks_str) - pos,
                                 "%s%d", pos ? "," : "", i + 1);

    blog(LOG_INFO, "[multi-scene-rec] '%s' started (%ux%u@%u, %s/%u, tracks=%s, %s)",
         cfg_.name.c_str(), cfg_.width, cfg_.height, cfg_.fps_num,
         cfg_.rate_control.c_str(), cfg_.rc_value, tracks_str,
         cfg_.replay_only      ? "replay-only" :
         cfg_.replay_enabled   ? "rec+replay"  : "rec-only");
    return true;
}

void SceneSlot::stop()
{
    if (!running_.exchange(false) && !showing_held_ && !scene_src_) return;
    teardown();
}

void SceneSlot::teardown()
{
    // Public entry: take slot_mtx_ (guards all members touched below), then
    // run the core. start()'s in-lock failure paths call teardown_locked()
    // directly since they already hold slot_mtx_ (non-recursive).
    std::lock_guard<std::mutex> lk(slot_mtx_);
    teardown_locked();
}

void SceneSlot::wait_for_output_stop(obs_output_t* out)
{
    if (!out) return;
    // obs_output_stop() is asynchronous: it only requests a stop. Block until
    // the output's encode/mux thread has actually finished so destroying the
    // view/encoders afterwards cannot be a use-after-free.
    const int max_iters = 500; // 500 * 10 ms = 5 s
    int iters = 0;
    while (obs_output_active(out) && iters < max_iters) {
        os_sleep_ms(10);
        ++iters;
    }
    if (obs_output_active(out)) {
        blog(LOG_WARNING,
             "[multi-scene-rec] '%s': output did not stop within 5s; forcing",
             cfg_.name.c_str());
        obs_output_force_stop(out);
    }
}

// Safe teardown order (caller holds slot_mtx_):
//   a. request stop on both outputs (after disconnecting their stop signal so
//      the handler does not re-enter a half-destroyed slot)
//   b. wait for each output's async stop to actually complete
//   c. release the outputs
//   d. release audio encoders
//   e. release the video encoder (owned OR borrowed -- we always hold a ref)
//   f. detach + destroy the per-slot view (clear its source first)
//   g. drop the scene "showing" reference
//   h. release the scene source
void SceneSlot::teardown_locked()
{
    // Note: hotkeys are intentionally NOT touched here. Their lifecycle is
    // owned by SlotManager so the record-toggle hotkey survives stop().

    // (a) Disconnect stop-signal handlers, then request stop on both outputs.
    if (rec_out_) {
        signal_handler_disconnect(
            obs_output_get_signal_handler(rec_out_),
            "stop", &SceneSlot::on_rec_output_stop, this);
        obs_output_stop(rec_out_);
    }
    if (replay_out_) {
        signal_handler_disconnect(
            obs_output_get_signal_handler(replay_out_),
            "stop", &SceneSlot::on_replay_output_stop, this);
        obs_output_stop(replay_out_);
    }

    // (b) Wait for the async stop to actually finish on each output.
    wait_for_output_stop(rec_out_);
    wait_for_output_stop(replay_out_);

    // (c) Release the outputs.
    if (rec_out_)    { obs_output_release(rec_out_);    rec_out_    = nullptr; }
    if (replay_out_) { obs_output_release(replay_out_); replay_out_ = nullptr; }

    // (d) Release audio encoders.
    for (auto* aenc : aencs_) if (aenc) obs_encoder_release(aenc);
    aencs_.clear();
    selected_tracks_.clear();

    // (e) Release the video encoder. We always hold a reference (owned via
    //     obs_video_encoder_create, or borrowed + obs_encoder_get_ref'd in
    //     start()), so release unconditionally.
    if (venc_) {
        obs_encoder_release(venc_); // release whether owned or borrowed
        venc_ = nullptr;
    }
    owns_venc_ = true;  // reset for next start cycle

    // (f) Detach and destroy the per-slot view. Clear its source first.
    if (view_) {
        obs_view_set_source(view_, 0, nullptr);
        obs_view_remove(view_);
        obs_view_destroy(view_);
        view_ = nullptr;
        video_ = nullptr;
    }

    // (g) Drop the scene "showing" reference.
    if (showing_held_ && scene_src_) {
        obs_source_dec_showing(scene_src_);
        showing_held_ = false;
    }
    // (h) Release the scene source.
    if (scene_src_) { obs_source_release(scene_src_); scene_src_ = nullptr; }
}

// =============================================================================
// video setup
// =============================================================================

bool SceneSlot::setup_video()
{
    view_ = obs_view_create();
    obs_view_set_source(view_, 0, scene_src_);

    struct obs_video_info ovi = {};
    ovi.fps_num        = cfg_.fps_num;
    ovi.fps_den        = cfg_.fps_den;
    ovi.base_width     = cfg_.width;
    ovi.base_height    = cfg_.height;
    ovi.output_width   = cfg_.width;
    ovi.output_height  = cfg_.height;
    ovi.output_format  = VIDEO_FORMAT_NV12;
    struct obs_video_info main_ovi = {};
    obs_get_video_info(&main_ovi);
    ovi.colorspace     = main_ovi.colorspace;
    ovi.range          = main_ovi.range;
    ovi.gpu_conversion = true;
    ovi.scale_type     = OBS_SCALE_BICUBIC;

    video_ = obs_view_add2(view_, &ovi);
    if (!video_) {
        blog(LOG_ERROR, "[multi-scene-rec] obs_view_add2 failed for '%s'",
             cfg_.name.c_str());
        return false;
    }
    return true;
}

// =============================================================================
// encoders
// =============================================================================

bool SceneSlot::setup_encoders()
{
    // ---- video encoder (only when this slot owns its encoder) ----
    if (owns_venc_) {
        const std::string enc_id =
            cfg_.video_encoder_id.empty() ? std::string("obs_x264")
                                          : cfg_.video_encoder_id;

        obs_data_t* vs = obs_data_create();
        apply_encoder_settings(vs, enc_id, cfg_);
        venc_ = obs_video_encoder_create(enc_id.c_str(),
                                         ("venc_" + cfg_.id).c_str(),
                                         vs, nullptr);
        obs_data_release(vs);

        if (!venc_) {
            // Fall back to x264 + CBR if the requested encoder is unavailable on
            // this machine. The saved rate control mode may not exist on x264
            // (e.g. CQP), so force CBR with a safe bitrate.
            blog(LOG_WARNING,
                 "[multi-scene-rec] '%s' encoder '%s' unavailable, falling back to obs_x264/CBR",
                 cfg_.name.c_str(), enc_id.c_str());
            obs_data_t* fs = obs_data_create();
            apply_family_presets(fs, "obs_x264", cfg_);
            obs_data_set_string(fs, "rate_control", "CBR");
            obs_data_set_int(fs, "bitrate",
                             rc_util::is_bitrate_based(cfg_.rate_control)
                                 ? cfg_.rc_value : 6000);
            venc_ = obs_video_encoder_create("obs_x264",
                                             ("venc_" + cfg_.id).c_str(),
                                             fs, nullptr);
            obs_data_release(fs);
            if (!venc_) {
                blog(LOG_ERROR, "[multi-scene-rec] video encoder create failed");
                return false;
            }
            // The user's rate-control configuration was discarded; surface
            // this in the UI (see Stats::encoder_fallback).
            encoder_fallback_ = true;
        }
        obs_encoder_set_video(venc_, video_);
    }

    // ---- audio encoders: one per selected OBS track ----
    audio_t* main_audio = obs_get_audio();
    if (!main_audio) {
        blog(LOG_ERROR, "[multi-scene-rec] obs_get_audio() returned null");
        return false;
    }

    aencs_.clear();
    selected_tracks_.clear();
    for (int track = 0; track < 6; ++track) {
        if ((cfg_.audio_tracks & (1u << track)) == 0) continue;

        obs_data_t* as = obs_data_create();
        obs_data_set_int(as, "bitrate", cfg_.audio_bitrate);

        std::string nm = "aenc_" + cfg_.id + "_t" + std::to_string(track + 1);
        obs_encoder_t* aenc = obs_audio_encoder_create(cfg_.audio_encoder_id.c_str(),
                                                       nm.c_str(),
                                                       as,
                                                       (size_t)track,
                                                       nullptr);
        obs_data_release(as);
        if (!aenc) {
            blog(LOG_ERROR, "[multi-scene-rec] audio encoder create failed for track %d", track + 1);
            return false;
        }
        obs_encoder_set_audio(aenc, main_audio);
        aencs_.push_back(aenc);
        selected_tracks_.push_back(track); // 0-based OBS track index
    }
    if (aencs_.empty()) {
        blog(LOG_ERROR, "[multi-scene-rec] no audio encoders created");
        return false;
    }
    return true;
}

// =============================================================================
// outputs
// =============================================================================

bool SceneSlot::setup_outputs()
{
    // ---- recording (ffmpeg_muxer) -- skipped entirely in replay-only mode ----
    if (!cfg_.replay_only) {
        obs_data_t* rs = obs_data_create();
        obs_data_set_string(rs, "path", build_output_filename(cfg_).c_str());
        obs_data_set_string(rs, "muxer_settings", "");

        rec_out_ = obs_output_create("ffmpeg_muxer",
                                     ("rec_out_" + cfg_.id).c_str(),
                                     rs, nullptr);
        obs_data_release(rs);
        if (!rec_out_) {
            blog(LOG_ERROR, "[multi-scene-rec] rec output create failed");
            return false;
        }
        obs_output_set_video_encoder(rec_out_, venc_);
        // Attach each encoder at the output audio index matching the encoder's
        // mixer (OBS track) index, not a dense 0..N-1 counter -- otherwise
        // non-contiguous track selections route to the wrong/empty tracks.
        for (size_t i = 0; i < aencs_.size(); ++i)
            obs_output_set_audio_encoder(rec_out_, aencs_[i],
                                         (size_t)selected_tracks_[i]);
        obs_output_set_mixers(rec_out_, cfg_.audio_tracks);

        // Detect external/error stops (disk full, encoder failure, ...).
        signal_handler_connect(
            obs_output_get_signal_handler(rec_out_),
            "stop", &SceneSlot::on_rec_output_stop, this);
    }

    // ---- replay buffer (optional) ----
    if (cfg_.replay_enabled) {
        // Size cap is a safety net. For quality-based rate control there is no
        // bitrate figure, so assume a generous 12 Mbps for the estimate.
        uint32_t est_kbps = rc_util::is_bitrate_based(cfg_.rate_control)
                                ? cfg_.rc_value : 12000;
        auto popcount = [](uint32_t v) -> uint32_t {
            uint32_t c = 0;
            for (; v; v >>= 1) c += v & 1;
            return c;
        };
        est_kbps += cfg_.audio_bitrate * popcount(cfg_.audio_tracks);

        obs_data_t* rb = obs_data_create();
        obs_data_set_string(rb, "directory", cfg_.path.c_str());
        obs_data_set_string(rb, "format", "Replay_%CCYY-%MM-%DD_%hh-%mm-%ss");
        obs_data_set_string(rb, "extension",
                            cfg_.container.empty() ? "mp4" : cfg_.container.c_str());
        obs_data_set_int(rb, "max_time_sec", cfg_.replay_seconds);
        uint64_t max_size_mb =
            (uint64_t)est_kbps * cfg_.replay_seconds / 8 / 1024 * 3 / 2;
        if (max_size_mb < 50) max_size_mb = 50;
        obs_data_set_int(rb, "max_size_mb", (long long)max_size_mb);

        replay_out_ = obs_output_create("replay_buffer",
                                        ("replay_out_" + cfg_.id).c_str(),
                                        rb, nullptr);
        obs_data_release(rb);
        if (!replay_out_) {
            blog(LOG_WARNING, "[multi-scene-rec] '%s' replay create failed",
                 cfg_.name.c_str());
        } else {
            obs_output_set_video_encoder(replay_out_, venc_);
            // Same track-aware attachment as the recording output.
            for (size_t i = 0; i < aencs_.size(); ++i)
                obs_output_set_audio_encoder(replay_out_, aencs_[i],
                                             (size_t)selected_tracks_[i]);
            obs_output_set_mixers(replay_out_, cfg_.audio_tracks);

            // Detect external/error stops on the replay output too.
            signal_handler_connect(
                obs_output_get_signal_handler(replay_out_),
                "stop", &SceneSlot::on_replay_output_stop, this);

            if (cfg_.container == "mp4" || cfg_.container == "MP4") {
                blog(LOG_WARNING,
                     "[multi-scene-rec] '%s': MP4 replay buffer will be unrecoverable if OBS crashes before save. Prefer MKV.",
                     cfg_.name.c_str());
            }
        }
    }

    // At least one output must have been created.
    if (!rec_out_ && !replay_out_) {
        blog(LOG_ERROR, "[multi-scene-rec] '%s' no outputs created", cfg_.name.c_str());
        return false;
    }
    return true;
}

// =============================================================================
// hotkeys
// =============================================================================

void SceneSlot::register_hotkeys()
{
    if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) return; // already registered

    std::string rec_name = "multi_scene_rec.record." + cfg_.id;
    std::string rec_desc = "Toggle Recording: " + cfg_.name;
    hotkey_record_ = obs_hotkey_register_frontend(
        rec_name.c_str(), rec_desc.c_str(), &SceneSlot::on_record_hotkey, this);

    std::string rep_name = "multi_scene_rec.save_replay." + cfg_.id;
    std::string rep_desc = "Save Replay: " + cfg_.name;
    hotkey_replay_ = obs_hotkey_register_frontend(
        rep_name.c_str(), rep_desc.c_str(), &SceneSlot::on_save_hotkey, this);
}

void SceneSlot::unregister_hotkeys()
{
    if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(hotkey_record_);
        hotkey_record_ = OBS_INVALID_HOTKEY_ID;
    }
    if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(hotkey_replay_);
        hotkey_replay_ = OBS_INVALID_HOTKEY_ID;
    }
}

void SceneSlot::on_record_hotkey(void* data, obs_hotkey_id /*id*/,
                                 obs_hotkey_t* /*hk*/, bool pressed)
{
    if (!pressed) return;
    auto* self = static_cast<SceneSlot*>(data);
    if (!self) return;
    if (self->is_running()) {
        self->stop();
        // A primary stopping via hotkey must also stop dependents that
        // borrow its encoder; otherwise they keep a stale reference. This
        // path does not hold SlotManager::mtx_, so acquiring it here keeps
        // the mtx_ -> slot_mtx_ order.
        SlotManager::instance().stop_dependents_of(self->config().id);
    } else {
        self->start();
    }
}

void SceneSlot::on_rec_output_stop(void* data, calldata_t* cd)
{
    auto* self = static_cast<SceneSlot*>(data);
    if (!self) return;
    long long code = calldata_int(cd, "code");
    // OBS_OUTPUT_SUCCESS (0) == intentional stop (our own obs_output_stop()
    // from teardown). Ignore to avoid double-teardown.
    if (code == OBS_OUTPUT_SUCCESS) return;

    blog(LOG_WARNING,
         "[multi-scene-rec] '%s': recording output stopped externally (code %lld)",
         self->config().name.c_str(), code);
    self->stop();
    if (auto* dock = get_dock())
        QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);
}

void SceneSlot::on_replay_output_stop(void* data, calldata_t* cd)
{
    auto* self = static_cast<SceneSlot*>(data);
    if (!self) return;
    long long code = calldata_int(cd, "code");
    if (code == OBS_OUTPUT_SUCCESS) return;

    blog(LOG_WARNING,
         "[multi-scene-rec] '%s': replay output stopped externally (code %lld)",
         self->config().name.c_str(), code);
    self->stop();
    if (auto* dock = get_dock())
        QMetaObject::invokeMethod(dock, "refresh", Qt::QueuedConnection);
}

void SceneSlot::on_save_hotkey(void* data, obs_hotkey_id /*id*/,
                               obs_hotkey_t* /*hk*/, bool pressed)
{
    if (!pressed) return;
    auto* self = static_cast<SceneSlot*>(data);
    if (self) self->save_replay();
}

bool SceneSlot::save_replay()
{
    // slot_mtx_ guards replay_out_ against concurrent teardown.
    std::lock_guard<std::mutex> lk(slot_mtx_);
    if (!replay_out_) return false;
    proc_handler_t* ph = obs_output_get_proc_handler(replay_out_);
    if (!ph) return false;
    calldata_t cd; calldata_init(&cd);
    bool ok = proc_handler_call(ph, "save", &cd);
    calldata_free(&cd);
    blog(LOG_INFO, "[multi-scene-rec] '%s' replay save %s",
         cfg_.name.c_str(), ok ? "OK" : "FAILED");
    return ok;
}

// =============================================================================
// stats
// =============================================================================

SceneSlot::Stats SceneSlot::stats()
{
    Stats out;
    if (!running_.load()) return out;

    // slot_mtx_ guards rec_out_/replay_out_/cfg_/encoder_fallback_.
    // stats_mtx_ (the bitrate sampler) is acquired AFTER slot_mtx_ to keep
    // the global lock order slot_mtx_ -> stats_mtx_.
    std::lock_guard<std::mutex> lk(slot_mtx_);

    // In replay-only mode there is no rec_out_; fall back to the replay
    // buffer output for the frame/byte counters.
    obs_output_t* primary = rec_out_ ? rec_out_ : replay_out_;
    if (!primary) return out;

    out.active           = obs_output_active(primary);
    out.replay_active    = replay_out_ ? obs_output_active(replay_out_) : false;
    out.total_frames     = obs_output_get_total_frames(primary);
    out.dropped_frames   = obs_output_get_frames_dropped(primary);
    out.total_bytes      = obs_output_get_total_bytes(primary);
    out.encoder_fallback = encoder_fallback_;

    uint64_t now_ns = os_gettime_ns();
    std::lock_guard<std::mutex> slk(stats_mtx_);
    if (last_sample_bytes_ns_ != 0) {
        uint64_t dt_ns  = now_ns - last_sample_bytes_ns_;
        uint64_t dbytes = out.total_bytes >= last_sample_bytes_ ?
                          out.total_bytes - last_sample_bytes_ : 0;
        if (dt_ns > 0) {
            double bits_per_sec = (double)dbytes * 8.0 * (1e9 / (double)dt_ns);
            last_kbps_ = bits_per_sec / 1000.0;
        }
    }
    last_sample_bytes_ns_ = now_ns;
    last_sample_bytes_    = out.total_bytes;
    out.kbps              = last_kbps_;
    return out;
}

void SceneSlot::reset_stats_sampler()
{
    std::lock_guard<std::mutex> lk(stats_mtx_);
    last_sample_bytes_ns_ = 0;
    last_sample_bytes_    = 0;
    last_kbps_            = 0.0;
}

obs_encoder_t* SceneSlot::video_encoder() const
{
    // slot_mtx_ guards venc_ against concurrent start()/teardown(). Returns
    // a copy of the pointer. Lock order: callers inside SlotManager hold
    // mtx_ first (mtx_ -> slot_mtx_); no inner lock is held here.
    std::lock_guard<std::mutex> lk(slot_mtx_);
    return venc_;
}
