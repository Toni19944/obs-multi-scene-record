#include "slot.hpp"
#include "manager.hpp"
#include "plugin-main.hpp"
#include "ui-dock.hpp"

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QMetaObject>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <sys/sysinfo.h>
#endif

// =============================================================================
// rate control helpers
// =============================================================================

namespace rc_util {

bool is_bitrate_based(const std::string &mode)
{
	// Bitrate-targeted modes across x264 / NVENC / AMF / QSV.
	static const char *bitrate_modes[] = {"CBR", "VBR", "ABR", "VBR_LAT", "VBR_HQ", "VBR_PEAK"};
	for (const char *m : bitrate_modes)
		if (mode == m)
			return true;
	return false; // CQP / CRF / CQ / ICQ / QVBR / Lossless -> not pure bitrate
}

bool is_lossless(const std::string &mode)
{
	return mode == "Lossless" || mode == "LOSSLESS" || mode == "lossless";
}

// Canonical match order — first present key wins for both editor range
// introspection and the encoder-build write target.
static constexpr const char *kQualityKeys[] = {"crf",  "cqp", "cq_level", "qp", "icq_quality", "global_quality",
					       nullptr};

// QSV split keys (CQP on QSV uses qpi/qpp/qpb as a unit). Walked only after
// kQualityKeys yielded no match.
static constexpr const char *kQualitySplitKeys[] = {"qpi", "qpp", "qpb", nullptr};

const char *const *quality_keys()
{
	return kQualityKeys;
}

const char *const *quality_split_keys()
{
	return kQualitySplitKeys;
}

} // namespace rc_util

// =============================================================================
// replay buffer sizing helpers
// =============================================================================

namespace replay_buffer_util {

static uint32_t popcount32(uint32_t v)
{
#if defined(_MSC_VER)
	return __popcnt(v);
#else
	return (uint32_t)__builtin_popcount(v);
#endif
}

uint64_t estimated_kbps(const SceneSlot::Config &cfg, const EffectiveRC &eff)
{
	double fps = cfg.fps_den > 0 ? (double)cfg.fps_num / cfg.fps_den : 60.0;
	double video_bps;
	if (rc_util::is_bitrate_based(eff.mode)) {
		video_bps = (double)eff.value * 1000.0;
	} else if (rc_util::is_lossless(eff.mode)) {
		video_bps = 8.0 * cfg.width * cfg.height * fps;
	} else {
		video_bps = 0.55 * cfg.width * cfg.height * fps;
	}
	double audio_bps = (double)cfg.audio_bitrate * popcount32(cfg.audio_tracks) * 1000.0;
	return (uint64_t)((video_bps + audio_bps) / 1000.0);
}

uint64_t auto_derived_max_size_mb(const SceneSlot::Config &cfg, const EffectiveRC &eff)
{
	return estimated_kbps(cfg, eff) * cfg.replay_seconds * 2 / (8 * 1024);
}

uint64_t available_physical_mb()
{
#ifdef _WIN32
	MEMORYSTATUSEX msex{};
	msex.dwLength = sizeof(msex);
	if (!GlobalMemoryStatusEx(&msex))
		return 0;
	return (uint64_t)msex.ullAvailPhys / (1024 * 1024);
#elif defined(__APPLE__)
	mach_port_t host = mach_host_self();
	vm_statistics64_data_t stats;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) != KERN_SUCCESS)
		return 0;
	uint64_t avail_bytes = ((uint64_t)stats.free_count + (uint64_t)stats.inactive_count) * vm_page_size;
	return avail_bytes / (1024 * 1024);
#else
	FILE *f = fopen("/proc/meminfo", "r");
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
	struct sysinfo si{};
	if (sysinfo(&si) != 0)
		return 0;
	return (uint64_t)si.freeram * si.mem_unit / (1024 * 1024);
#endif
}

uint64_t resolve_max_size_mb(const SceneSlot::Config &cfg, const EffectiveRC &eff, bool *out_was_clamped,
			     uint64_t *out_requested_mb, uint64_t *out_auto_mb, uint32_t *out_assumed_kbps)
{
	// O-003: compute the estimate chain ONCE and surface the intermediate
	// values callers previously recomputed (auto-derived MB, assumed kbps).
	// auto_mb uses the exact auto_derived_max_size_mb formula, so results
	// are byte-identical to the former triple computation.
	uint64_t est_kbps = estimated_kbps(cfg, eff);
	uint64_t auto_mb = est_kbps * cfg.replay_seconds * 2 / (8 * 1024);
	if (out_auto_mb)
		*out_auto_mb = auto_mb;
	if (out_assumed_kbps)
		*out_assumed_kbps = (uint32_t)est_kbps;
	uint64_t requested_mb = (cfg.replay_max_size_mb > 0) ? cfg.replay_max_size_mb : auto_mb;
	if (out_requested_mb)
		*out_requested_mb = requested_mb;

	uint64_t avail_mb = available_physical_mb();
	uint64_t clamped_mb;
	bool was_clamped;
	if (avail_mb > 0 && requested_mb > avail_mb / 2) {
		clamped_mb = avail_mb / 2;
		was_clamped = true;
	} else {
		clamped_mb = requested_mb;
		was_clamped = false;
	}
	if (out_was_clamped)
		*out_was_clamped = was_clamped;

	if (clamped_mb < 50)
		return 0;
	return clamped_mb;
}

} // namespace replay_buffer_util

// =============================================================================
// replay filename helpers
// =============================================================================

namespace replay_util {

std::string sanitize_for_filename(const std::string &name)
{
	std::string out;
	out.reserve(name.size());

	auto is_illegal = [](unsigned char c) -> bool {
		if (c <= 0x1F || c == 0x7F)
			return true;
		switch (c) {
		case '<':
		case '>':
		case ':':
		case '"':
		case '/':
		case '\\':
		case '|':
		case '?':
		case '*':
		case '%':
			return true;
		default:
			return false;
		}
	};

	// Replace illegal chars with '_' and collapse runs of '_'.
	for (char ch : name) {
		char repl = is_illegal((unsigned char)ch) ? '_' : ch;
		if (repl == '_' && !out.empty() && out.back() == '_')
			continue;
		out.push_back(repl);
	}

	// Strip leading {_, ., space}.
	size_t start = 0;
	while (start < out.size() && (out[start] == '_' || out[start] == '.' || out[start] == ' '))
		++start;
	// Strip trailing {_, ., space}.
	size_t end = out.size();
	while (end > start && (out[end - 1] == '_' || out[end - 1] == '.' || out[end - 1] == ' '))
		--end;
	out = out.substr(start, end - start);

	// Windows reserved device names: prepend '_' if case-folded match.
	if (!out.empty()) {
		std::string upper;
		upper.reserve(out.size());
		for (char ch : out)
			upper.push_back((char)std::toupper((unsigned char)ch));

		static const char *reserved_exact[] = {"CON", "PRN", "AUX", "NUL", nullptr};
		bool match = false;
		for (const char **r = reserved_exact; *r; ++r) {
			if (upper == *r) {
				match = true;
				break;
			}
		}
		if (!match && upper.size() == 4 &&
		    (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' &&
		    upper[3] <= '9') {
			match = true;
		}
		if (match)
			out.insert(out.begin(), '_');
	}

	return out;
}

std::string build_replay_format(const SceneSlot::Config &cfg)
{
	std::string name = sanitize_for_filename(cfg.name);
	if (name.empty())
		name = "slot";

	std::string id6;
	if (cfg.id.size() >= 6)
		id6 = cfg.id.substr(cfg.id.size() - 6);
	else
		id6 = cfg.id;

	std::string out;
	out.reserve(name.size() + 1 + id6.size() + 40);
	out += name;
	out += '_';
	out += id6;
	out += "_Replay_%CCYY-%MM-%DD_%hh-%mm-%ss";
	return out;
}

} // namespace replay_util

// =============================================================================
// helpers
// =============================================================================

namespace {

std::string generate_slot_id()
{
	static std::atomic<uint32_t> counter{0};
	uint64_t t = os_gettime_ns();
	uint32_t c = counter.fetch_add(1);
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%llx-%x", (unsigned long long)t, c);
	return buf;
}

obs_source_t *fetch_scene_source(const std::string &name)
{
	obs_source_t *s = obs_get_source_by_name(name.c_str());
	if (!s)
		return nullptr;
	if (obs_source_get_type(s) != OBS_SOURCE_TYPE_SCENE) {
		obs_source_release(s);
		return nullptr;
	}
	return s;
}

std::string build_output_filename(const SceneSlot::Config &cfg)
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

	std::string safe_name = replay_util::sanitize_for_filename(cfg.name);
	if (safe_name.empty())
		safe_name = "slot";

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

	const std::string ext = cfg.container.empty() ? std::string("mp4") : cfg.container;

	// F-007: the timestamp only has one-second resolution, so a stop+restart
	// inside the same second would silently overwrite the finished file.
	// Probe deterministically: base name first, then _1, _2, ... suffixes
	// before the extension until a free name is found (a pre-existing _N on
	// disk is simply skipped past). Bounded; the epoch-ns fallback can
	// realistically never collide, so nothing is ever overwritten.
	std::string candidate = path + '.' + ext;
	static constexpr int kMaxSuffixProbes = 1000;
	for (int i = 1; os_file_exists(candidate.c_str()) && i <= kMaxSuffixProbes; ++i)
		candidate = path + '_' + std::to_string(i) + '.' + ext;
	if (os_file_exists(candidate.c_str()))
		candidate = path + '_' + std::to_string((unsigned long long)os_gettime_ns()) + '.' + ext;
	return candidate;
}

// Write the quality value into whichever quality-named setting the encoder
// actually exposes — discovered by introspecting its properties. Setting a key
// the encoder doesn't have is avoided; this keeps us encoder-agnostic.
void set_quality_value(obs_data_t *settings, const char *enc_id, int value)
{
	obs_properties_t *props = obs_get_encoder_properties(enc_id);
	if (!props) {
		obs_data_set_int(settings, "cqp", value);
		return;
	}
	// First present key from the canonical list wins. The editor's range
	// introspection (introspect_quality_range in ui-slot-editor.cpp) walks
	// the same list — adding a key to one side without the other becomes a
	// compile-time follow-through to rc_util::quality_keys().
	for (const char *const *k = rc_util::quality_keys(); *k; ++k) {
		if (obs_properties_get(props, *k)) {
			obs_data_set_int(settings, *k, value);
			obs_properties_destroy(props);
			return;
		}
	}
	// QSV split keys: set every present split key as a unit.
	bool any_split = false;
	for (const char *const *k = rc_util::quality_split_keys(); *k; ++k)
		if (obs_properties_get(props, *k)) {
			obs_data_set_int(settings, *k, value);
			any_split = true;
		}
	if (!any_split)
		obs_data_set_int(settings, "cqp", value); // last-resort fallback
	obs_properties_destroy(props);
}

// Encoder-family presets that are orthogonal to rate control: speed preset,
// h264 profile, tuning, multipass, etc. Matched by ID substring so newer ID
// variants still resolve.
void apply_family_presets(obs_data_t *s, const std::string &enc_id, const SceneSlot::Config &cfg)
{
	obs_data_set_int(s, "keyint_sec", cfg.keyframe_interval_sec);

	auto has = [&](const char *needle) {
		return enc_id.find(needle) != std::string::npos;
	};

	// Resolves to `val` when non-empty, otherwise to `fallback`.
	// Preserves former hardcoded behavior for old saves that have empty strings.
	auto eff = [](const std::string &val, const char *fallback) -> const char * {
		return val.empty() ? fallback : val.c_str();
	};

	if (has("x264")) {
		obs_data_set_string(s, "preset", eff(cfg.encoder_preset, "veryfast"));
		obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));
		// tune: empty string is the valid x264 "no tune" value; set it directly.
		obs_data_set_string(s, "tune", cfg.encoder_tune.c_str());
		if (cfg.b_frames >= 0)
			obs_data_set_int(s, "bf", cfg.b_frames);

		if (cfg.advanced_settings) {
			obs_data_set_bool(s, "cabac", cfg.cabac);
			obs_data_set_bool(s, "mbtree", cfg.mbtree);
			if (!cfg.x264opts.empty())
				obs_data_set_string(s, "x264opts", cfg.x264opts.c_str());
			if (cfg.aq_mode >= 0)
				obs_data_set_int(s, "aq_mode", cfg.aq_mode);
			if (cfg.min_qp >= 0)
				obs_data_set_int(s, "qpmin", cfg.min_qp);
			if (cfg.max_qp >= 0)
				obs_data_set_int(s, "qpmax", cfg.max_qp);
		}
	} else if (has("nvenc")) {
		const char *eff_preset = eff(cfg.encoder_preset, "p5");
		obs_data_set_string(s, "preset", eff_preset);  // ffmpeg nvenc compat
		obs_data_set_string(s, "preset2", eff_preset); // jim_nvenc
		obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));
		obs_data_set_string(s, "tune", eff(cfg.encoder_tune, "hq"));
		obs_data_set_string(s, "multipass", eff(cfg.multipass, "qres"));
		obs_data_set_bool(s, "lookahead", cfg.lookahead);
		obs_data_set_bool(s, "psycho_aq", cfg.psycho_aq);
		if (cfg.b_frames >= 0)
			obs_data_set_int(s, "bf", cfg.b_frames);
		if (cfg.gpu_index >= 0)
			obs_data_set_int(s, "gpu", cfg.gpu_index);

		if (cfg.advanced_settings) {
			obs_data_set_bool(s, "repeat_headers", cfg.nvenc_repeat_headers);
			obs_data_set_bool(s, "force_idr", cfg.nvenc_force_idr);
			obs_data_set_bool(s, "dynamic_bitrate", cfg.nvenc_dyn_bitrate);
			if (cfg.min_qp >= 0)
				obs_data_set_int(s, "min_qp", cfg.min_qp);
			if (cfg.max_qp >= 0)
				obs_data_set_int(s, "max_qp", cfg.max_qp);
		}
	} else if (has("amf")) {
		obs_data_set_string(s, "preset", eff(cfg.encoder_preset, "quality"));
		obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));
		if (cfg.gpu_index >= 0)
			obs_data_set_int(s, "gpu", cfg.gpu_index);

		if (cfg.advanced_settings) {
			obs_data_set_bool(s, "enforce_hrd", cfg.amf_enforce_hrd);
			obs_data_set_bool(s, "vbaq", cfg.amf_vbaq);
			obs_data_set_bool(s, "preanalysis", cfg.amf_pre_analysis);
			obs_data_set_bool(s, "enable_throughput", cfg.amf_enable_throughput);
			if (cfg.min_qp >= 0)
				obs_data_set_int(s, "min_qp", cfg.min_qp);
			if (cfg.max_qp >= 0)
				obs_data_set_int(s, "max_qp", cfg.max_qp);
		}
	} else if (has("qsv")) {
		// QSV uses "target_usage" as its preset key.
		obs_data_set_string(s, "target_usage", eff(cfg.encoder_preset, "balanced"));
		obs_data_set_string(s, "profile", eff(cfg.encoder_profile, "high"));

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
				obs_data_set_int(s, "frames_before_start", cfg.vt_frames_before_start);
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
void apply_encoder_settings(obs_data_t *s, const std::string &enc_id, const SceneSlot::Config &cfg)
{
	apply_family_presets(s, enc_id, cfg);

	const std::string &rc = cfg.rate_control;
	obs_data_set_string(s, "rate_control", rc.c_str());

	if (rc_util::is_lossless(rc)) {
		// No numeric value for lossless.
	} else if (rc_util::is_bitrate_based(rc)) {
		obs_data_set_int(s, "bitrate", cfg.rc_value);
	} else {
		set_quality_value(s, enc_id.c_str(), (int)cfg.rc_value);
	}
}

} // anonymous namespace

// =============================================================================
// shared encoder context (scene/view/video/encoder pipeline)
// =============================================================================
//
// Relocated here (not duplicated): the former SceneSlot::setup_video() and the
// owner branch of SceneSlot::setup_encoders() now live in SharedEncoder::build,
// parameterized by the group-key slot's Config rather than the consuming
// slot's. fetch_scene_source / apply_encoder_settings / apply_family_presets /
// rc_util are reused from above.

bool SharedEncoder::build(const SceneSlot::Config &cfg)
{
	// (1) scene source + "showing" reference.
	scene_src_ = fetch_scene_source(cfg.scene_name);
	if (!scene_src_) {
		blog(LOG_WARNING, "[multi-scene-rec] shared encoder: scene '%s' not found", cfg.scene_name.c_str());
		return false;
	}
	obs_source_inc_showing(scene_src_);

	// (2) per-group view + video at the owner's resolution/fps.
	view_ = obs_view_create();
	obs_view_set_source(view_, 0, scene_src_);

	// Per-slot video info. Per-slot scene independence (the plugin's identity)
	// requires this dedicated video pipeline — see specs/003-perf-parity-audit
	// research D1. The cost is one extra compositing pass per group; that is
	// accepted as irreducible. We DO match every non-resolution / non-fps field
	// to the user's OBS main video info (output_format, scale_type,
	// gpu_conversion, colorspace, range) so we never impose an avoidable extra
	// conversion on top of that irreducible cost.
	struct obs_video_info main_ovi = {};
	obs_get_video_info(&main_ovi);

	struct obs_video_info ovi = {};
	ovi.fps_num = cfg.fps_num;
	ovi.fps_den = cfg.fps_den;
	ovi.base_width = cfg.width;
	ovi.base_height = cfg.height;
	ovi.output_width = cfg.width;
	ovi.output_height = cfg.height;
	ovi.output_format = main_ovi.output_format;
	ovi.colorspace = main_ovi.colorspace;
	ovi.range = main_ovi.range;
	ovi.gpu_conversion = main_ovi.gpu_conversion;
	ovi.scale_type = main_ovi.scale_type;

	video_ = obs_view_add2(view_, &ovi);
	if (!video_) {
		blog(LOG_ERROR, "[multi-scene-rec] shared encoder: obs_view_add2 failed for '%s'", cfg.name.c_str());
		return false; // dtor cleans view + scene
	}

	// (3) video encoder + x264/CBR fallback (identical path to the former
	//     owner branch of setup_encoders()), then bind to this group's video_t.
	const std::string enc_id = cfg.video_encoder_id.empty() ? std::string("obs_x264") : cfg.video_encoder_id;

	obs_data_t *vs = obs_data_create();
	apply_encoder_settings(vs, enc_id, cfg);
	venc_ = obs_video_encoder_create(enc_id.c_str(), ("venc_" + cfg.id).c_str(), vs, nullptr);
	obs_data_release(vs);

	if (!venc_) {
		// Fall back to x264 + CBR if the requested encoder is unavailable on
		// this machine. The saved rate control mode may not exist on x264
		// (e.g. CQP), so force CBR with a safe bitrate.
		blog(LOG_WARNING, "[multi-scene-rec] shared encoder '%s' unavailable, falling back to obs_x264/CBR",
		     enc_id.c_str());
		obs_data_t *fs = obs_data_create();
		apply_family_presets(fs, "obs_x264", cfg);
		obs_data_set_string(fs, "rate_control", "CBR");
		obs_data_set_int(fs, "bitrate", rc_util::is_bitrate_based(cfg.rate_control) ? cfg.rc_value : 6000);
		venc_ = obs_video_encoder_create("obs_x264", ("venc_" + cfg.id).c_str(), fs, nullptr);
		obs_data_release(fs);
		if (!venc_) {
			blog(LOG_ERROR, "[multi-scene-rec] shared video encoder create failed");
			return false; // dtor cleans view + scene
		}
		// The user's rate-control configuration was discarded; surface this
		// in the UI (see Stats::encoder_fallback) for owner and sharers.
		encoder_fallback_ = true;
	}
	obs_encoder_set_video(venc_, video_);
	return true;
}

SharedEncoder::~SharedEncoder()
{
	// Mandatory destroy order: encoder before view, view before scene. Also
	// serves as partial-build cleanup (any subset of handles may be null).

	// (1) Final encoder release, matching obs_video_encoder_create in build().
	//     All consumer refs (obs_encoder_get_ref) were released by
	//     release_shared_encoder before use_count reached 0; this drops the
	//     creation ref and destroys the encoder.
	if (venc_) {
		obs_encoder_release(venc_);
		venc_ = nullptr;
	}
	// (2) Detach and destroy the per-group view (clear its source first).
	if (view_) {
		obs_view_set_source(view_, 0, nullptr);
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
		video_ = nullptr;
	}
	// (3) Drop the scene "showing" reference, then release the scene source.
	if (scene_src_) {
		obs_source_dec_showing(scene_src_);
		obs_source_release(scene_src_);
		scene_src_ = nullptr;
	}
}

// =============================================================================
// ctor/dtor
// =============================================================================

SceneSlot::SceneSlot(Config cfg) : cfg_(std::move(cfg))
{
	if (cfg_.id.empty())
		cfg_.id = generate_slot_id();
}

SceneSlot::~SceneSlot()
{
	stop();
	unregister_hotkeys();
	// Release any binding arrays that were stashed (load_from restore) but
	// never consumed by register_hotkeys() before this slot was destroyed.
	if (pending_hk_record_)
		obs_data_array_release(pending_hk_record_);
	if (pending_hk_replay_)
		obs_data_array_release(pending_hk_replay_);
}

void SceneSlot::update_config(const Config &c)
{
	bool was_running = running_.load();
	bool had_hotkeys = (hotkey_record_ != OBS_INVALID_HOTKEY_ID);

	// stop() -> teardown() acquires slot_mtx_; must NOT be held here.
	if (was_running)
		stop();

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

	// The user's key combination is NOT preserved by a bare unregister+
	// register: obs_hotkey_register_*() creates a fresh, unbound hotkey.
	// Snapshot the two live bindings, cycle, then re-apply (register_hotkeys()
	// consumes the pending_* arrays right after re-registering). This keeps an
	// in-session rename/edit on the user's assigned keys.
	if (had_hotkeys) {
		capture_hotkey_bindings();
		unregister_hotkeys();
		register_hotkeys();
	}

	// The restart re-resolves the group key from the NEW cfg_ and re-acquires
	// the shared context inside start(). A sharing slot whose owner is not
	// running now restarts fine: start() builds the context from the owner's
	// persisted Config (the owner is never required to run).
	if (was_running)
		start();
}

void SceneSlot::set_enabled(bool enabled)
{
	// Feature 019 (research D4): flip ONLY the flag under slot_mtx_ — the
	// same lock start()'s Phase-1 gate reads it under, so the toggle is
	// race-free against a concurrent start. Deliberately no hotkey
	// re-registration or any other update_config side effect (FR-009).
	std::lock_guard<std::mutex> lk(slot_mtx_);
	cfg_.enabled = enabled;
}

// =============================================================================
// start / stop
// =============================================================================

bool SceneSlot::start()
{
	bool expected = false;
	if (!running_.compare_exchange_strong(expected, true))
		return true;

	// ---- Phase 1 (F-003, short slot_mtx_ hold): normalize cfg_ (written
	//      back so behavior/persistence is unchanged) and copy ONE coherent
	//      snapshot. Everything below reads the snapshot, never cfg_ — an
	//      update_config racing this start can no longer produce a torn mix
	//      of old and new values. The epoch tags this start attempt so the
	//      commit below can detect being superseded by a stop()+start()
	//      that ran while the pipeline was being built unlocked (F-008). ----
	Config snapshot;
	uint64_t my_epoch;
	{
		std::lock_guard<std::mutex> pre_lk(slot_mtx_);
		// Feature 019 (research D1): authoritative enabled gate. Every
		// start path — bulk button, per-slot hotkey, state-column click,
		// add_slot auto-start — funnels through here, so a disabled slot
		// can never start and never acquires any encoder/output/shared-
		// encoder resource (FR-004, FR-005).
		if (!cfg_.enabled) {
			blog(LOG_INFO, "[multi-scene-rec] '%s': start ignored — slot is disabled", cfg_.name.c_str());
			running_.store(false);
			return false;
		}
		if (cfg_.audio_tracks == 0) {
			blog(LOG_WARNING, "[multi-scene-rec] '%s': no audio tracks selected; defaulting to track 1",
			     cfg_.name.c_str());
			cfg_.audio_tracks = 0x01;
		}
		// replay-only is meaningless without the replay buffer; enforce it.
		if (cfg_.replay_only && !cfg_.replay_enabled) {
			blog(LOG_WARNING,
			     "[multi-scene-rec] '%s': replay-only set but replay disabled; enabling replay",
			     cfg_.name.c_str());
			cfg_.replay_enabled = true;
		}
		snapshot = cfg_;
		my_epoch = ++start_epoch_;
	}

	// ---- Resolve the encoder-group key and a COPY of the owner Config from
	//      the snapshot. For a sharing slot this looks up the referenced
	//      slot's Config via SlotManager (mtx_ taken briefly and released
	//      inside config_by_slot_id). Lock order is mtx_ -> slot_mtx_, and
	//      acquire_shared_encoder (shared_mtx_, leaf) must run with neither
	//      mtx_ nor slot_mtx_ held. ----
	std::string group_key;
	SceneSlot::Config owner_cfg;
	if (!snapshot.shared_encoder_slot_id.empty()) {
		group_key = snapshot.shared_encoder_slot_id;
		if (!SlotManager::instance().config_by_slot_id(group_key, owner_cfg)) {
			blog(LOG_ERROR, "[multi-scene-rec] '%s': encoder source slot '%s' not found",
			     snapshot.name.c_str(), group_key.c_str());
			running_.store(false);
			return false;
		}
	} else {
		group_key = snapshot.id;
		owner_cfg = snapshot;
	}

	// Validate before acquiring anything so these failures need no cleanup.
	if (snapshot.path.empty()) {
		blog(LOG_ERROR, "[multi-scene-rec] '%s': output path is empty", snapshot.name.c_str());
		running_.store(false);
		return false;
	}
	if (!os_file_exists(snapshot.path.c_str())) {
		blog(LOG_ERROR, "[multi-scene-rec] '%s': output path does not exist: %s", snapshot.name.c_str(),
		     snapshot.path.c_str());
		running_.store(false);
		return false;
	}

	// Acquire (or build) the shared encoder context for this group. Takes
	// shared_mtx_ ONLY (strict leaf): not under mtx_, not under slot_mtx_.
	SharedEncoder *shared = SlotManager::instance().acquire_shared_encoder(group_key, owner_cfg);
	if (!shared) {
		blog(LOG_ERROR, "[multi-scene-rec] '%s': could not acquire shared encoder (group '%s')",
		     snapshot.name.c_str(), group_key.c_str());
		running_.store(false);
		return false;
	}
	// Both fields are fixed for the context's lifetime and we hold a
	// consumption ref, so reading them without shared_mtx_ is safe (same
	// guarantee the previous under-slot_mtx_ read relied on).
	obs_encoder_t *shared_venc = shared->venc_;
	const bool shared_fallback = shared->encoder_fallback_;

	// Resolve effective rate-control with NO plugin lock held. The helper
	// internally takes mtx_ then shared_mtx_ (independently), so calling it
	// under slot_mtx_ would invert the global order mtx_ -> slot_mtx_.
	const EffectiveRC eff_rc = SlotManager::instance().effective_rate_control(snapshot);

	// ---- Phase 2 (F-008, UNLOCKED): build the whole pipeline into locals
	//      and start it. slot_mtx_ is NOT held across encoder/output
	//      construction or obs_output_start, so stats() and the replay-save
	//      hotkey never stall behind a slow disk or encoder. On failure the
	//      locals are torn down right here — the members were never touched,
	//      so concurrent readers only ever see "not started". ----
	std::vector<obs_encoder_t *> local_aencs;
	std::vector<int> local_tracks;
	obs_output_t *local_rec = nullptr;
	obs_output_t *local_replay = nullptr;
	ReplaySizing sizing;
	uint64_t started_ns = 0;

	bool ok = setup_encoders(snapshot, local_aencs, local_tracks);
	if (ok && !shared_venc) {
		blog(LOG_ERROR, "[multi-scene-rec] '%s': no video encoder available", snapshot.name.c_str());
		ok = false;
	}
	if (ok)
		ok = setup_outputs(snapshot, eff_rc, shared_venc, local_aencs, &local_rec, &local_replay, &sizing);

	if (ok) {
		started_ns = os_gettime_ns();
		// Recording output is absent in replay-only mode.
		if (local_rec && !obs_output_start(local_rec)) {
			blog(LOG_ERROR, "[multi-scene-rec] '%s' rec start failed: %s", snapshot.name.c_str(),
			     obs_output_get_last_error(local_rec));
			ok = false;
		}
	}
	if (ok && local_replay && !obs_output_start(local_replay)) {
		blog(LOG_WARNING, "[multi-scene-rec] '%s' replay start failed: %s", snapshot.name.c_str(),
		     obs_output_get_last_error(local_replay));
		obs_output_release(local_replay);
		local_replay = nullptr;
		if (snapshot.replay_only) {
			// The replay buffer was the only output -- nothing left to run.
			blog(LOG_ERROR, "[multi-scene-rec] '%s' replay-only but replay failed to start",
			     snapshot.name.c_str());
			ok = false;
		}
	}

	// ---- Phase 3 (short slot_mtx_ re-acquire): commit the complete
	//      pipeline into the members. The epoch check catches a stop() (or
	//      stop()+start()) that ran during the unlocked build: in that case
	//      this attempt is stale and must discard its locals instead of
	//      committing a ghost pipeline over the superseding owner's state.
	//      running_ is left alone on that path — the superseding start()
	//      owns it now. ----
	bool committed = false;
	if (ok) {
		std::lock_guard<std::mutex> lk(slot_mtx_);
		if (start_epoch_ == my_epoch && running_.load()) {
			group_key_ = group_key;
			venc_ = shared_venc;
			encoder_fallback_ = shared_fallback; // local copy for stats()
			aencs_ = std::move(local_aencs);
			selected_tracks_ = std::move(local_tracks);
			rec_out_ = local_rec;
			replay_out_ = local_replay;
			start_time_ns_.store(started_ns, std::memory_order_release);
			resolved_max_size_mb_.store(sizing.resolved_mb, std::memory_order_release);
			was_clamped_at_start_.store(sizing.was_clamped, std::memory_order_release);
			replay_seconds_at_start_.store(sizing.replay_seconds, std::memory_order_release);
			assumed_kbps_at_start_.store(sizing.assumed_kbps, std::memory_order_release);
			committed = true;
		}
	}

	if (!committed) {
		// Failure or superseded: tear the locals down directly. Mirrors
		// teardown()'s order (disconnect, stop, wait, release) without
		// touching any member — readers never saw this pipeline.
		if (local_rec) {
			signal_handler_disconnect(obs_output_get_signal_handler(local_rec), "stop",
						  &SceneSlot::on_rec_output_stop, hotkey_handle_);
			obs_output_stop(local_rec);
			wait_for_output_stop(local_rec, snapshot.name);
			obs_output_release(local_rec);
		}
		if (local_replay) {
			signal_handler_disconnect(obs_output_get_signal_handler(local_replay), "saved",
						  &SceneSlot::on_replay_saved, hotkey_handle_);
			signal_handler_disconnect(obs_output_get_signal_handler(local_replay), "stop",
						  &SceneSlot::on_replay_output_stop, hotkey_handle_);
			obs_output_stop(local_replay);
			wait_for_output_stop(local_replay, snapshot.name);
			obs_output_release(local_replay);
		}
		for (auto *aenc : local_aencs)
			if (aenc)
				obs_encoder_release(aenc);
		SlotManager::instance().release_shared_encoder(group_key);
		if (!ok)
			running_.store(false);
		return false;
	}

	// F-S1: no running_.store(true) here -- the CAS at the top of start()
	// already set running_=true, and every failure path between resets it
	// to false (or leaves it to the superseding start) before this point.

	char tracks_str[16] = {0};
	int pos = 0;
	for (int i = 0; i < 6; ++i)
		if (snapshot.audio_tracks & (1u << i))
			pos += std::snprintf(tracks_str + pos, sizeof(tracks_str) - pos, "%s%d", pos ? "," : "", i + 1);

	// Rate-control segment: Lossless prints no numeric value; bitrate/quality
	// modes print "<mode>/<value>". A [CBR fallback] prefix surfaces the
	// owner's encoder construction having fallen back to obs_x264/CBR.
	char rc_buf[96];
	if (rc_util::is_lossless(eff_rc.mode))
		std::snprintf(rc_buf, sizeof(rc_buf), "%sLossless", eff_rc.fallback ? "[CBR fallback] " : "");
	else
		std::snprintf(rc_buf, sizeof(rc_buf), "%s%s/%u", eff_rc.fallback ? "[CBR fallback] " : "",
			      eff_rc.mode.c_str(), eff_rc.value);

	const bool inherited = !eff_rc.owner_slot_name.empty();
	blog(LOG_INFO, "[multi-scene-rec] '%s' started (%ux%u@%u, %s, tracks=%s, %s)%s%s%s", snapshot.name.c_str(),
	     snapshot.width, snapshot.height, snapshot.fps_num, rc_buf, tracks_str,
	     snapshot.replay_only      ? "replay-only"
	     : snapshot.replay_enabled ? "rec+replay"
				       : "rec-only",
	     inherited ? " inherited from '" : "", inherited ? eff_rc.owner_slot_name.c_str() : "",
	     inherited ? "'" : "");
	return true;
}

void SceneSlot::stop()
{
	if (!running_.exchange(false))
		return;
	teardown();
}

void SceneSlot::teardown()
{
	obs_output_t *local_rec = nullptr;
	obs_output_t *local_replay = nullptr;
	std::string name;
	{
		std::lock_guard<std::mutex> lk(slot_mtx_);
		name = cfg_.name; // F-003: captured under the lock for the unlocked wait below
		if (rec_out_) {
			signal_handler_disconnect(obs_output_get_signal_handler(rec_out_), "stop",
						  &SceneSlot::on_rec_output_stop, hotkey_handle_);
			obs_output_stop(rec_out_);
		}
		if (replay_out_) {
			signal_handler_disconnect(obs_output_get_signal_handler(replay_out_), "saved",
						  &SceneSlot::on_replay_saved, hotkey_handle_);
			signal_handler_disconnect(obs_output_get_signal_handler(replay_out_), "stop",
						  &SceneSlot::on_replay_output_stop, hotkey_handle_);
			obs_output_stop(replay_out_);
		}
		local_rec = rec_out_;
		local_replay = replay_out_;
		rec_out_ = nullptr;
		replay_out_ = nullptr;
	}
	wait_for_output_stop(local_rec, name);
	wait_for_output_stop(local_replay, name);
	{
		std::lock_guard<std::mutex> lk(slot_mtx_);
		if (local_rec)
			obs_output_release(local_rec);
		if (local_replay)
			obs_output_release(local_replay);
		for (auto *aenc : aencs_)
			if (aenc)
				obs_encoder_release(aenc);
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

void SceneSlot::wait_for_output_stop(obs_output_t *out, const std::string &name)
{
	// F-003: the slot name is passed in (captured under slot_mtx_ by the
	// caller) because this runs without the lock — reading cfg_.name here
	// would race update_config.
	if (!out)
		return;
	static constexpr int kStopTimeoutMs = 5000;
	static constexpr int kStopPollMs = 10;
	const int max_iters = kStopTimeoutMs / kStopPollMs;
	int iters = 0;
	while (obs_output_active(out) && iters < max_iters) {
		os_sleep_ms(kStopPollMs);
		++iters;
	}
	if (obs_output_active(out)) {
		blog(LOG_WARNING, "[multi-scene-rec] '%s': output did not stop within 5s; forcing", name.c_str());
		obs_output_force_stop(out);
	}
}

// =============================================================================
// encoders (audio only — the video encoder lives in the shared context)
// =============================================================================

bool SceneSlot::setup_encoders(const Config &cfg, std::vector<obs_encoder_t *> &aencs, std::vector<int> &tracks)
{
	// The video encoder belongs to the SharedEncoder context acquired in
	// start(); this slot only builds its own per-slot audio encoders.
	// F-008: runs UNLOCKED on start()'s config snapshot, building into the
	// caller's locals — members are only touched by start()'s commit phase.
	// A mid-loop failure leaves the encoders created so far in `aencs`;
	// the caller's failure path releases them.

	static std::atomic<uint64_t> encoder_epoch{0};
	uint64_t epoch = encoder_epoch.fetch_add(1);

	// ---- audio encoders: one per selected OBS track ----
	audio_t *main_audio = obs_get_audio();
	if (!main_audio) {
		blog(LOG_ERROR, "[multi-scene-rec] obs_get_audio() returned null");
		return false;
	}

	aencs.clear();
	tracks.clear();
	for (int track = 0; track < 6; ++track) {
		if ((cfg.audio_tracks & (1u << track)) == 0)
			continue;

		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", cfg.audio_bitrate);

		std::string nm = "aenc_" + cfg.id + "_e" + std::to_string(epoch) + "_t" + std::to_string(track + 1);
		obs_encoder_t *aenc =
			obs_audio_encoder_create(cfg.audio_encoder_id.c_str(), nm.c_str(), as, (size_t)track, nullptr);
		obs_data_release(as);
		if (!aenc) {
			blog(LOG_ERROR, "[multi-scene-rec] audio encoder create failed for track %d", track + 1);
			return false;
		}
		blog(LOG_DEBUG, "[multi-scene-rec] '%s': created audio encoder '%s' (epoch %llu, track %d)",
		     cfg.name.c_str(), nm.c_str(), (unsigned long long)epoch, track + 1);
		obs_encoder_set_audio(aenc, main_audio);
		aencs.push_back(aenc);
		tracks.push_back(track); // 0-based OBS track index
	}
	if (aencs.empty()) {
		blog(LOG_ERROR, "[multi-scene-rec] no audio encoders created");
		return false;
	}
	return true;
}

// =============================================================================
// outputs
// =============================================================================

bool SceneSlot::setup_outputs(const Config &cfg, const EffectiveRC &eff, obs_encoder_t *venc,
			      const std::vector<obs_encoder_t *> &aencs, obs_output_t **out_rec,
			      obs_output_t **out_replay, ReplaySizing *sizing)
{
	// F-008: runs UNLOCKED on start()'s config snapshot. Outputs are handed
	// back through out_rec/out_replay (assigned as soon as they exist so the
	// caller's failure path can release them); replay sizing telemetry goes
	// through `sizing` and reaches the atomics only in start()'s commit.
	obs_output_t *rec = nullptr;
	obs_output_t *replay = nullptr;

	// ---- recording (ffmpeg_muxer) -- skipped entirely in replay-only mode ----
	if (!cfg.replay_only) {
		obs_data_t *rs = obs_data_create();
		obs_data_set_string(rs, "path", build_output_filename(cfg).c_str());
		obs_data_set_string(rs, "muxer_settings", "");

		rec = obs_output_create("ffmpeg_muxer", ("rec_out_" + cfg.id).c_str(), rs, nullptr);
		obs_data_release(rs);
		if (!rec) {
			blog(LOG_ERROR, "[multi-scene-rec] rec output create failed");
			return false;
		}
		*out_rec = rec;
		obs_output_set_video_encoder(rec, venc);
		for (size_t i = 0; i < aencs.size(); ++i)
			obs_output_set_audio_encoder(rec, aencs[i], i);

		signal_handler_connect(obs_output_get_signal_handler(rec), "stop", &SceneSlot::on_rec_output_stop,
				       hotkey_handle_);
	}

	// ---- replay buffer (optional) ----
	if (cfg.replay_enabled) {
		bool was_clamped = false;
		uint64_t requested_mb = 0;
		uint64_t auto_mb = 0;
		uint32_t assumed_kbps = 0;
		// O-003: one call resolves the cap AND returns the auto-derived MB
		// and assumed kbps it computes internally, so the estimate chain
		// runs exactly once per start (values identical by construction).
		uint64_t resolved_mb = replay_buffer_util::resolve_max_size_mb(cfg, eff, &was_clamped, &requested_mb,
									       &auto_mb, &assumed_kbps);

		if (resolved_mb == 0) {
			blog(LOG_ERROR,
			     "[multi-scene-rec] '%s': replay buffer DECLINED — "
			     "even the clamped ceiling falls below the 50 MB defensive floor; "
			     "host has only %llu MB available physical memory. Slot will start "
			     "without a replay buffer. Remedies: set 'Max replay buffer size (MB)' "
			     "smaller, lower replay duration, lower quality, or switch to a "
			     "bitrate-based rate-control mode.",
			     cfg.name.c_str(), (unsigned long long)replay_buffer_util::available_physical_mb());
		} else {
			sizing->resolved_mb = resolved_mb;
			sizing->was_clamped = was_clamped;
			sizing->replay_seconds = cfg.replay_seconds;
			sizing->assumed_kbps = assumed_kbps;

			if (was_clamped) {
				blog(LOG_WARNING,
				     "[multi-scene-rec] '%s': replay buffer requested %llu MB "
				     "but clamped to %llu MB (host has %llu MB available). "
				     "Configured %u s replay duration will NOT be honored under "
				     "typical bitrate; clip will be shorter than configured. "
				     "Remedies: set 'Max replay buffer size (MB)' to a smaller "
				     "explicit value to suppress this warning, OR lower the "
				     "replay duration, OR lower the rate-control quality, OR "
				     "switch to a bitrate-based rate-control mode.",
				     cfg.name.c_str(), (unsigned long long)requested_mb,
				     (unsigned long long)resolved_mb,
				     (unsigned long long)replay_buffer_util::available_physical_mb(),
				     cfg.replay_seconds);
			} else {
				if (cfg.replay_max_size_mb == 0) {
					blog(LOG_INFO,
					     "[multi-scene-rec] '%s': replay buffer reserved %llu MB "
					     "(auto-derived from %ux%u@%ufps %s; assumes %u kbps total, "
					     "incl. %u kbps audio)",
					     cfg.name.c_str(), (unsigned long long)resolved_mb, cfg.width, cfg.height,
					     cfg.fps_den > 0 ? cfg.fps_num / cfg.fps_den : cfg.fps_num,
					     eff.mode.c_str(), assumed_kbps,
					     cfg.audio_bitrate * replay_buffer_util::popcount32(cfg.audio_tracks));
				} else {
					blog(LOG_INFO,
					     "[multi-scene-rec] '%s': replay buffer reserved %llu MB "
					     "(user override; auto-derived would have been %llu MB)",
					     cfg.name.c_str(), (unsigned long long)resolved_mb,
					     (unsigned long long)auto_mb);
				}
			}

			obs_data_t *rb = obs_data_create();
			obs_data_set_string(rb, "directory", cfg.path.c_str());
			obs_data_set_string(rb, "format", replay_util::build_replay_format(cfg).c_str());
			obs_data_set_string(rb, "extension", cfg.container.empty() ? "mp4" : cfg.container.c_str());
			obs_data_set_int(rb, "max_time_sec", cfg.replay_seconds);
			obs_data_set_int(rb, "max_size_mb", (long long)resolved_mb);

			replay = obs_output_create("replay_buffer", ("replay_out_" + cfg.id).c_str(), rb, nullptr);
			obs_data_release(rb);
			if (!replay) {
				blog(LOG_WARNING, "[multi-scene-rec] '%s' replay create failed", cfg.name.c_str());
			} else {
				*out_replay = replay;
				obs_output_set_video_encoder(replay, venc);
				for (size_t i = 0; i < aencs.size(); ++i)
					obs_output_set_audio_encoder(replay, aencs[i], i);

				signal_handler_connect(obs_output_get_signal_handler(replay), "stop",
						       &SceneSlot::on_replay_output_stop, hotkey_handle_);
				signal_handler_connect(obs_output_get_signal_handler(replay), "saved",
						       &SceneSlot::on_replay_saved, hotkey_handle_);

				if (cfg.container == "mp4" || cfg.container == "MP4") {
					blog(LOG_WARNING,
					     "[multi-scene-rec] '%s': MP4 replay buffer will be unrecoverable if OBS crashes before save. Prefer MKV.",
					     cfg.name.c_str());
				}
			}
		}
	}

	// At least one output must have been created.
	if (!rec && !replay) {
		blog(LOG_ERROR, "[multi-scene-rec] '%s' no outputs created", cfg.name.c_str());
		return false;
	}
	return true;
}

// =============================================================================
// hotkeys
// =============================================================================

void SceneSlot::register_hotkeys()
{
	if (hotkey_record_ != OBS_INVALID_HOTKEY_ID)
		return; // already registered

	// Per-slot inert sentinel output. Its sole purpose is to give the two
	// hotkeys below an obs_output_t* to register against, so OBS Settings >
	// Hotkeys groups both rows under its name ("Multi-Scene Record: <slot
	// name>"). Type is "ffmpeg_muxer" because this plugin already uses it
	// elsewhere (guaranteed present in every supported OBS build); we never
	// call obs_output_start on it, so its real semantics are irrelevant.
	//
	// libobs has no obs_output_set_name, so updating the group label after a
	// slot rename requires destroying + recreating this output. update_config()
	// already pairs every rename with unregister_hotkeys() -> register_hotkeys();
	// since unregister_hotkeys() releases hotkey_out_, the rename naturally
	// refreshes the label here.
	std::string group_name = "Multi-Scene Record: " + cfg_.name;
	hotkey_out_ = obs_output_create("ffmpeg_muxer", group_name.c_str(), nullptr, nullptr);

	std::string rec_name = "multi_scene_rec.record." + cfg_.id;
	std::string rec_desc = "Toggle Recording: " + cfg_.name;
	std::string rep_name = "multi_scene_rec.save_replay." + cfg_.id;
	std::string rep_desc = "Save Replay: " + cfg_.name;

	hotkey_handle_ = new HotkeyHandle{shared_from_this()};

	if (hotkey_out_) {
		hotkey_record_ = obs_hotkey_register_output(hotkey_out_, rec_name.c_str(), rec_desc.c_str(),
							    &SceneSlot::on_record_hotkey, hotkey_handle_);
		hotkey_replay_ = obs_hotkey_register_output(hotkey_out_, rep_name.c_str(), rep_desc.c_str(),
							    &SceneSlot::on_save_hotkey, hotkey_handle_);
	} else {
		// Defensive fallback: if libobs refuses to create the sentinel output
		// (e.g. OOM, or the hotkey system isn't ready), keep the hotkeys
		// working by falling back to frontend registration. The user loses the
		// per-slot group label but not the hotkey itself.
		blog(LOG_WARNING,
		     "[multi-scene-rec] failed to create hotkey-group output for slot "
		     "'%s'; registering hotkeys under Front-End instead",
		     cfg_.name.c_str());
		hotkey_record_ = obs_hotkey_register_frontend(rec_name.c_str(), rec_desc.c_str(),
							      &SceneSlot::on_record_hotkey, hotkey_handle_);
		hotkey_replay_ = obs_hotkey_register_frontend(rep_name.c_str(), rep_desc.c_str(),
							      &SceneSlot::on_save_hotkey, hotkey_handle_);
	}

	// Re-apply any saved/captured binding to the now-live hotkey ids.
	// obs_hotkey_load() binds the live id (same path the Settings dialog uses
	// to apply a binding); the caller still owns the array and must release.
	// Mechanism-agnostic — works identically for output- and frontend-
	// registered hotkeys.
	if (pending_hk_record_) {
		if (hotkey_record_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_load(hotkey_record_, pending_hk_record_);
		obs_data_array_release(pending_hk_record_);
		pending_hk_record_ = nullptr;
	}
	if (pending_hk_replay_) {
		if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_load(hotkey_replay_, pending_hk_replay_);
		obs_data_array_release(pending_hk_replay_);
		pending_hk_replay_ = nullptr;
	}
}

// --- ITEM A: hotkey-binding persistence helpers ------------------------------

void SceneSlot::capture_hotkey_bindings()
{
	// Snapshot the live hotkeys into pending_*. obs_hotkey_save() returns a
	// new array the caller must release; release any stale pending first so
	// re-entry cannot leak.
	if (pending_hk_record_) {
		obs_data_array_release(pending_hk_record_);
		pending_hk_record_ = nullptr;
	}
	if (pending_hk_replay_) {
		obs_data_array_release(pending_hk_replay_);
		pending_hk_replay_ = nullptr;
	}
	if (hotkey_record_ != OBS_INVALID_HOTKEY_ID)
		pending_hk_record_ = obs_hotkey_save(hotkey_record_);
	if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID)
		pending_hk_replay_ = obs_hotkey_save(hotkey_replay_);
}

void SceneSlot::save_hotkey_bindings(obs_data_t *d) const
{
	// Prefer the live hotkey's current binding; if it is not registered yet
	// (e.g. a save that lands between load_from and FINISHED_LOADING) fall
	// back to the not-yet-applied pending_* array so the binding is never
	// dropped from the durable save. obs_data_set_array does not take
	// ownership, so release every array we obtained.
	if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *a = obs_hotkey_save(hotkey_record_);
		if (a) {
			obs_data_set_array(d, "hk_record", a);
			obs_data_array_release(a);
		}
	} else if (pending_hk_record_) {
		obs_data_set_array(d, "hk_record", pending_hk_record_);
	}
	if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *a = obs_hotkey_save(hotkey_replay_);
		if (a) {
			obs_data_set_array(d, "hk_save_replay", a);
			obs_data_array_release(a);
		}
	} else if (pending_hk_replay_) {
		obs_data_set_array(d, "hk_save_replay", pending_hk_replay_);
	}
}

void SceneSlot::set_pending_hotkey_bindings(obs_data_array_t *rec, obs_data_array_t *rep)
{
	// Takes ownership of rec/rep (each a ref from obs_data_get_array, or
	// nullptr when the key was absent in an older save). Drop any prior
	// pending first so a repeated stash cannot leak.
	if (pending_hk_record_)
		obs_data_array_release(pending_hk_record_);
	if (pending_hk_replay_)
		obs_data_array_release(pending_hk_replay_);
	pending_hk_record_ = rec;
	pending_hk_replay_ = rep;
}

void SceneSlot::unregister_hotkeys()
{
	// Order matters: each registered hotkey holds a weak ref to hotkey_out_
	// (via libobs's hotkey registry). Unregister both before dropping the
	// strong ref so the weak refs never dangle.
	if (hotkey_record_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(hotkey_record_);
		hotkey_record_ = OBS_INVALID_HOTKEY_ID;
	}
	if (hotkey_replay_ != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(hotkey_replay_);
		hotkey_replay_ = OBS_INVALID_HOTKEY_ID;
	}
	if (hotkey_out_) {
		obs_output_release(hotkey_out_);
		hotkey_out_ = nullptr;
	}
	delete hotkey_handle_;
	hotkey_handle_ = nullptr;
}

void SceneSlot::on_record_hotkey(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hk*/, bool pressed)
{
	if (!pressed)
		return;
	auto *h = static_cast<HotkeyHandle *>(data);
	if (!h)
		return;
	auto sp = h->wp.lock();
	if (!sp)
		return;
	if (sp->is_running()) {
		// F-002: stop() can block for hundreds of ms (wait_for_output_stop
		// flush) and this callback runs on libobs's hotkey thread, where a
		// long stall delays EVERY hotkey in OBS. Defer to the UI task queue
		// with a shared_ptr keep-alive — mirrors the external-stop deferral
		// in on_rec_output_stop. If both this and an external stop queue a
		// stop for the same slot, the second stop() is a no-op via the
		// running_.exchange(false) guard.
		auto *prevent_destroy = new std::shared_ptr<SceneSlot>(sp);
		obs_queue_task(
			OBS_TASK_UI,
			[](void *d) {
				auto *prevent = static_cast<std::shared_ptr<SceneSlot> *>(d);
				(*prevent)->stop();
				if (auto *dock = get_dock())
					dock->refresh();
				delete prevent;
			},
			prevent_destroy, false);
	} else {
		// Start stays on the hotkey thread (F-008 made start() cheap under
		// its lock); the dock refresh is queued via the F-006-safe helper
		// instead of reading g_dock from this non-UI thread.
		sp->start();
		notify_dock_refresh();
	}
}

void SceneSlot::on_rec_output_stop(void *data, calldata_t *cd)
{
	auto *h = static_cast<HotkeyHandle *>(data);
	if (!h)
		return;
	auto sp = h->wp.lock();
	if (!sp)
		return;
	long long code = calldata_int(cd, "code");
	if (code == OBS_OUTPUT_SUCCESS)
		return;

	blog(LOG_WARNING, "[multi-scene-rec] '%s': recording output stopped externally (code %lld)",
	     sp->config().name.c_str(), code);
	auto *prevent_destroy = new std::shared_ptr<SceneSlot>(sp);
	obs_queue_task(
		OBS_TASK_UI,
		[](void *d) {
			auto *prevent = static_cast<std::shared_ptr<SceneSlot> *>(d);
			(*prevent)->stop();
			if (auto *dock = get_dock())
				dock->refresh();
			delete prevent;
		},
		prevent_destroy, false);
}

void SceneSlot::on_replay_output_stop(void *data, calldata_t *cd)
{
	auto *h = static_cast<HotkeyHandle *>(data);
	if (!h)
		return;
	auto sp = h->wp.lock();
	if (!sp)
		return;
	long long code = calldata_int(cd, "code");
	if (code == OBS_OUTPUT_SUCCESS)
		return;

	blog(LOG_WARNING, "[multi-scene-rec] '%s': replay output stopped externally (code %lld)",
	     sp->config().name.c_str(), code);
	auto *prevent_destroy = new std::shared_ptr<SceneSlot>(sp);
	obs_queue_task(
		OBS_TASK_UI,
		[](void *d) {
			auto *prevent = static_cast<std::shared_ptr<SceneSlot> *>(d);
			(*prevent)->stop();
			if (auto *dock = get_dock())
				dock->refresh();
			delete prevent;
		},
		prevent_destroy, false);
}

void SceneSlot::on_replay_saved(void *data, calldata_t * /*cd*/)
{
	auto *h = static_cast<HotkeyHandle *>(data);
	if (!h)
		return;
	auto sp = h->wp.lock();
	if (!sp)
		return;
	sp->log_replay_saved();
}

void SceneSlot::log_replay_saved()
{
	// F-001: hold our own strong ref across the proc-handler query so a
	// concurrent teardown cannot free the output between the member copy
	// and the call below. Null ref (never started, or mid-destruction) ->
	// existing '<unknown>' path.
	std::string name;
	obs_output_t *replay = nullptr;
	{
		std::lock_guard<std::mutex> lk(slot_mtx_);
		name = cfg_.name;
		replay = obs_output_get_ref(replay_out_);
	}
	if (!replay) {
		blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '<unknown>'", name.c_str());
		return;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(replay);
	if (!ph) {
		blog(LOG_INFO, "[multi-scene-rec] '%s' replay save wrote '<unknown>'", name.c_str());
		obs_output_release(replay);
		return;
	}
	calldata_t cd;
	calldata_init(&cd);
	proc_handler_call(ph, "get_last_replay", &cd);
	const char *path = calldata_string(&cd, "path");
	obs_output_release(replay);

	uint64_t start_ns = start_time_ns_.load(std::memory_order_acquire);
	uint64_t uptime_sec = start_ns ? (os_gettime_ns() - start_ns) / 1000000000ULL : 0;
	uint64_t resolved_mb = resolved_max_size_mb_.load(std::memory_order_acquire);
	bool was_clamped = was_clamped_at_start_.load(std::memory_order_acquire);
	uint32_t replay_seconds = replay_seconds_at_start_.load(std::memory_order_acquire);
	uint32_t assumed_kbps = assumed_kbps_at_start_.load(std::memory_order_acquire);

	double ewma_kbps = 0.0;
	{
		std::lock_guard<std::mutex> lk(stats_mtx_);
		ewma_kbps = observed_kbps_ewma_;
	}

	if (ewma_kbps > 0.0) {
		blog(LOG_INFO,
		     "[multi-scene-rec] '%s' replay save wrote '%s' "
		     "(observed %.0f Mbps, assumed %.0f Mbps)",
		     name.c_str(), (path && *path) ? path : "<unknown>", ewma_kbps / 1000.0, assumed_kbps / 1000.0);
	} else {
		blog(LOG_INFO,
		     "[multi-scene-rec] '%s' replay save wrote '%s' "
		     "(observed N/A, assumed %.0f Mbps)",
		     name.c_str(), (path && *path) ? path : "<unknown>", assumed_kbps / 1000.0);
	}

	uint64_t needed_mb = (uint64_t)(ewma_kbps * replay_seconds / 8 / 1024);

	if (uptime_sec >= replay_seconds && needed_mb > resolved_mb && !was_clamped && ewma_kbps > 0.0) {
		blog(LOG_WARNING,
		     "[multi-scene-rec] '%s' replay save likely truncated to "
		     "less than configured %u s: observed %.0f Mbps suggests buffer needed "
		     "~%llu MB but resolved cap is %llu MB (auto-derived assumed %.0f Mbps); "
		     "suspected memory cap (cause not directly confirmed). "
		     "Consider setting 'Max replay buffer size (MB)' override, lowering "
		     "replay duration, or lowering quality.",
		     name.c_str(), replay_seconds, ewma_kbps / 1000.0, (unsigned long long)needed_mb,
		     (unsigned long long)resolved_mb, assumed_kbps / 1000.0);
	} else if (start_ns != 0 && uptime_sec < replay_seconds) {
		blog(LOG_INFO,
		     "[multi-scene-rec] '%s' note: slot uptime %llu s < configured "
		     "replay %u s; saved file will be shorter than configured (this is "
		     "expected \xe2\x80\x94 buffer hadn't filled).",
		     name.c_str(), (unsigned long long)uptime_sec, replay_seconds);
	}

	calldata_free(&cd);
}

void SceneSlot::on_save_hotkey(void *data, obs_hotkey_id /*id*/, obs_hotkey_t * /*hk*/, bool pressed)
{
	if (!pressed)
		return;
	auto *h = static_cast<HotkeyHandle *>(data);
	if (!h)
		return;
	auto sp = h->wp.lock();
	if (sp)
		sp->save_replay();
}

bool SceneSlot::save_replay()
{
	// slot_mtx_ guards replay_out_ against concurrent teardown.
	std::lock_guard<std::mutex> lk(slot_mtx_);
	if (!replay_out_)
		return false;
	proc_handler_t *ph = obs_output_get_proc_handler(replay_out_);
	if (!ph)
		return false;
	calldata_t cd;
	calldata_init(&cd);
	bool ok = proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	if (ok) {
		// Neutral wording: the proc dispatched, but the on-disk write
		// happens asynchronously on the OBS mux thread. The truthful
		// success line ("replay save wrote '<path>'") is emitted from
		// on_replay_saved only after the mux thread confirms the write.
		blog(LOG_INFO, "[multi-scene-rec] '%s' replay save requested", cfg_.name.c_str());
	} else {
		blog(LOG_WARNING, "[multi-scene-rec] '%s' replay save proc-dispatch FAILED (slot not capturing?)",
		     cfg_.name.c_str());
	}
	return ok;
}

// =============================================================================
// stats
// =============================================================================

SceneSlot::Stats SceneSlot::stats()
{
	Stats out;
	if (!running_.load())
		return out;

	// F-001: take our own strong refs under slot_mtx_, then query OUTSIDE the
	// lock on the ref'd pointers. A concurrent teardown can release the
	// members at any time after we unlock, but it can no longer free the
	// objects out from under the queries below.
	obs_output_t *primary = nullptr;
	obs_output_t *replay = nullptr;
	bool fallback = false;
	{
		std::lock_guard<std::mutex> lk(slot_mtx_);
		primary = obs_output_get_ref(rec_out_ ? rec_out_ : replay_out_);
		replay = obs_output_get_ref(replay_out_);
		fallback = encoder_fallback_;
	}
	if (!primary) {
		if (replay)
			obs_output_release(replay);
		return out;
	}

	out.active = obs_output_active(primary);
	out.replay_active = replay ? obs_output_active(replay) : false;
	out.total_frames = obs_output_get_total_frames(primary);
	out.dropped_frames = obs_output_get_frames_dropped(primary);
	out.total_bytes = obs_output_get_total_bytes(primary);
	out.encoder_fallback = fallback;

	if (replay)
		obs_output_release(replay);
	obs_output_release(primary);

	uint64_t now_ns = os_gettime_ns();
	std::lock_guard<std::mutex> slk(stats_mtx_);
	if (last_sample_bytes_ns_ != 0) {
		uint64_t dt_ns = now_ns - last_sample_bytes_ns_;
		uint64_t dbytes = out.total_bytes >= last_sample_bytes_ ? out.total_bytes - last_sample_bytes_ : 0;
		if (dt_ns > 0) {
			double bits_per_sec = (double)dbytes * 8.0 * (1e9 / (double)dt_ns);
			last_kbps_ = bits_per_sec / 1000.0;
		}
	}
	last_sample_bytes_ns_ = now_ns;
	last_sample_bytes_ = out.total_bytes;
	out.kbps = last_kbps_;

	constexpr double alpha = 0.25;
	if (observed_kbps_ewma_ == 0.0)
		observed_kbps_ewma_ = last_kbps_;
	else
		observed_kbps_ewma_ = alpha * last_kbps_ + (1.0 - alpha) * observed_kbps_ewma_;

	return out;
}

void SceneSlot::reset_stats_sampler()
{
	std::lock_guard<std::mutex> lk(stats_mtx_);
	last_sample_bytes_ns_ = 0;
	last_sample_bytes_ = 0;
	last_kbps_ = 0.0;
	observed_kbps_ewma_ = 0.0;
}
