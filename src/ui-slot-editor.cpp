#include "ui-slot-editor.hpp"
#include "manager.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QScrollArea>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QGroupBox>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QMessageBox>

#include <algorithm>

// =============================================================================
// introspection helpers
// =============================================================================

namespace {

struct IntRange {
    bool found = false;
    int  min = 0, max = 0, step = 1;
};

// Walk an encoder type's properties for an int property's range.
IntRange introspect_int_range(obs_properties_t* props, const char* key)
{
    IntRange r;
    obs_property_t* p = obs_properties_get(props, key);
    if (p && obs_property_get_type(p) == OBS_PROPERTY_INT) {
        r.found = true;
        r.min  = obs_property_int_min(p);
        r.max  = obs_property_int_max(p);
        r.step = obs_property_int_step(p);
        if (r.step <= 0) r.step = 1;
    }
    return r;
}

// First quality-value property the encoder actually exposes, with its range.
// Returns {found=false} if the encoder has no recognised quality key.
// Walks rc_util::quality_keys() then quality_split_keys() — the same lists
// the encoder-build write site (set_quality_value in slot.cpp) consumes, so
// the editor's range source and the encoder's write target are derived from
// the same canonical list by construction.
IntRange introspect_quality_range(obs_properties_t* props)
{
    for (const char* const* k = rc_util::quality_keys(); *k; ++k) {
        IntRange r = introspect_int_range(props, *k);
        if (r.found) return r;
    }
    for (const char* const* k = rc_util::quality_split_keys(); *k; ++k) {
        IntRange r = introspect_int_range(props, *k);
        if (r.found) return r;
    }
    return {};
}

} // namespace

// =============================================================================
// construction
// =============================================================================

SlotEditor::SlotEditor(QWidget* parent, SceneSlot::Config cfg)
    : QDialog(parent), cfg_(std::move(cfg))
{
    setWindowTitle("Slot Configuration");
    resize(520, 640);

    form_ = new QFormLayout;
    auto* form = form_;

    name_edit_ = new QLineEdit(QString::fromStdString(cfg_.name));
    form->addRow("Name", name_edit_);

    scene_combo_ = new QComboBox;
    populate_scene_combo();
    form->addRow("Scene", scene_combo_);

    auto* res_row = new QHBoxLayout;
    // NV12 (4:2:0) requires even width/height; step by 2 and snap the
    // initial value up to even.
    w_spin_ = new QSpinBox;
    w_spin_->setRange(64, 7680);
    w_spin_->setSingleStep(2);
    w_spin_->setValue((int)(cfg_.width  % 2 == 0 ? cfg_.width  : cfg_.width  + 1));
    h_spin_ = new QSpinBox;
    h_spin_->setRange(64, 4320);
    h_spin_->setSingleStep(2);
    h_spin_->setValue((int)(cfg_.height % 2 == 0 ? cfg_.height : cfg_.height + 1));
    res_row->addWidget(w_spin_); res_row->addWidget(new QLabel("x")); res_row->addWidget(h_spin_);
    auto* res_wrap = new QWidget; res_wrap->setLayout(res_row);
    form->addRow("Resolution", res_wrap);

    auto* fps_row = new QHBoxLayout;
    fps_num_spin_ = new QSpinBox; fps_num_spin_->setRange(1, 240000);
    fps_num_spin_->setValue(cfg_.fps_num ? (int)cfg_.fps_num : 60);
    fps_den_spin_ = new QSpinBox; fps_den_spin_->setRange(1, 1001);
    fps_den_spin_->setValue(cfg_.fps_den ? (int)cfg_.fps_den : 1);
    fps_row->addWidget(fps_num_spin_);
    fps_row->addWidget(new QLabel("/"));
    fps_row->addWidget(fps_den_spin_);
    auto* fps_wrap = new QWidget; fps_wrap->setLayout(fps_row);
    form->addRow("FPS num / den", fps_wrap);

    auto* path_row = new QHBoxLayout;
    path_edit_ = new QLineEdit(QString::fromStdString(cfg_.path));
    auto* browse = new QPushButton("...");
    path_row->addWidget(path_edit_); path_row->addWidget(browse);
    auto* path_wrap = new QWidget; path_wrap->setLayout(path_row);
    form->addRow("Output dir", path_wrap);
    connect(browse, &QPushButton::clicked, this, &SlotEditor::on_browse_path);

    container_combo_ = new QComboBox;
    container_combo_->addItems({"mp4", "mkv", "flv", "mov", "ts"});
    {
        int ci = container_combo_->findText(QString::fromStdString(
            cfg_.container.empty() ? "mp4" : cfg_.container));
        if (ci >= 0) container_combo_->setCurrentIndex(ci);
    }
    form->addRow("Container", container_combo_);

    venc_combo_ = new QComboBox;
    populate_video_encoder_combo();
    form->addRow("Video encoder", venc_combo_);

    // --- User-configurable video encoder settings (encoder-specific show/hide) ---

    keyframe_sec_spin_ = new QSpinBox;
    keyframe_sec_spin_->setRange(1, 10);
    keyframe_sec_spin_->setValue((int)cfg_.keyframe_interval_sec);
    keyframe_sec_spin_->setSuffix(" s");
    keyframe_sec_spin_->setToolTip("Keyframe interval in seconds.");
    form->addRow("Keyframe interval", keyframe_sec_spin_);

    encoder_preset_combo_ = new QComboBox;
    form->addRow("Encoder preset", encoder_preset_combo_);

    encoder_profile_combo_ = new QComboBox;
    form->addRow("Profile", encoder_profile_combo_);

    encoder_tune_combo_ = new QComboBox;
    form->addRow("Tune", encoder_tune_combo_);

    multipass_combo_ = new QComboBox;
    form->addRow("Multipass", multipass_combo_);

    lookahead_check_ = new QCheckBox("Enable look-ahead");
    lookahead_check_->setChecked(cfg_.lookahead);
    form->addRow(lookahead_check_);

    psycho_aq_check_ = new QCheckBox("Psycho Visual Tuning (Psycho AQ)");
    psycho_aq_check_->setChecked(cfg_.psycho_aq);
    form->addRow(psycho_aq_check_);

    b_frames_spin_ = new QSpinBox;
    b_frames_spin_->setRange(-1, 16);
    b_frames_spin_->setValue(cfg_.b_frames);
    b_frames_spin_->setSpecialValueText("Auto");
    b_frames_spin_->setToolTip("Number of consecutive B-frames. \"Auto\" = encoder default.");
    form->addRow("B-frames", b_frames_spin_);

    gpu_index_spin_ = new QSpinBox;
    gpu_index_spin_->setRange(-1, 7);
    gpu_index_spin_->setValue(cfg_.gpu_index);
    gpu_index_spin_->setSpecialValueText("Auto");
    gpu_index_spin_->setToolTip("GPU device index for encoding. \"Auto\" = let OBS choose.");
    form->addRow("GPU index", gpu_index_spin_);

    aenc_combo_ = new QComboBox;
    populate_audio_encoder_combo();
    form->addRow("Audio encoder", aenc_combo_);

    rc_combo_ = new QComboBox;
    form->addRow("Rate control", rc_combo_);

    rc_value_label_ = new QLabel("Bitrate");
    rc_value_spin_  = new QSpinBox;
    rc_value_spin_->setRange(0, 300000);
    rc_value_spin_->setValue((int)cfg_.rc_value);
    form->addRow(rc_value_label_, rc_value_spin_);

    // Populate rate control modes from the selected encoder, then size the
    // value field. Done after both widgets exist so the slots can touch them.
    populate_rate_control_combo();
    update_rc_value_field();

    connect(venc_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SlotEditor::on_encoder_changed);
    connect(rc_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SlotEditor::on_rc_changed);

    // --- Advanced encoder settings section ---

    // Advanced check — toggles visibility of advanced_box_.
    advanced_check_ = new QCheckBox("Advanced encoder settings");
    advanced_check_->setChecked(cfg_.advanced_settings);
    form->addRow(advanced_check_);

    // Advanced groupbox — contains its own form layout with all advanced settings.
    advanced_box_ = new QGroupBox("Advanced");
    adv_form_ = new QFormLayout(advanced_box_);

    // --- Common (shown for x264, NVENC, AMF) ---
    max_qp_spin_ = new QSpinBox;
    max_qp_spin_->setRange(-1, 69);
    max_qp_spin_->setValue(cfg_.max_qp);
    max_qp_spin_->setSpecialValueText("\u2014");  // em-dash for "not set"
    adv_form_->addRow("Max QP", max_qp_spin_);

    min_qp_spin_ = new QSpinBox;
    min_qp_spin_->setRange(-1, 69);
    min_qp_spin_->setValue(cfg_.min_qp);
    min_qp_spin_->setSpecialValueText("\u2014");
    adv_form_->addRow("Min QP", min_qp_spin_);

    // --- x264 ---
    cabac_check_ = new QCheckBox("Enable CABAC");
    cabac_check_->setChecked(cfg_.cabac);
    adv_form_->addRow(cabac_check_);

    x264opts_edit_ = new QLineEdit(QString::fromStdString(cfg_.x264opts));
    x264opts_edit_->setPlaceholderText("e.g. deblock=0:0 ref=3");
    adv_form_->addRow("x264 extra options", x264opts_edit_);

    mbtree_check_ = new QCheckBox("Enable MB-tree rate control");
    mbtree_check_->setChecked(cfg_.mbtree);
    adv_form_->addRow(mbtree_check_);

    aq_mode_combo_ = new QComboBox;
    aq_mode_combo_->addItem("(encoder default)", -1);
    aq_mode_combo_->addItem("0 \u2014 Disabled",              0);
    aq_mode_combo_->addItem("1 \u2014 Variance AQ",           1);
    aq_mode_combo_->addItem("2 \u2014 Autovariance AQ",       2);
    aq_mode_combo_->addItem("3 \u2014 Autovariance AQ + bias",3);
    {
        int idx = aq_mode_combo_->findData(cfg_.aq_mode);
        aq_mode_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    adv_form_->addRow("AQ mode", aq_mode_combo_);

    // --- NVENC ---
    nvenc_repeat_headers_check_ = new QCheckBox("Repeat SPS/PPS headers (repeat_headers)");
    nvenc_repeat_headers_check_->setChecked(cfg_.nvenc_repeat_headers);
    adv_form_->addRow(nvenc_repeat_headers_check_);

    nvenc_force_idr_check_ = new QCheckBox("Force IDR on scene cut (force_idr)");
    nvenc_force_idr_check_->setChecked(cfg_.nvenc_force_idr);
    adv_form_->addRow(nvenc_force_idr_check_);

    nvenc_dyn_bitrate_check_ = new QCheckBox("Dynamic bitrate control (dynamic_bitrate)");
    nvenc_dyn_bitrate_check_->setChecked(cfg_.nvenc_dyn_bitrate);
    adv_form_->addRow(nvenc_dyn_bitrate_check_);

    // --- AMF ---
    amf_enforce_hrd_check_ = new QCheckBox("Enforce HRD (enforce_hrd)");
    amf_enforce_hrd_check_->setChecked(cfg_.amf_enforce_hrd);
    adv_form_->addRow(amf_enforce_hrd_check_);

    amf_vbaq_check_ = new QCheckBox("VBAQ (vbaq)");
    amf_vbaq_check_->setChecked(cfg_.amf_vbaq);
    adv_form_->addRow(amf_vbaq_check_);

    amf_pre_analysis_check_ = new QCheckBox("Pre-analysis (preanalysis)");
    amf_pre_analysis_check_->setChecked(cfg_.amf_pre_analysis);
    adv_form_->addRow(amf_pre_analysis_check_);

    amf_enable_throughput_check_ = new QCheckBox("Enable throughput mode (enable_throughput)");
    amf_enable_throughput_check_->setChecked(cfg_.amf_enable_throughput);
    adv_form_->addRow(amf_enable_throughput_check_);

    // --- QSV ---
    qsv_async_depth_spin_ = new QSpinBox;
    qsv_async_depth_spin_->setRange(-1, 32);
    qsv_async_depth_spin_->setValue(cfg_.qsv_async_depth);
    qsv_async_depth_spin_->setSpecialValueText("Auto");
    adv_form_->addRow("Async depth", qsv_async_depth_spin_);

    qsv_latency_combo_ = new QComboBox;
    qsv_latency_combo_->addItem("(encoder default)", QString(""));
    // Introspect "latency" property from the QSV encoder; if not available, add known values.
    // The populate call happens in update_encoder_specific_ui. Seed with known fallback now:
    qsv_latency_combo_->addItem("Normal", QString("normal"));
    qsv_latency_combo_->addItem("Low",    QString("low"));
    {
        int idx = qsv_latency_combo_->findData(QString::fromStdString(cfg_.qsv_latency));
        qsv_latency_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    adv_form_->addRow("Latency mode", qsv_latency_combo_);

    // --- VideoToolbox ---
    vt_realtime_check_ = new QCheckBox("Real-time encoding priority (realtime)");
    vt_realtime_check_->setChecked(cfg_.vt_realtime);
    adv_form_->addRow(vt_realtime_check_);

    vt_frames_before_start_spin_ = new QSpinBox;
    vt_frames_before_start_spin_->setRange(-1, 300);
    vt_frames_before_start_spin_->setValue(cfg_.vt_frames_before_start);
    vt_frames_before_start_spin_->setSpecialValueText("Auto");
    adv_form_->addRow("Frames before start", vt_frames_before_start_spin_);

    // --- HEVC/AV1 ---
    hevc_tier_combo_ = new QComboBox;
    hevc_tier_combo_->addItem("Main", QString("main"));
    hevc_tier_combo_->addItem("High", QString("high"));
    {
        int idx = hevc_tier_combo_->findData(QString::fromStdString(cfg_.hevc_tier));
        hevc_tier_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    adv_form_->addRow("HEVC tier", hevc_tier_combo_);

    av1_tile_cols_spin_ = new QSpinBox;
    av1_tile_cols_spin_->setRange(-1, 6);
    av1_tile_cols_spin_->setValue(cfg_.av1_tile_cols);
    av1_tile_cols_spin_->setSpecialValueText("Auto");
    adv_form_->addRow("AV1 tile columns", av1_tile_cols_spin_);

    av1_tile_rows_spin_ = new QSpinBox;
    av1_tile_rows_spin_->setRange(-1, 6);
    av1_tile_rows_spin_->setValue(cfg_.av1_tile_rows);
    av1_tile_rows_spin_->setSpecialValueText("Auto");
    adv_form_->addRow("AV1 tile rows", av1_tile_rows_spin_);

    advanced_box_->setVisible(cfg_.advanced_settings);
    form->addRow(advanced_box_);

    connect(advanced_check_, &QCheckBox::toggled, this, [this](bool on) {
        advanced_box_->setVisible(on);
    });

    abitrate_spin_ = new QSpinBox; abitrate_spin_->setRange(32, 512);
    abitrate_spin_->setValue((int)cfg_.audio_bitrate); abitrate_spin_->setSuffix(" kbps");
    form->addRow("Audio bitrate (per track)", abitrate_spin_);

    auto* tracks_box = new QGroupBox("Audio tracks (1-6)");
    tracks_box->setToolTip(
        "Select which of OBS's 6 audio mix tracks to include in this recording.\n"
        "Each selected track becomes a separate audio stream in the output file.\n"
        "Track routing is configured via Advanced Audio Properties in the main OBS window.");
    auto* tracks_row = new QHBoxLayout(tracks_box);
    for (int i = 0; i < 6; ++i) {
        auto* cb = new QCheckBox(QString::number(i + 1));
        cb->setChecked((cfg_.audio_tracks & (1u << i)) != 0);
        track_checks_[i] = cb;
        tracks_row->addWidget(cb);
    }
    tracks_row->addStretch();
    form->addRow(tracks_box);

    replay_check_ = new QCheckBox("Enable replay buffer");
    replay_check_->setChecked(cfg_.replay_enabled);
    form->addRow(replay_check_);

    replay_mp4_warn_ = new QLabel(
        "Warning: MP4 replay files are unrecoverable if OBS crashes. Prefer MKV.");
    replay_mp4_warn_->setStyleSheet("color: rgb(220, 140, 60);");
    replay_mp4_warn_->setWordWrap(true);
    form->addRow(replay_mp4_warn_);

    replay_only_check_ = new QCheckBox("Replay buffer only (no continuous recording file)");
    replay_only_check_->setChecked(cfg_.replay_only);
    replay_only_check_->setEnabled(cfg_.replay_enabled);
    replay_only_check_->setToolTip(
        "When enabled, the slot runs only its replay buffer.\n"
        "No continuous recording file is written -- use the Save Replay\n"
        "hotkey or button to capture clips on demand.");
    form->addRow(replay_only_check_);

    // replay-only is meaningless without the replay buffer enabled.
    connect(replay_check_, &QCheckBox::toggled, this, [this](bool on) {
        replay_only_check_->setEnabled(on);
        if (!on) replay_only_check_->setChecked(false);
        update_mp4_warning();
    });
    connect(container_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { update_mp4_warning(); });
    update_mp4_warning();

    replay_secs_ = new QSpinBox; replay_secs_->setRange(5, 600);
    replay_secs_->setValue((int)cfg_.replay_seconds); replay_secs_->setSuffix(" s");
    form->addRow("Replay length", replay_secs_);

    replay_max_size_spin_ = new QSpinBox;
    replay_max_size_spin_->setRange(0, 65536);
    replay_max_size_spin_->setSuffix(" MB");
    replay_max_size_spin_->setSpecialValueText("Auto");
    replay_max_size_spin_->setValue((int)cfg_.replay_max_size_mb);
    replay_max_size_spin_->setToolTip(QString::fromUtf8(
        "Memory ceiling for the replay buffer.\n\n"
        "Empty / 0 (Auto): sized automatically from "
        "resolution \xc3\x97 fps \xc3\x97 replay seconds \xc3\x97 2\xc3\x97 safety margin, "
        "calibrated for typical high-quality settings "
        "(around CQP-17 / CRF-18). The resolved value is shown "
        "alongside this field.\n\n"
        "Positive integer: overrides the auto-derived value verbatim. "
        "Set higher if you use an extreme-quality setting "
        "and see \"suspected memory cap\" warnings in the log; "
        "set lower to cap RAM use at the cost of shorter saved clips."));
    replay_max_size_label_ = new QLabel;
    auto* mb_row = new QHBoxLayout;
    mb_row->addWidget(replay_max_size_spin_);
    mb_row->addWidget(replay_max_size_label_, 1);
    auto* mb_row_wrap = new QWidget;
    mb_row_wrap->setLayout(mb_row);
    form->addRow("Max replay buffer size", mb_row_wrap);

    {
        auto refresh = [this]() { on_replay_max_size_inputs_changed(); };
        connect(replay_max_size_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(replay_secs_,          QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(w_spin_,               QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(h_spin_,               QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(fps_num_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(fps_den_spin_,         QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        connect(rc_combo_,             QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, refresh);
        connect(rc_value_spin_,        QOverload<int>::of(&QSpinBox::valueChanged),
                this, refresh);
        refresh();
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SlotEditor::on_accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Wrap the form in a scroll area so the dialog remains usable when its
    // content exceeds the available screen height (e.g. advanced encoder
    // settings expanded on a small display).
    auto* scroll_content = new QWidget;
    scroll_content->setLayout(form); // widgets are now parented — safe to call setVisible

    // Set initial visibility and populate encoder-specific combos.
    // Must run AFTER setLayout so widgets have a parent and setVisible(true)
    // doesn't spawn them as temporary top-level windows.
    update_encoder_specific_ui();
    update_shared_encoder_visibility();

    auto* scroll = new QScrollArea;
    scroll->setWidget(scroll_content);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* root = new QVBoxLayout(this);
    root->addWidget(scroll);
    root->addWidget(buttons);
}

// =============================================================================
// combo population
// =============================================================================

void SlotEditor::populate_scene_combo()
{
    char** scenes = obs_frontend_get_scene_names();
    if (!scenes) return;
    for (char** p = scenes; *p; ++p)
        scene_combo_->addItem(QString::fromUtf8(*p));
    bfree(scenes);

    int idx = scene_combo_->findText(QString::fromStdString(cfg_.scene_name));
    if (idx >= 0) {
        scene_combo_->setCurrentIndex(idx);
    } else if (!cfg_.scene_name.empty()) {
        // Configured scene no longer exists. Insert a placeholder carrying
        // the original name as item data so on_accept() can preserve it if
        // the user doesn't pick a valid scene.
        scene_combo_->insertItem(
            0,
            QString("[missing] %1").arg(QString::fromStdString(cfg_.scene_name)),
            QString::fromStdString(cfg_.scene_name));
        scene_combo_->setCurrentIndex(0);
    }
}

void SlotEditor::populate_video_encoder_combo()
{
    const char* enc_id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &enc_id); ++i) {
        if (!enc_id) continue;
        if (obs_get_encoder_type(enc_id) != OBS_ENCODER_VIDEO) continue;
        const char* codec = obs_get_encoder_codec(enc_id);
        const char* disp  = obs_encoder_get_display_name(enc_id);
        QString label = QString("%1  [%2 / %3]")
            .arg(QString::fromUtf8(disp ? disp : enc_id))
            .arg(QString::fromUtf8(codec ? codec : "?"))
            .arg(QString::fromUtf8(enc_id));
        venc_combo_->addItem(label, QString::fromUtf8(enc_id));
    }

    // --- Shared encoder entries ---
    venc_combo_->insertSeparator(venc_combo_->count());

    auto& mgr = SlotManager::instance();
    for (size_t si = 0; si < mgr.slot_count(); ++si) {
        const SceneSlot* other = mgr.slot_at(si);
        if (!other) continue;
        const auto& oc = other->config();
        // Don't list self (when editing an existing slot).
        if (oc.id == cfg_.id) continue;
        // Don't list slots that are themselves dependents (no chaining).
        if (!oc.shared_encoder_slot_id.empty()) continue;

        QString label = QString("Use \"%1\" encoder").arg(QString::fromStdString(oc.name));
        QString data  = QString("shared:%1").arg(QString::fromStdString(oc.id));
        venc_combo_->addItem(label, data);
    }

    if (!cfg_.shared_encoder_slot_id.empty()) {
        QString want = QString("shared:%1").arg(QString::fromStdString(cfg_.shared_encoder_slot_id));
        int idx = venc_combo_->findData(want);
        if (idx >= 0) venc_combo_->setCurrentIndex(idx);
        else {
            // Referenced slot no longer exists — insert a placeholder.
            venc_combo_->insertItem(0, QString("Use [deleted slot] encoder  [unavailable]"), want);
            venc_combo_->setCurrentIndex(0);
        }
    } else {
        // Existing real-encoder selection logic (unchanged).
        QString cur = QString::fromStdString(
            cfg_.video_encoder_id.empty() ? "obs_x264" : cfg_.video_encoder_id);
        int idx = venc_combo_->findData(cur);
        if (idx >= 0) {
            venc_combo_->setCurrentIndex(idx);
        } else {
            venc_combo_->insertItem(0, cur + "  [unavailable]", cur);
            venc_combo_->setCurrentIndex(0);
        }
    }
}

void SlotEditor::populate_audio_encoder_combo()
{
    const char* enc_id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &enc_id); ++i) {
        if (!enc_id) continue;
        if (obs_get_encoder_type(enc_id) != OBS_ENCODER_AUDIO) continue;
        const char* codec = obs_get_encoder_codec(enc_id);
        const char* disp  = obs_encoder_get_display_name(enc_id);
        QString label = QString("%1  [%2 / %3]")
            .arg(QString::fromUtf8(disp ? disp : enc_id))
            .arg(QString::fromUtf8(codec ? codec : "?"))
            .arg(QString::fromUtf8(enc_id));
        aenc_combo_->addItem(label, QString::fromUtf8(enc_id));
    }

    QString cur = QString::fromStdString(
        cfg_.audio_encoder_id.empty() ? "ffmpeg_aac" : cfg_.audio_encoder_id);
    int idx = aenc_combo_->findData(cur);
    if (idx >= 0) {
        aenc_combo_->setCurrentIndex(idx);
    } else {
        aenc_combo_->insertItem(0, cur + "  [unavailable]", cur);
        aenc_combo_->setCurrentIndex(0);
    }
}

// Introspect the selected encoder for its supported rate control modes.
void SlotEditor::populate_rate_control_combo()
{
    loading_ = true;
    rc_combo_->clear();

    const std::string enc_id =
        venc_combo_->currentData().toString().toStdString();

    obs_properties_t* props =
        enc_id.empty() ? nullptr : obs_get_encoder_properties(enc_id.c_str());

    if (props) {
        obs_property_t* rc = obs_properties_get(props, "rate_control");
        if (rc && obs_property_get_type(rc) == OBS_PROPERTY_LIST) {
            size_t count = obs_property_list_item_count(rc);
            for (size_t i = 0; i < count; ++i) {
                const char* nm  = obs_property_list_item_name(rc, i);
                const char* val = obs_property_list_item_string(rc, i);
                if (!val || !*val) continue;
                rc_combo_->addItem(QString::fromUtf8(nm ? nm : val),
                                   QString::fromUtf8(val));
            }
        }
        obs_properties_destroy(props);
    }

    // Fallback if the encoder didn't expose a rate_control list at all.
    if (rc_combo_->count() == 0)
        rc_combo_->addItem("CBR", "CBR");

    // Select the configured mode, or fall back to the first available.
    QString want = QString::fromStdString(
        cfg_.rate_control.empty() ? "CBR" : cfg_.rate_control);
    int idx = rc_combo_->findData(want);
    rc_combo_->setCurrentIndex(idx >= 0 ? idx : 0);

    loading_ = false;
}

// =============================================================================
// dynamic value field
// =============================================================================

// Relabel + re-range the value spinbox for the currently selected rate
// control mode, using introspected property ranges where available.
void SlotEditor::update_rc_value_field()
{
    const std::string enc_id =
        venc_combo_->currentData().toString().toStdString();
    const std::string mode =
        rc_combo_->currentData().toString().toStdString();

    if (rc_util::is_lossless(mode)) {
        rc_value_label_->setText("Quality");
        rc_value_spin_->setEnabled(false);
        rc_value_spin_->setSuffix("");
        rc_value_spin_->setSpecialValueText("— (lossless)");
        rc_value_spin_->setRange(0, 0);
        return;
    }

    rc_value_spin_->setEnabled(true);
    rc_value_spin_->setSpecialValueText("");

    obs_properties_t* props =
        enc_id.empty() ? nullptr : obs_get_encoder_properties(enc_id.c_str());

    if (rc_util::is_bitrate_based(mode)) {
        rc_value_label_->setText("Bitrate");
        rc_value_spin_->setSuffix(" kbps");
        IntRange r = props ? introspect_int_range(props, "bitrate") : IntRange{};
        if (r.found) rc_value_spin_->setRange(std::max(1, r.min), r.max);
        else         rc_value_spin_->setRange(500, 300000);
        // Keep value if it's sane for a bitrate; otherwise default.
        if (cfg_.rc_value < (uint32_t)rc_value_spin_->minimum() ||
            cfg_.rc_value > (uint32_t)rc_value_spin_->maximum())
            rc_value_spin_->setValue(std::min(6000, rc_value_spin_->maximum()));
        else
            rc_value_spin_->setValue((int)cfg_.rc_value);
    } else {
        // Quality-based mode (CQP / CRF / CQ / ICQ / ...).
        rc_value_label_->setText("Quality (lower = better)");
        rc_value_spin_->setSuffix("");
        IntRange r = props ? introspect_quality_range(props) : IntRange{};
        if (r.found) rc_value_spin_->setRange(r.min, r.max);
        else         rc_value_spin_->setRange(0, 51);
        if (cfg_.rc_value < (uint32_t)rc_value_spin_->minimum() ||
            cfg_.rc_value > (uint32_t)rc_value_spin_->maximum()) {
            int def = std::min(23, rc_value_spin_->maximum());
            def = std::max(def, rc_value_spin_->minimum());
            rc_value_spin_->setValue(def);
        } else {
            rc_value_spin_->setValue((int)cfg_.rc_value);
        }
    }

    if (props) obs_properties_destroy(props);
}

// =============================================================================
// slots
// =============================================================================

void SlotEditor::on_encoder_changed()
{
    if (loading_) return;

    const QString data = venc_combo_->currentData().toString();
    const bool is_shared = data.startsWith("shared:");

    // FR-016: a consumer → standalone transition within an open editor
    // session must seed cfg_.rate_control / cfg_.rc_value with valid defaults
    // for the newly-selected encoder, otherwise populate_rate_control_combo
    // and update_rc_value_field would see the "<inherited>" sentinel + 0 the
    // consumer state carried in (or stale values from a previous standalone
    // selection). The previously-consumer state is detected by
    // shared_encoder_slot_id being non-empty when this handler fires; the
    // sentinel value of rate_control is the typical precondition but the
    // seeding rule applies regardless of the prior persisted shape.
    const bool was_consumer = !cfg_.shared_encoder_slot_id.empty();
    if (was_consumer && !is_shared) {
        cfg_.shared_encoder_slot_id.clear();
        cfg_.video_encoder_id = data.toStdString();
        if (cfg_.video_encoder_id.empty()) cfg_.video_encoder_id = "obs_x264";

        std::string seeded_mode;
        int seeded_value = 0;
        bool have_range = false;
        obs_properties_t* props = obs_get_encoder_properties(cfg_.video_encoder_id.c_str());
        if (props) {
            obs_property_t* rc = obs_properties_get(props, "rate_control");
            if (rc && obs_property_get_type(rc) == OBS_PROPERTY_LIST &&
                obs_property_list_item_count(rc) > 0) {
                const char* first = obs_property_list_item_string(rc, 0);
                if (first && *first) seeded_mode = first;
            }
            if (!seeded_mode.empty() && !rc_util::is_lossless(seeded_mode)) {
                IntRange r = rc_util::is_bitrate_based(seeded_mode)
                                 ? introspect_int_range(props, "bitrate")
                                 : introspect_quality_range(props);
                if (r.found) {
                    seeded_value = (r.min + r.max) / 2;
                    have_range = true;
                }
            }
            obs_properties_destroy(props);
        }
        cfg_.rate_control = seeded_mode.empty() ? std::string("CBR") : seeded_mode;
        // When no range was introspected, leave rc_value at 0 — update_rc_value_field
        // will snap the spinbox to the field's minimum, and the user can edit
        // before clicking Save.
        cfg_.rc_value = have_range ? (uint32_t)std::max(0, seeded_value) : 0u;
    }

    if (!is_shared) {
        populate_rate_control_combo();
        update_rc_value_field();
        update_encoder_specific_ui();
    }
    update_shared_encoder_visibility();
}

void SlotEditor::on_rc_changed()
{
    if (loading_) return;
    // Persist whatever the user had typed before we re-range the field, so a
    // mode switch doesn't silently discard a still-valid value.
    cfg_.rc_value = (uint32_t)rc_value_spin_->value();
    update_rc_value_field();
}

void SlotEditor::update_mp4_warning()
{
    if (!replay_mp4_warn_) return;
    const QString container = container_combo_->currentText().toLower();
    const bool show = replay_check_->isChecked() && container == "mp4";
    replay_mp4_warn_->setVisible(show);
}

void SlotEditor::on_browse_path()
{
    QString d = QFileDialog::getExistingDirectory(this, "Output directory", path_edit_->text());
    if (!d.isEmpty()) path_edit_->setText(d);
}

void SlotEditor::on_accept()
{
    cfg_.name       = name_edit_->text().toStdString();

    // The [missing]-scene placeholder carries the original scene name in
    // item data; regular scene items have no data. Prefer data when present
    // so an unfixed missing scene is written back unchanged.
    const QString scene_data = scene_combo_->currentData().toString();
    const bool scene_is_missing_placeholder = !scene_data.isEmpty();
    cfg_.scene_name = scene_is_missing_placeholder
        ? scene_data.toStdString()
        : scene_combo_->currentText().toStdString();

    // NV12 needs even dimensions; defensively round down and floor at 64.
    cfg_.width  = (uint32_t)(w_spin_->value() & ~1u);
    cfg_.height = (uint32_t)(h_spin_->value() & ~1u);
    if (cfg_.width  < 64) cfg_.width  = 64;
    if (cfg_.height < 64) cfg_.height = 64;
    cfg_.fps_num    = (uint32_t)fps_num_spin_->value();
    cfg_.fps_den    = (uint32_t)fps_den_spin_->value();
    cfg_.path       = path_edit_->text().toStdString();
    cfg_.container  = container_combo_->currentText().toStdString();

    // Distinguish shared encoder from real encoder selection.
    const QString venc_data = venc_combo_->currentData().toString();
    if (venc_data.startsWith("shared:")) {
        cfg_.shared_encoder_slot_id = venc_data.mid(7).toStdString();
        cfg_.video_encoder_id.clear();
        // Symmetric save-side write (Decision 2 / FR-006): a consumer slot
        // must never persist standalone rate-control values. The sentinel +
        // zero are what slot_from_data also writes at load. The unconditional
        // rc_combo_/rc_value_spin_ read below is guarded by the same
        // !shared_encoder_slot_id.empty() check so these writes survive.
        cfg_.rate_control = "<inherited>";
        cfg_.rc_value     = 0;
    } else {
        cfg_.shared_encoder_slot_id.clear();
        cfg_.video_encoder_id = venc_data.toStdString();
        if (cfg_.video_encoder_id.empty()) cfg_.video_encoder_id = "obs_x264";
    }

    // Video encoder settings: only read when using own encoder.
    if (cfg_.shared_encoder_slot_id.empty()) {
    cfg_.keyframe_interval_sec = (uint32_t)keyframe_sec_spin_->value();

    // Preset: read from current combo data only if the combo is visible (relevant encoder).
    // If hidden, the combo may contain stale data from a previous encoder; don't overwrite.
    if (encoder_preset_combo_->isVisible())
        cfg_.encoder_preset = encoder_preset_combo_->currentData().toString().toStdString();

    if (encoder_profile_combo_->isVisible())
        cfg_.encoder_profile = encoder_profile_combo_->currentData().toString().toStdString();

    if (encoder_tune_combo_->isVisible())
        cfg_.encoder_tune = encoder_tune_combo_->currentData().toString().toStdString();

    if (multipass_combo_->isVisible())
        cfg_.multipass = multipass_combo_->currentData().toString().toStdString();

    // Checkboxes: always written regardless of visibility — they hold the last
    // user-set value and the fallback in apply_family_presets uses them only for
    // the relevant encoder family.
    cfg_.lookahead  = lookahead_check_->isChecked();
    cfg_.psycho_aq  = psycho_aq_check_->isChecked();

    cfg_.b_frames   = b_frames_spin_->value();
    cfg_.gpu_index  = gpu_index_spin_->value();

    // Advanced
    cfg_.advanced_settings = advanced_check_->isChecked();
    cfg_.max_qp     = max_qp_spin_->value();
    cfg_.min_qp     = min_qp_spin_->value();
    cfg_.cabac      = cabac_check_->isChecked();
    cfg_.x264opts   = x264opts_edit_->text().toStdString();
    cfg_.mbtree     = mbtree_check_->isChecked();
    cfg_.aq_mode    = aq_mode_combo_->currentData().toInt();
    cfg_.nvenc_repeat_headers  = nvenc_repeat_headers_check_->isChecked();
    cfg_.nvenc_force_idr       = nvenc_force_idr_check_->isChecked();
    cfg_.nvenc_dyn_bitrate     = nvenc_dyn_bitrate_check_->isChecked();
    cfg_.amf_enforce_hrd       = amf_enforce_hrd_check_->isChecked();
    cfg_.amf_vbaq              = amf_vbaq_check_->isChecked();
    cfg_.amf_pre_analysis      = amf_pre_analysis_check_->isChecked();
    cfg_.amf_enable_throughput = amf_enable_throughput_check_->isChecked();
    cfg_.qsv_async_depth       = qsv_async_depth_spin_->value();
    cfg_.qsv_latency = qsv_latency_combo_->currentData().toString().toStdString();
    cfg_.vt_realtime           = vt_realtime_check_->isChecked();
    cfg_.vt_frames_before_start = vt_frames_before_start_spin_->value();
    cfg_.hevc_tier  = hevc_tier_combo_->currentData().toString().toStdString();
    cfg_.av1_tile_cols = av1_tile_cols_spin_->value();
    cfg_.av1_tile_rows = av1_tile_rows_spin_->value();
    } // end if (cfg_.shared_encoder_slot_id.empty())

    cfg_.audio_encoder_id = aenc_combo_->currentData().toString().toStdString();
    if (cfg_.audio_encoder_id.empty()) cfg_.audio_encoder_id = "ffmpeg_aac";

    // Rate control: only persist standalone values for own-encoder slots.
    // The consumer branch above already wrote the sentinel + 0.
    if (cfg_.shared_encoder_slot_id.empty()) {
        cfg_.rate_control = rc_combo_->currentData().toString().toStdString();
        if (cfg_.rate_control.empty()) cfg_.rate_control = "CBR";
        cfg_.rc_value = (uint32_t)rc_value_spin_->value();
    }

    cfg_.audio_bitrate  = (uint32_t)abitrate_spin_->value();
    cfg_.replay_enabled = replay_check_->isChecked();
    cfg_.replay_only    = replay_only_check_->isChecked() && cfg_.replay_enabled;
    cfg_.replay_seconds = (uint32_t)replay_secs_->value();
    cfg_.replay_max_size_mb = (uint32_t)replay_max_size_spin_->value();

    cfg_.audio_tracks = 0;
    for (int i = 0; i < 6; ++i)
        if (track_checks_[i] && track_checks_[i]->isChecked())
            cfg_.audio_tracks |= (1u << i);
    if (cfg_.audio_tracks == 0) cfg_.audio_tracks = 0x01;

    // A scene-bound slot whose configured scene no longer exists cannot
    // record meaningfully. Refuse to close until the user picks a real
    // scene (dependent slots borrow an encoder and don't need a scene).
    if (scene_is_missing_placeholder && cfg_.shared_encoder_slot_id.empty()) {
        QMessageBox::warning(
            this, tr("Scene not found"),
            tr("The scene \"%1\" no longer exists. Select a valid scene "
               "before saving this slot.")
                .arg(QString::fromStdString(cfg_.scene_name)));
        return; // keep the dialog open
    }

    // cfg_.id is left untouched -> stable identity preserved across edits.
    accept();
}

// =============================================================================
// encoder-specific UI helpers
// =============================================================================

bool SlotEditor::enc_has(const std::string& enc_id, const char* needle)
{
    return enc_id.find(needle) != std::string::npos;
}

const char* SlotEditor::preset_prop_key(const std::string& enc_id)
{
    if (enc_has(enc_id, "jim_nvenc") || enc_has(enc_id, "jim_hevc_nvenc"))
        return "preset2";
    if (enc_has(enc_id, "qsv"))
        return "target_usage";
    return "preset";
}

void SlotEditor::set_form_row_visible(QFormLayout* fl, QWidget* field, bool visible)
{
    if (!field) return;
    field->setVisible(visible);
    if (fl) {
        QWidget* lbl = qobject_cast<QWidget*>(fl->labelForField(field));
        if (lbl) lbl->setVisible(visible);
    }
}

void SlotEditor::populate_combo_from_encoder_property(
    QComboBox* combo,
    obs_properties_t* props,
    const char* prop_key,
    const std::string& current_val,
    bool add_empty_first)
{
    combo->clear();
    if (add_empty_first)
        combo->addItem("(encoder default)", QString(""));

    // F-USE1: props is borrowed from the caller's single obs_get_encoder_properties
    // call. Do NOT destroy it here -- the caller owns the lifetime.
    if (props) {
        obs_property_t* p = obs_properties_get(props, prop_key);
        if (p && obs_property_get_type(p) == OBS_PROPERTY_LIST) {
            size_t n = obs_property_list_item_count(p);
            for (size_t i = 0; i < n; ++i) {
                const char* nm  = obs_property_list_item_name(p, i);
                const char* val = obs_property_list_item_string(p, i);
                if (!val || !*val) continue;
                combo->addItem(QString::fromUtf8(nm ? nm : val),
                               QString::fromUtf8(val));
            }
        }
    }

    // Select configured value; fall back to index 0.
    int idx = combo->findData(QString::fromStdString(current_val));
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void SlotEditor::update_encoder_specific_ui()
{
    const std::string enc_id =
        venc_combo_->currentData().toString().toStdString();

    // Determine encoder family.
    const bool is_x264  = enc_has(enc_id, "x264");
    const bool is_nvenc = enc_has(enc_id, "nvenc");
    const bool is_amf   = enc_has(enc_id, "amf");
    const bool is_qsv   = enc_has(enc_id, "qsv");
    const bool is_vt    = enc_has(enc_id, "videotoolbox") || enc_has(enc_id, "apple");

    // Determine codec for HEVC/AV1 visibility.
    const char* raw_codec = obs_get_encoder_codec(enc_id.c_str());
    const std::string codec = raw_codec ? raw_codec : "";
    const bool is_hevc = (codec == "hevc");
    const bool is_av1  = (codec == "av1");

    // F-USE1: fetch obs_get_encoder_properties ONCE for this update and reuse
    // it across every populate_combo_from_encoder_property call below. The
    // former implementation re-fetched + re-destroyed the properties object
    // four times (preset / profile / tune / multipass) for the same encoder.
    // May be null (e.g., enc_id empty); helpers handle null gracefully.
    obs_properties_t* props =
        enc_id.empty() ? nullptr : obs_get_encoder_properties(enc_id.c_str());

    // ---- Repopulate combos ----

    // Preset: introspect the encoder-appropriate property key.
    // When empty (encoder exposes no preset), hide the row.
    {
        const char* pkey = preset_prop_key(enc_id);
        const bool has_preset = is_x264 || is_nvenc || is_amf || is_qsv;
        if (has_preset) {
            populate_combo_from_encoder_property(
                encoder_preset_combo_, props, pkey, cfg_.encoder_preset);
        }
        set_form_row_visible(form_, encoder_preset_combo_,
                             has_preset && encoder_preset_combo_->count() > 0);
    }

    // Profile: all listed families support profile.
    {
        const bool has_profile = is_x264 || is_nvenc || is_amf || is_qsv || is_vt;
        if (has_profile) {
            populate_combo_from_encoder_property(
                encoder_profile_combo_, props, "profile", cfg_.encoder_profile);
        }
        set_form_row_visible(form_, encoder_profile_combo_,
                             has_profile && encoder_profile_combo_->count() > 0);
    }

    // Tune: x264 and NVENC only.
    {
        const bool has_tune = is_x264 || is_nvenc;
        if (has_tune) {
            populate_combo_from_encoder_property(
                encoder_tune_combo_, props, "tune", cfg_.encoder_tune, true);
            // add_empty_first=true adds "(encoder default)" at top for tune,
            // which lets users deselect a tune without picking a specific one.
        }
        set_form_row_visible(form_, encoder_tune_combo_,
                             has_tune && encoder_tune_combo_->count() > 0);
    }

    // Multipass: NVENC only.
    {
        if (is_nvenc) {
            populate_combo_from_encoder_property(
                multipass_combo_, props, "multipass", cfg_.multipass);
        }
        set_form_row_visible(form_, multipass_combo_,
                             is_nvenc && multipass_combo_->count() > 0);
    }

    // F-USE1: destroy the hoisted properties object exactly once after all
    // combo populations are done. Safe to call on null.
    if (props) obs_properties_destroy(props);

    // ---- Main form visibility ----

    // Look-ahead and Psycho AQ: NVENC only. No form label — just toggle widget.
    if (lookahead_check_)  lookahead_check_->setVisible(is_nvenc);
    if (psycho_aq_check_)  psycho_aq_check_->setVisible(is_nvenc);

    // B-frames: x264 and NVENC.
    set_form_row_visible(form_, b_frames_spin_, is_x264 || is_nvenc);

    // GPU index: NVENC and AMF.
    set_form_row_visible(form_, gpu_index_spin_, is_nvenc || is_amf);

    // ---- Advanced section visibility ----

    // Determine whether ANY advanced settings exist for this encoder.
    const bool has_any_advanced =
        is_x264 || is_nvenc || is_amf || is_qsv || is_vt || is_hevc || is_av1;

    // Show/hide the advanced check and box.
    if (advanced_check_) advanced_check_->setVisible(has_any_advanced);
    if (advanced_box_) {
        advanced_box_->setVisible(
            has_any_advanced && advanced_check_ && advanced_check_->isChecked());
    }

    // Advanced: common (max/min QP) — x264, NVENC, AMF.
    const bool has_qp = is_x264 || is_nvenc || is_amf;
    set_form_row_visible(adv_form_, max_qp_spin_, has_qp);
    set_form_row_visible(adv_form_, min_qp_spin_, has_qp);

    // Advanced: x264.
    if (cabac_check_)   cabac_check_->setVisible(is_x264);
    set_form_row_visible(adv_form_, x264opts_edit_, is_x264);
    if (mbtree_check_)  mbtree_check_->setVisible(is_x264);
    set_form_row_visible(adv_form_, aq_mode_combo_, is_x264);

    // Advanced: NVENC.
    if (nvenc_repeat_headers_check_)  nvenc_repeat_headers_check_->setVisible(is_nvenc);
    if (nvenc_force_idr_check_)       nvenc_force_idr_check_->setVisible(is_nvenc);
    if (nvenc_dyn_bitrate_check_)     nvenc_dyn_bitrate_check_->setVisible(is_nvenc);

    // Advanced: AMF.
    if (amf_enforce_hrd_check_)       amf_enforce_hrd_check_->setVisible(is_amf);
    if (amf_vbaq_check_)              amf_vbaq_check_->setVisible(is_amf);
    if (amf_pre_analysis_check_)      amf_pre_analysis_check_->setVisible(is_amf);
    if (amf_enable_throughput_check_) amf_enable_throughput_check_->setVisible(is_amf);

    // Advanced: QSV.
    set_form_row_visible(adv_form_, qsv_async_depth_spin_, is_qsv);
    set_form_row_visible(adv_form_, qsv_latency_combo_,    is_qsv);

    // Advanced: VideoToolbox.
    if (vt_realtime_check_)           vt_realtime_check_->setVisible(is_vt);
    set_form_row_visible(adv_form_, vt_frames_before_start_spin_, is_vt);

    // Advanced: HEVC/AV1.
    set_form_row_visible(adv_form_, hevc_tier_combo_,     is_hevc);
    set_form_row_visible(adv_form_, av1_tile_cols_spin_,  is_av1);
    set_form_row_visible(adv_form_, av1_tile_rows_spin_,  is_av1);
}

void SlotEditor::on_replay_max_size_inputs_changed()
{
    SceneSlot::Config preview;
    preview.width = (uint32_t)(w_spin_->value() & ~1u);
    preview.height = (uint32_t)(h_spin_->value() & ~1u);
    if (preview.width  < 64) preview.width  = 64;
    if (preview.height < 64) preview.height = 64;
    preview.fps_num = (uint32_t)fps_num_spin_->value();
    preview.fps_den = (uint32_t)fps_den_spin_->value();
    if (preview.fps_den == 0) preview.fps_den = 1;
    preview.rate_control = rc_combo_->currentData().toString().toStdString();
    if (preview.rate_control.empty()) preview.rate_control = "CBR";
    preview.rc_value = (uint32_t)rc_value_spin_->value();
    preview.audio_bitrate = (uint32_t)abitrate_spin_->value();
    preview.audio_tracks = 0;
    for (int i = 0; i < 6; ++i)
        if (track_checks_[i] && track_checks_[i]->isChecked())
            preview.audio_tracks |= (1u << i);
    if (preview.audio_tracks == 0) preview.audio_tracks = 0x01;
    preview.replay_seconds = (uint32_t)replay_secs_->value();
    preview.replay_max_size_mb = (uint32_t)replay_max_size_spin_->value();
    preview.shared_encoder_slot_id = cfg_.shared_encoder_slot_id;

    EffectiveRC eff;
    if (!preview.shared_encoder_slot_id.empty())
        eff = SlotManager::instance().effective_rate_control(preview);
    else
        eff = {preview.rate_control, preview.rc_value, false, ""};

    bool was_clamped = false;
    uint64_t requested_mb = 0;
    uint64_t resolved = replay_buffer_util::resolve_max_size_mb(
        preview, eff, &was_clamped, &requested_mb);

    if (resolved == 0) {
        replay_max_size_label_->setText(
            QString("(would be declined \xe2\x80\x94 host RAM too low)"));
        replay_max_size_label_->setStyleSheet("color: rgb(220, 140, 60);");
    } else if (preview.replay_max_size_mb > 0) {
        uint64_t auto_mb = replay_buffer_util::auto_derived_max_size_mb(preview, eff);
        replay_max_size_label_->setText(
            QString("(set: %1 MB \xe2\x80\x94 auto would be %2 MB)")
                .arg(resolved).arg(auto_mb));
        replay_max_size_label_->setStyleSheet("");
    } else {
        replay_max_size_label_->setText(
            QString("(auto: %1 MB)").arg(resolved));
        replay_max_size_label_->setStyleSheet("");
    }
}

void SlotEditor::update_shared_encoder_visibility()
{
    const QString data = venc_combo_->currentData().toString();
    const bool is_shared = data.startsWith("shared:");

    // When shared: hide all video pipeline settings EXCEPT rate-control rows.
    // FR-005: rate-control rows stay visible for consumers, rendered read-only
    // and labeled with the owner's name so the user knows where the settings
    // come from (information gained, no control lost).

    // Resolution row
    set_form_row_visible(form_, w_spin_->parentWidget(), !is_shared);
    // FPS row
    set_form_row_visible(form_, fps_num_spin_->parentWidget(), !is_shared);
    // Scene (dependent has no view, scene is meaningless)
    set_form_row_visible(form_, scene_combo_, !is_shared);

    // Rate control + value: keep visible; render read-only inherited content
    // when is_shared, restore editable state otherwise.
    set_form_row_visible(form_, rc_combo_, true);
    set_form_row_visible(form_, rc_value_spin_, true);
    if (is_shared) {
        auto eff = SlotManager::instance().effective_rate_control(cfg_);

        // Label shape (canonical strings — see contracts/rate-control-coherence.md):
        //   normal (owner resolved): "(inherited from <name>)"
        //   orphan  (owner missing): "(inherited — owner missing)"
        //   fallback active        : append " [CBR fallback]"
        QString suffix;
        if (!eff.owner_slot_name.empty())
            suffix = QString(" (inherited from %1)")
                         .arg(QString::fromStdString(eff.owner_slot_name));
        else
            suffix = QString(" (inherited — owner missing)");
        if (eff.fallback)
            suffix += " [CBR fallback]";

        // Rate-control combo: single disabled item showing the owner's mode.
        loading_ = true;
        rc_combo_->clear();
        rc_combo_->addItem(QString::fromStdString(eff.mode),
                           QString::fromStdString(eff.mode));
        rc_combo_->setCurrentIndex(0);
        rc_combo_->setEnabled(false);
        loading_ = false;
        if (auto* lbl = qobject_cast<QLabel*>(form_->labelForField(rc_combo_)))
            lbl->setText(tr("Rate control") + suffix);

        // Value spinbox: [V,V] read-only, or 0+specialValueText for Lossless.
        if (rc_util::is_lossless(eff.mode)) {
            rc_value_spin_->setSpecialValueText(tr("— (lossless)"));
            rc_value_spin_->setRange(0, 0);
            rc_value_spin_->setValue(0);
        } else {
            rc_value_spin_->setSpecialValueText("");
            rc_value_spin_->setRange((int)eff.value, (int)eff.value);
            rc_value_spin_->setValue((int)eff.value);
        }
        rc_value_spin_->setEnabled(false);
        if (rc_value_label_) rc_value_label_->setText(tr("Value") + suffix);
    } else {
        // Standalone slot: re-enable widgets and restore plain row labels.
        // populate_rate_control_combo + update_rc_value_field have already
        // re-populated the rows for the newly-selected encoder (via the
        // !is_shared branch of on_encoder_changed, or via the ctor's initial
        // pass for a slot opened as standalone), so the combo content is
        // already correct here.
        rc_combo_->setEnabled(true);
        rc_value_spin_->setEnabled(true);
        if (auto* lbl = qobject_cast<QLabel*>(form_->labelForField(rc_combo_)))
            lbl->setText(tr("Rate control"));
        // rc_value_label_'s text is restored by update_rc_value_field to the
        // mode-appropriate "Bitrate" / "Quality (lower = better)".
    }

    // All video encoder settings
    set_form_row_visible(form_, keyframe_sec_spin_, !is_shared);
    set_form_row_visible(form_, encoder_preset_combo_, !is_shared);
    set_form_row_visible(form_, encoder_profile_combo_, !is_shared);
    set_form_row_visible(form_, encoder_tune_combo_, !is_shared);
    set_form_row_visible(form_, multipass_combo_, !is_shared);
    if (lookahead_check_) lookahead_check_->setVisible(!is_shared);
    if (psycho_aq_check_) psycho_aq_check_->setVisible(!is_shared);
    set_form_row_visible(form_, b_frames_spin_, !is_shared);
    set_form_row_visible(form_, gpu_index_spin_, !is_shared);

    // Advanced section
    if (advanced_check_) advanced_check_->setVisible(!is_shared);
    if (advanced_box_) advanced_box_->setVisible(!is_shared && advanced_check_ && advanced_check_->isChecked());

    // When NOT shared, restore correct per-encoder visibility.
    if (!is_shared)
        update_encoder_specific_ui();
}
