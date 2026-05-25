#pragma once

#include <obs.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Forward-declared so SceneSlot::setup_outputs can take it by const-ref. The
// full definition lives in manager.hpp, which itself includes this header —
// breaking the cycle with a forward declaration here keeps both directions
// safe.
struct EffectiveRC;

// One independent recording + optional replay buffer.
//
// Video: the scene/view/video/encoder pipeline lives in a refcounted
//        SharedEncoder context owned by SlotManager and keyed by an
//        encoder-group key (this slot's own id when it owns the encoder, or
//        the referenced slot's id when it shares one). A slot is just a
//        consumer of that context: it acquires it in start() and releases it
//        in teardown. The context (and its scene/view/video/encoder) is built
//        on the first consumer's acquire and destroyed by the LAST consumer's
//        release, so stopping one slot never disturbs others in the group and
//        the encoder's owner need not be running.
// Audio: shares OBS's main audio mix. Each enabled track gets its own audio
//        encoder bound at the corresponding mixer index; all encoders attach
//        to the output at consecutive positions -> multi-track output file.
//        Audio encoders remain strictly per-slot.
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

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    bool save_replay();

    const Config& config() const { return cfg_; }
    void update_config(const Config& c);

    // Hotkey lifecycle is owned by the caller (SlotManager), not by start/stop,
    // so the "toggle recording" hotkey works even while the slot is stopped.
    // Both calls are idempotent.
    void register_hotkeys();
    void unregister_hotkeys();

    // --- per-slot hotkey-binding persistence (ITEM A) ---------------------
    // obs_hotkey_register_*() only creates the live hotkey; the user's key
    // combination lives nowhere unless we explicitly save/restore it. These
    // make a register/unregister cycle (and a destroy+recreate across
    // save/load) preserve the binding.
    //
    // capture_hotkey_bindings(): snapshot the two LIVE hotkeys into the
    //   pending_* members. Used around an in-session unregister+register
    //   cycle (update_config); register_hotkeys() then re-applies them.
    // save_hotkey_bindings(d): write the two bindings into `d` under stable
    //   keys for durable (cross-restart / cross-reload) persistence. Uses the
    //   live hotkeys when registered, else any not-yet-applied pending_*.
    // set_pending_hotkey_bindings(rec,rep): take ownership of binding arrays
    //   read back from saved data so register_hotkeys() can apply them once
    //   the hotkeys are (re)created. nullptr => nothing to restore.
    void capture_hotkey_bindings();
    void save_hotkey_bindings(obs_data_t* d) const;
    void set_pending_hotkey_bindings(obs_data_array_t* rec,
                                     obs_data_array_t* rep);

    static void on_record_hotkey(void* data, obs_hotkey_id id,
                                 obs_hotkey_t* hotkey, bool pressed);
    static void on_save_hotkey(void* data, obs_hotkey_id id,
                               obs_hotkey_t* hotkey, bool pressed);

    // Output "stop" signal handlers. Fired by libobs when an output stops,
    // including external/error stops (disk full, encoder failure, ...).
    static void on_rec_output_stop(void* data, calldata_t* cd);
    static void on_replay_output_stop(void* data, calldata_t* cd);

private:
    bool setup_encoders();
    // Resolved rate-control passed in by start() before taking slot_mtx_, so
    // the replay-buffer memory-cap estimate uses the OWNER's effective values
    // (consumer reads route through SlotManager::effective_rate_control). The
    // helper takes mtx_ then shared_mtx_ independently; resolving before
    // slot_mtx_ keeps the global order mtx_ -> slot_mtx_ -> shared_mtx_.
    bool setup_outputs(const struct EffectiveRC& eff);
    // Public locking entry point: acquires slot_mtx_ then calls teardown_locked().
    void teardown();
    // Core teardown. Caller MUST already hold slot_mtx_ (e.g. start()'s
    // in-lock failure paths) so the non-recursive mutex is not re-entered.
    void teardown_locked();

    // Blocks until obs_output_active(out) is false (async stop completes),
    // up to a 5 s timeout; force-stops as a last resort.
    void wait_for_output_stop(obs_output_t* out);

    // Guards every mutable SceneSlot member below (outputs, audio encoders,
    // the borrowed shared-encoder pointer, group_key_, cfg_). Acquired by
    // start()/teardown()/stats()/save_replay()/update_config(). Lock order:
    // SlotManager::mtx_ -> SceneSlot::slot_mtx_ -> SceneSlot::stats_mtx_,
    // and (in start()/teardown_locked()) the strict leaf
    // SlotManager::shared_mtx_ is taken last of all.
    mutable std::mutex slot_mtx_;

    Config cfg_;
    std::atomic<bool> running_{false};

    // Local copy of the shared context's fallback flag, taken at start() so
    // stats() can surface "[CBR fallback]" for the owner AND every sharer
    // without touching the shared registry. Guarded by slot_mtx_.
    bool encoder_fallback_ = false;

    // Encoder-group key of the SharedEncoder this slot is currently
    // consuming (own id for an owner, referenced slot's id for a sharer).
    // Set in start(), used by teardown_locked() to release the right
    // context, cleared on teardown. Guarded by slot_mtx_.
    std::string group_key_;

    // Borrowed pointer to the shared context's encoder (a strong ref is held
    // on our behalf by SlotManager::acquire_shared_encoder; released via
    // release_shared_encoder). Used to attach to this slot's outputs. Non-null
    // only while this slot is an active consumer. Guarded by slot_mtx_.
    obs_encoder_t* venc_ = nullptr;
    std::vector<obs_encoder_t*> aencs_;   // one encoder per selected track
    // 0-based OBS track index for each entry in aencs_, in push order. Used
    // to attach each encoder at the matching output audio index.
    std::vector<int> selected_tracks_;

    obs_output_t* rec_out_    = nullptr;
    obs_output_t* replay_out_ = nullptr;

    obs_hotkey_id hotkey_record_ = OBS_INVALID_HOTKEY_ID;
    obs_hotkey_id hotkey_replay_ = OBS_INVALID_HOTKEY_ID;

    // Inert per-slot output that exists solely so OBS Settings > Hotkeys
    // groups the two hotkeys above under its name ("Multi-Scene Record: <slot
    // name>"). Created in register_hotkeys() via obs_output_create with type
    // "ffmpeg_muxer" and never started; released in unregister_hotkeys() after
    // both hotkey ids have been unregistered (the registered hotkeys hold a
    // weak ref to it). Destroy+recreate is the only way to refresh the label
    // on rename, since libobs has no obs_output_set_name.
    obs_output_t* hotkey_out_ = nullptr;

    // Owned key-binding arrays awaiting (re)application by register_hotkeys()
    // immediately after the matching obs_hotkey_register_* call. Populated
    // either by capture_hotkey_bindings() (in-session cycle) or by
    // set_pending_hotkey_bindings() (restored from saved data during a
    // load_from rebuild). null when nothing is pending. register_hotkeys()
    // obs_hotkey_load()s then release+nulls each; ~SceneSlot releases any
    // still-pending array so an unconsumed restore cannot leak.
    obs_data_array_t* pending_hk_record_ = nullptr;
    obs_data_array_t* pending_hk_replay_ = nullptr;

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

// Canonical, null-terminated list of single-key encoder property names that
// carry a quality-mode value (CRF / CQP / CQ / ICQ / Global Quality). The
// FIRST present key wins for both editor range introspection
// (introspect_quality_range) and the encoder-build write target
// (set_quality_value) — both call sites walk this list, so any future PR that
// adds a key to one side without the other becomes a compile-time follow-
// through to this single source.
const char* const* quality_keys();

// Null-terminated list of QSV "split" quality keys. Set as a unit when none of
// quality_keys() matched but any of these do (matches set_quality_value).
const char* const* quality_split_keys();

} // namespace rc_util
