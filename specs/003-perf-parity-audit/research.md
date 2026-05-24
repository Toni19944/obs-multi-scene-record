# Phase 0 Research: Performance parity with OBS native recording

This document is the audit deliverable for User Story 3 (P3) and Functional Requirements FR-008 / FR-009. It catalogues every architectural difference identified between this plugin's recording pipeline and OBS Studio native's recording pipeline (reference: `D:\Programs\Tools\obs-dev-kit\obs-studio`), classifies each with a disposition, and cites the file/line locations on both sides.

**Audit reference versions**: OBS Studio source tree at `D:\Programs\Tools\obs-dev-kit\obs-studio`. The plugin targets OBS 31.1.1 per `buildspec.json`. If a future OBS upgrade changes the recording-pipeline surface, this document MUST be revalidated.

**Disposition legend** (per spec FR-009):

- **CLOSE** — the plugin is brought into alignment with OBS native in this feature.
- **ACCEPT (irreducible)** — closure is infeasible; the cited evidence is the libobs constraint, the conflicting requirement, or the empirical zero-contribution measurement.
- **KEEP / at parity** — no divergence to close; the plugin already matches OBS native.

---

## D1 — Per-group video pipeline (accepted as irreducible)

**Plugin** (`src/slot.cpp:255-321`, `SharedEncoder::build`):

```cpp
view_ = obs_view_create();
obs_view_set_source(view_, 0, scene_src_);

struct obs_video_info ovi = {};
ovi.fps_num = cfg.fps_num;
ovi.fps_den = cfg.fps_den;
ovi.base_width  = cfg.width;
ovi.base_height = cfg.height;
ovi.output_width  = cfg.width;
ovi.output_height = cfg.height;
ovi.output_format = VIDEO_FORMAT_NV12;
// ... colorspace, range from main_ovi ...
ovi.gpu_conversion = true;
ovi.scale_type     = OBS_SCALE_BICUBIC;

video_ = obs_view_add2(view_, &ovi);
// ...
obs_encoder_set_video(venc_, video_);
```

A new `obs_view_t` and a new `video_t` are created for every encoder group, **always** — even when the slot's scene/resolution/fps match OBS's main canvas exactly.

**OBS native** (`obs-studio/frontend/utility/SimpleOutput.cpp:576`):

```cpp
obs_encoder_set_video(videoRecording, obs_get_video());
```

And in `obs-studio/frontend/utility/AdvancedOutput.cpp:541`:

```cpp
obs_encoder_set_video(videoRecording, obs_get_video());
```

OBS native's recording always uses `obs_get_video()` — the program's existing video output. No per-output view is created. The recording shares the same rendered frames the program window is already producing for preview/streaming/the scene compositor.

The only place OBS native creates an `obs_view_t` in the output flow is for the **virtual camera** (`obs-studio/frontend/utility/BasicOutputHandler.cpp:253-259`):

```cpp
if (!virtualCamView && !typeIsProgram)
    virtualCamView = obs_view_create();

UpdateVirtualCamOutputSource();

if (!virtualCamVideo) {
    virtualCamVideo = typeIsProgram ? obs_get_video()
                                    : obs_view_add(virtualCamView);
    // ...
}
```

— and even there, only when the user explicitly chose to point the virtual camera at a **non-program** source. When the virtual cam mirrors the program, OBS uses `obs_get_video()` directly.

**Cost of the divergence**:

The plugin asks the GPU to composite the slot's scene independently — even when the same scene is also rendered for OBS's program — because the slot must continue rendering its configured scene regardless of program changes during the recording. The encoder gets independent pixels keyed to the slot's configuration; the cost is one extra compositing pass per shared-encoder group.

**Disposition**: **ACCEPT (irreducible)** — FR-009 disposition (b), explicitly the per-slot scene independence case.

**Why we are NOT closing this**: an earlier draft of this document proposed reusing `obs_get_video()` when the slot's scene matches the program scene at start time. That optimization was rejected by the user: the plugin's identity is per-slot scene independence — a slot configured to record `Scene_Game` must keep recording `Scene_Game` even if the user switches OBS program to `Scene_BRB` mid-recording. Using `obs_get_video()` would tie the slot to whatever the program is currently showing, which would silently capture `Scene_BRB` content in `Scene_Game`'s file the moment the user switched scenes. That is a worse outcome than the FPS hit. The per-group `obs_view_t + video_t` IS the mechanism that delivers per-slot independence; closing it deletes the feature.

**Evidence cited for FR-009 (b)**:

- Plugin design intent: `cfg_.scene_name` is configured per slot and persisted; users expect their slot to capture the scene they named, not "whatever program is showing now."
- Code path: `obs_view_set_source(view_, 0, scene_src_)` at `src/slot.cpp:267` is the line that anchors the slot's rendering to its own configured scene independent of program. Any optimization that bypasses this loses the independence.
- User direction (2026-05-19 session): "The plugin's whole identity is based on it being independent from OBS main stream/record output. I don't want to change that since that would fully defeat the purpose the plugin was made for in the first place. We just have to take the hit that individual slots will cause, and optimize performance where we can."

**Consequence for FPS measurement**: a single-slot plugin recording will measurably lag a single-slot OBS-native recording by approximately one extra GPU compositing pass per frame. This is institutional-memory information for the quickstart benchmark (record the delta so future regressions can be detected against this baseline) — not a pass/fail bar.

**What this feature DOES close** (the items below — D2 through D5 — are the audit's remaining actionable findings).

---

## D2 — Hardcoded video output format

**Plugin** (`src/slot.cpp:276`):

```cpp
ovi.output_format = VIDEO_FORMAT_NV12;
```

**OBS native**: format is whatever the user configured via Settings > Video; `obs_get_video()` returns the corresponding `video_t`. Common formats include NV12 (GPU-friendly, hardware encoders), I420 (CPU-friendly, x264), or others.

**Cost**: when OBS main is configured for a non-NV12 format, the plugin's per-group `video_t` produces NV12 anyway — this forces an extra GPU→GPU conversion that OBS native's path doesn't pay for the same setup. Real overhead, no per-slot-independence rationale.

**Disposition**: **CLOSE**.

**Fix**: replace the hardcoded `ovi.output_format = VIDEO_FORMAT_NV12;` with `ovi.output_format = main_ovi.output_format;`. One-line change inside `SharedEncoder::build`.

---

## D3 — Hardcoded `scale_type`

**Plugin** (`src/slot.cpp:282`):

```cpp
ovi.scale_type = OBS_SCALE_BICUBIC;
```

**OBS native**: scale type comes from the user's Settings > Video → "Downscale Filter" choice; `obs_get_video()` reflects it.

**Cost**: when the slot's `output_width/height` equals `base_width/height` there is no scaling and the scale_type setting is irrelevant. When scaling IS happening AND the user picked something other than Bicubic in Settings, the per-group path uses a different filter than OBS native would — primarily a quality/correctness difference, but worth aligning regardless.

**Disposition**: **CLOSE**.

**Fix**: use `main_ovi.scale_type` instead of hardcoded `OBS_SCALE_BICUBIC`. One-line change.

---

## D4 — Hardcoded `gpu_conversion`

**Plugin** (`src/slot.cpp:281`):

```cpp
ovi.gpu_conversion = true;
```

**OBS native**: comes from `main_ovi.gpu_conversion` (configured in Settings > Advanced).

**Cost**: `gpu_conversion = true` is the modern OBS default and almost always the right choice; users virtually never disable it. Zero functional cost in practice for nearly every user. Still worth aligning for correctness so the rare user who has it disabled in OBS Settings doesn't get a different behavior here.

**Disposition**: **CLOSE**. One-line change to use `main_ovi.gpu_conversion`.

---

## D5 — `obs_source_inc_showing` on the scene source

**Plugin** (`src/slot.cpp:263`):

```cpp
obs_source_inc_showing(scene_src_);
```

— matching `dec_showing` in the dtor at `:346`.

**OBS native**: inc_showing on a scene is driven by the program/preview/projector that's displaying it. When the slot's scene matches the program scene, OBS already inc_showing'd it; the plugin's additional inc_showing is redundant.

**Cost**: `inc_showing` is a reference count that toggles source-tree behavior (sub-sources start preparing/ticking when their parent's show count goes 0→1). In the matched-program case the show count is already > 0 so the additional increment is a refcount-only no-op; in the divergent-scene case (slot scene is NOT in program), this is what causes the scene to start rendering at all. Either way, zero per-frame cost.

**Disposition**: **KEEP**. Per-group view requires the scene to be "showing" in order to render into the view. Removing this would break the divergent-scene case (the most important case — it's WHY the plugin needs per-slot views). At parity overhead-wise.

---

## D6 — Per-slot audio encoders attached to main mix

**Plugin** (`src/slot.cpp:662-700`, `setup_encoders`):

```cpp
audio_t *main_audio = obs_get_audio();
// for each selected track:
obs_encoder_t *aenc = obs_audio_encoder_create(... track ...);
obs_encoder_set_audio(aenc, main_audio);
```

**OBS native** (`obs-studio/frontend/utility/SimpleOutput.cpp` and `AdvancedOutput.cpp`): same pattern. Audio encoders are created per track and bound to `obs_get_audio()`. For multi-track recordings (advanced output), one encoder per enabled track.

**Cost**: at parity. Both create one audio encoder per enabled mixer track; both attach to the shared main audio output.

**Disposition**: **KEEP / at parity**. No change.

---

## D7 — Output types (`ffmpeg_muxer`, `replay_buffer`)

**Plugin** (`src/slot.cpp:714`, `:756`):

```cpp
rec_out_    = obs_output_create("ffmpeg_muxer", ...);
replay_out_ = obs_output_create("replay_buffer", ...);
```

**OBS native** (`SimpleOutput.cpp`, `AdvancedOutput.cpp`): identical. `ffmpeg_muxer` for recordings, `replay_buffer` for replay.

**Disposition**: **KEEP / at parity**. No change.

---

## D8 — Per-slot output `stop` signal handling

**Plugin** (`src/slot.cpp:729-730`, `:768-769`):

```cpp
signal_handler_connect(obs_output_get_signal_handler(rec_out_),
                       "stop", &SceneSlot::on_rec_output_stop, this);
```

**OBS native**: uses the same `signal_handler_connect` mechanism for `start`/`stop`/`stopping`/`deactivate` signals on the recording output.

**Disposition**: **KEEP / at parity**. No change.

---

## D9 — Shared encoder context across slots

**Plugin** (`src/manager.cpp:215-273`, `acquire_shared_encoder` / `release_shared_encoder`): a slot can declare `shared_encoder_slot_id` and become a "sharer" of another slot's video pipeline. The `SharedEncoder` is refcounted; multiple sharing slots reuse the same `obs_view_t` + `video_t` + video encoder.

**OBS native**: has no equivalent multi-output sharing concept at the user level — each recording/streaming output owns its encoders.

**Disposition**: **KEEP** — this is a plugin-specific feature, not a divergence from native. It actually *amplifies* the benefit of D1's parity-mode fix: when two sharing slots both record what amounts to the program scene at main settings, BOTH slots get the no-extra-compositing benefit (the single shared `SharedEncoder` uses `obs_get_video()`). Cost analysis: at parity per group; no per-consumer overhead.

---

## D10 — Per-encoder x264/CBR fallback

**Plugin** (`src/slot.cpp:299-318`, inside `SharedEncoder::build`): if the requested encoder fails to create, falls back to `obs_x264` + CBR with a sane bitrate. Constitution Principle V mandates this.

**OBS native**: also has a similar fallback for hardware encoders (in `SimpleOutput.cpp` / `AdvancedOutput.cpp`, x264 used as fallback for NVENC/AMF/QSV unavailability).

**Disposition**: **KEEP / at parity**. No change.

---

## D11 — Per-slot scene independence (and its consequence: D1 is irreducible)

**Plugin**: by design, each slot can record an arbitrary scene independent of what OBS is currently showing in program — the per-slot scene name in `cfg_.scene_name` is fetched and rendered into the slot's own view.

**OBS native**: cannot do this in its recording flow at all. The recording output uses `obs_get_video()`, which is whatever the program is showing. To record a non-program scene, the user would have to switch program, record, switch back.

**Disposition**: **ACCEPT (irreducible)** — FR-009 disposition (b). This is the plugin's identity. D1 (per-group `obs_view_t + video_t`) is the mechanism that DELIVERS D11; the two are inseparable. See D1 for the full rationale.

**Scope note**: an FPS measurement under matched-scene single-slot conditions is still useful — recorded as institutional memory in the quickstart so a future regression that ADDS overhead beyond the current baseline can be flagged. The absolute number is not a pass/fail bar.

---

## Memory & leak analysis (FR-004 — FR-007)

This is an audit-by-inspection of the plugin's resource lifecycle; the live 4-hour test in [quickstart.md](./quickstart.md) is the definitive check.

**Refs to track**:

1. `scene_src_` — strong ref from `obs_get_source_by_name` / `fetch_scene_source` (`src/slot.cpp:258`). Released in `~SharedEncoder` (`:347`). Paired with `inc_showing`/`dec_showing` (`:263`, `:346`). ✓
2. `view_` — owned by `obs_view_create` (`:266`). Destroyed via `obs_view_set_source(NULL) + obs_view_remove + obs_view_destroy` in `~SharedEncoder` (`:337-342`). ✓
3. `video_` — borrowed from `obs_view_add2`; destroyed implicitly when the view is destroyed (per libobs docs). In parity mode this borrowing extends to `obs_get_video()` — also a borrowed pointer with no release required. ✓
4. `venc_` — strong ref from `obs_video_encoder_create` (`:296`/`:309`). Released in `~SharedEncoder` (`:333`). Consumer slots acquire additional refs via `obs_encoder_get_ref` (`src/manager.cpp:240`) and release them in `release_shared_encoder` (`:263`). Refcount model is symmetric. ✓
5. `aencs_[i]` — per-slot audio encoders from `obs_audio_encoder_create` (`src/slot.cpp:684`). Released in `SceneSlot::teardown_locked`. ✓
6. `rec_out_`, `replay_out_` — per-slot outputs from `obs_output_create` (`:714`/`:756`). Released in `teardown_locked`. ✓
7. `hotkey_out_` (per feature 001) — sentinel output. Released in `unregister_hotkeys`. ✓
8. `pending_hk_record_`, `pending_hk_replay_` — `obs_data_array_t*` arrays. Released either in `register_hotkeys` (consume + release) or in `~SceneSlot` (leftover release). ✓

**Signal handlers**: connected at output create, disconnected by output destruction (libobs cleans up signal connections when the source/output is destroyed). The frontend `obs_frontend_add_event_callback` and `obs_frontend_add_save_callback` registered in `SlotManager::init` are removed in `SlotManager::shutdown`. ✓

**Potential leak risk**: the only one I identified by inspection is in the `shared_` map — if `release_shared_encoder` is called for a `group_key` that doesn't exist (defensive null path at `manager.cpp:258`), it just returns. If a build fails partway, `acquire_shared_encoder` cleans up the partial context before returning null. The shutdown path scans `shared_` and logs a `LOG_ERROR` for any survivor — and erases them with the dtor running (per `manager.cpp:39-45`). No leak path identified.

**Disposition**: by-inspection audit finds no leak. **Action**: run the 4-hour quickstart memory test to confirm. If a live leak is found, this section will be amended.

---

## Items considered and ruled out

- **Color space / range mismatch**: plugin already reads `main_ovi.colorspace` and `main_ovi.range` and applies them to its per-group `ovi`. At parity for these two fields even in the per-group path. No change.
- **Audio resampling overhead**: per-slot audio encoders read the same `obs_get_audio()` output — no extra audio mixing. At parity.
- **Refresh stats 1 Hz poll**: reads encoder stats; doesn't render frames. Negligible CPU. Not an FPS factor.
- **Hotkey dispatch**: event-driven; zero per-frame cost. Already verified via features 001 and 002.
- **Replay buffer max size estimation**: a one-shot calculation at output create; not per-frame.
