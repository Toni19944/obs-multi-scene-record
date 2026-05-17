#pragma once

#include <obs.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// One independent recording + optional replay buffer attached to a single scene.
//
// Video: per-slot obs_view_t at slot resolution/FPS, fully independent.
// Audio: shares OBS's main audio mix. Each enabled track gets its own audio
//        encoder bound at the corresponding mixer index; all encoders attach
//        to the output at consecutive positions -> multi-track output file.
// Rate control: the mode string and its value are stored generically. The
//        editor discovers valid modes by introspecting the encoder's
//        properties, so this struct doesn't hardcode any encoder specifics.
class SceneSlot {
public:
    struct Config {
        // Stable unique identity, generated once, persisted. Used for hotkey
        // registration names so bindings survive renames/reorders. NOT shown
        // in the UI — `name` is the user-facing label.
        std::string id;

        std::string name;
        std::string scene_name;
        uint32_t width  = 1920;
        uint32_t height = 1080;
        uint32_t fps_num = 60;
        uint32_t fps_den = 1;
        std::string path;
        std::string container;
        std::string video_encoder_id = "obs_x264";
        std::string shared_encoder_slot_id;
        std::string audio_encoder_id = "ffmpeg_aac";

        // Rate control. `rate_control` is the encoder's mode string ("CBR",
        // "CQP", "CRF", ...). `rc_value` is bitrate in kbps for bitrate-based
        // modes, or the quality level (CRF/CQP/etc.) for quality-based modes.
        std::string rate_control = "CBR";
        uint32_t    rc_value = 6000;

        uint32_t audio_bitrate = 160;
        // Bitmask of OBS audio tracks 1-6 to record. Bit 0 = track 1.
        uint32_t audio_tracks = 0x01;

        bool replay_enabled = false;
        // When true, the slot runs ONLY its replay buffer -- no continuous
        // recording file. Implies replay_enabled. Save clips via hotkey/button.
        bool replay_only = false;
        uint32_t replay_seconds = 30;

        // ---- Video encoder: user-configurable ----

        // Keyframe interval in seconds. Replaces the hardcoded 2 in apply_family_presets.
        uint32_t keyframe_interval_sec = 2;

        // Encoder preset. Empty string → apply_family_presets uses the former hardcoded default
        // for that family (see Part 3). Keys: x264/AMF/NVENC→"preset"+"preset2", QSV→"target_usage".
        std::string encoder_preset = "";

        // Encoder profile. Empty → former hardcoded default ("high" for all families).
        // Key: "profile" for all families.
        std::string encoder_profile = "";

        // Encoder tune. Empty → former hardcoded default ("" for x264, "hq" for NVENC).
        // Key: "tune" for x264 and NVENC. Not used for AMF/QSV/VideoToolbox.
        std::string encoder_tune = "";

        // NVENC multipass mode. Empty → former hardcoded default ("qres").
        // Key: "multipass". NVENC only.
        std::string multipass = "";

        // NVENC look-ahead. Key: "lookahead". NVENC only. Former hardcoded default: false.
        bool lookahead = false;

        // NVENC Psycho Visual Tuning. Key: "psycho_aq". NVENC only. Former hardcoded: true.
        bool psycho_aq = true;

        // B-frames. -1 = do not set (encoder default). Key: "bf". x264 and NVENC only.
        int b_frames = -1;

        // GPU device index. -1 = do not set (encoder default). Key: "gpu". NVENC and AMF only.
        int gpu_index = -1;

        // ---- Video encoder: advanced (applied only when advanced_settings = true) ----

        // Whether the advanced settings below are applied during encoder setup.
        bool advanced_settings = false;

        // Max quantiser parameter. -1 = do not set.
        // Key: "qpmax" for x264; "max_qp" for NVENC and AMF.
        int max_qp = -1;

        // Min quantiser parameter. -1 = do not set.
        // Key: "qpmin" for x264; "min_qp" for NVENC and AMF.
        int min_qp = -1;

        // x264: enable CABAC entropy coding. Key: "cabac". Default true (x264 default).
        bool cabac = true;

        // x264: extra options passthrough string. Key: "x264opts". Default "".
        std::string x264opts = "";

        // x264: enable macroblock-tree rate control. Key: "mbtree". Default true (x264 default).
        bool mbtree = true;

        // x264: AQ mode. -1 = do not set (encoder default).
        // Key: "aq_mode". Values: 0=disabled, 1=variance, 2=autovariance, 3=autovariance+bias.
        int aq_mode = -1;

        // NVENC: write SPS/PPS before every IDR frame. Key: "repeat_headers". Default false.
        bool nvenc_repeat_headers = false;

        // NVENC: force IDR on scene cut. Key: "force_idr". Default false.
        bool nvenc_force_idr = false;

        // NVENC: dynamic bitrate control. Key: "dynamic_bitrate". Default false.
        bool nvenc_dyn_bitrate = false;

        // AMF: enforce HRD. Key: "enforce_hrd". Default false.
        bool amf_enforce_hrd = false;

        // AMF: VBAQ (Variance Based Adaptive Quantization). Key: "vbaq". Default false.
        bool amf_vbaq = false;

        // AMF: pre-analysis. Key: "preanalysis". Default false.
        bool amf_pre_analysis = false;

        // AMF: enable throughput mode. Key: "enable_throughput". Default false.
        bool amf_enable_throughput = false;

        // QSV: async submission depth. -1 = do not set. Key: "async_depth".
        int qsv_async_depth = -1;

        // QSV: latency mode. "" = do not set. Key: "latency". Values: "normal", "low".
        std::string qsv_latency = "";

        // VideoToolbox: realtime encoding priority. Key: "realtime". Default false.
        bool vt_realtime = false;

        // VideoToolbox: warm-up frames before start. -1 = do not set. Key: "frames_before_start".
        int vt_frames_before_start = -1;

        // HEVC tier (any HEVC encoder). "" = do not set. Key: "tier". Values: "main", "high".
        std::string hevc_tier = "";

        // AV1 tile columns (any AV1 encoder). -1 = do not set. Key: "tile_cols".
        int av1_tile_cols = -1;

        // AV1 tile rows (any AV1 encoder). -1 = do not set. Key: "tile_rows".
        int av1_tile_rows = -1;
    };

    struct Stats {
        bool     active           = false;
        bool     replay_active    = false;
        int      total_frames     = 0;
        int      dropped_frames   = 0;
        uint64_t total_bytes      = 0;
        double   kbps             = 0.0;
        bool     encoder_fallback = false;
    };
    Stats stats();
    void  reset_stats_sampler();

    explicit SceneSlot(Config cfg);
    ~SceneSlot();

    SceneSlot(const SceneSlot&) = delete;
    SceneSlot& operator=(const SceneSlot&) = delete;

    bool start(obs_encoder_t* borrowed_venc = nullptr);
    void stop();
    bool is_running() const { return running_.load(); }

    bool save_replay();

    const Config& config() const { return cfg_; }
    void update_config(const Config& c, obs_encoder_t* resolved_venc = nullptr);

    // Returns the live video encoder under slot_mtx_ (copy of the pointer).
    obs_encoder_t* video_encoder() const;

    // Hotkey lifecycle is owned by the caller (SlotManager), not by start/stop,
    // so the "toggle recording" hotkey works even while the slot is stopped.
    // Both calls are idempotent.
    void register_hotkeys();
    void unregister_hotkeys();

    static void on_record_hotkey(void* data, obs_hotkey_id id,
                                 obs_hotkey_t* hotkey, bool pressed);
    static void on_save_hotkey(void* data, obs_hotkey_id id,
                               obs_hotkey_t* hotkey, bool pressed);

    // Output "stop" signal handlers. Fired by libobs when an output stops,
    // including external/error stops (disk full, encoder failure, ...).
    static void on_rec_output_stop(void* data, calldata_t* cd);
    static void on_replay_output_stop(void* data, calldata_t* cd);

private:
    bool setup_video();
    bool setup_encoders();
    bool setup_outputs();
    // Public locking entry point: acquires slot_mtx_ then calls teardown_locked().
    void teardown();
    // Core teardown. Caller MUST already hold slot_mtx_ (e.g. start()'s
    // in-lock failure paths) so the non-recursive mutex is not re-entered.
    void teardown_locked();

    // Blocks until obs_output_active(out) is false (async stop completes),
    // up to a 5 s timeout; force-stops as a last resort.
    void wait_for_output_stop(obs_output_t* out);

    // Guards every mutable SceneSlot member below (outputs, encoders, view,
    // scene source, cfg_). Acquired by start()/teardown()/stats()/
    // save_replay()/update_config()/video_encoder(). Lock order:
    // SlotManager::mtx_ -> SceneSlot::slot_mtx_ -> SceneSlot::stats_mtx_.
    mutable std::mutex slot_mtx_;

    Config cfg_;
    std::atomic<bool> running_{false};

    // True when the configured encoder was unavailable and we fell back to
    // obs_x264/CBR. Surfaced to the UI so the user knows their rate-control
    // configuration was not applied. Guarded by slot_mtx_.
    bool encoder_fallback_ = false;

    obs_source_t* scene_src_    = nullptr;
    std::atomic<bool> showing_held_{false};

    obs_view_t* view_  = nullptr;
    video_t*    video_ = nullptr;

    obs_encoder_t* venc_ = nullptr;
    bool owns_venc_ = true;
    std::vector<obs_encoder_t*> aencs_;   // one encoder per selected track
    // 0-based OBS track index for each entry in aencs_, in push order. Used
    // to attach each encoder at the matching output audio index.
    std::vector<int> selected_tracks_;

    obs_output_t* rec_out_    = nullptr;
    obs_output_t* replay_out_ = nullptr;

    obs_hotkey_id hotkey_record_ = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_replay_ = OBS_INVALID_HOTKEY_ID;

    std::mutex stats_mtx_;
    uint64_t   last_sample_bytes_ns_ = 0;
    uint64_t   last_sample_bytes_    = 0;
    double     last_kbps_            = 0.0;
};

// --- rate control helpers (shared with the UI editor) ------------------------

namespace rc_util {

// True if `mode` is a bitrate-targeted rate control (value = kbps).
// False for quality-targeted modes (CQP/CRF/ICQ/...) and Lossless.
bool is_bitrate_based(const std::string& mode);

// True if `mode` takes no numeric value at all.
bool is_lossless(const std::string& mode);

} // namespace rc_util
