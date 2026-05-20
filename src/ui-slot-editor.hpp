#pragma once

#include "slot.hpp"

#include <QDialog>
#include <array>

class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QGroupBox;
class QFormLayout;

class SlotEditor : public QDialog {
    Q_OBJECT
public:
    explicit SlotEditor(QWidget* parent, SceneSlot::Config cfg);
    SceneSlot::Config result() const { return cfg_; }

private slots:
    void on_accept();
    void on_browse_path();
    void on_encoder_changed();   // re-introspect rate control modes
    void on_rc_changed();        // relabel + re-range the value field

private:
    void populate_scene_combo();
    void populate_video_encoder_combo();
    void populate_audio_encoder_combo();
    void populate_rate_control_combo(); // introspects the selected encoder
    void update_rc_value_field();       // introspects the value property
    void update_mp4_warning();          // show/hide MP4 replay warning

    // Returns true if enc_id contains the given substring (case-sensitive).
    // Used to determine encoder family for show/hide logic.
    static bool enc_has(const std::string& enc_id, const char* needle);

    // Returns the property key for the preset of enc_id:
    //   jim_nvenc / jim_hevc_nvenc → "preset2"
    //   qsv variants               → "target_usage"
    //   everything else            → "preset"
    static const char* preset_prop_key(const std::string& enc_id);

    // Populates `combo` from a list-type property `prop_key` on a pre-fetched
    // obs_properties_t* `props`. The caller owns `props` (acquired ONCE per
    // update_encoder_specific_ui via obs_get_encoder_properties, destroyed
    // ONCE at the end) -- F-USE1: avoids re-fetching properties per combo.
    // `props` may be null; the combo is then left empty (or with just the
    // empty placeholder when `add_empty_first` is true). Clears the combo
    // first. After population, selects the entry whose data() ==
    // QString::fromStdString(current_val); falls back to index 0 if not
    // found. If `add_empty_first` is true, inserts an "(encoder default)"
    // entry (data="") at index 0 before populating from the property.
    static void populate_combo_from_encoder_property(
        QComboBox* combo,
        obs_properties_t* props,
        const char* prop_key,
        const std::string& current_val,
        bool add_empty_first = false);

    // Hides or shows a row in the given QFormLayout by toggling visibility of
    // both `field` and its associated label (retrieved via fl->labelForField(field)).
    // For form rows added with addRow(QWidget*) only (no label), pass fl = nullptr
    // and only field->setVisible is called.
    static void set_form_row_visible(QFormLayout* fl, QWidget* field, bool visible);

    // Called once after all widgets are constructed and whenever on_encoder_changed fires.
    // Reads the current venc_combo_ selection, determines encoder family and codec,
    // repopulates encoder_preset_combo_ / encoder_profile_combo_ / encoder_tune_combo_ /
    // multipass_combo_, then shows/hides all encoder-specific widgets.
    void update_encoder_specific_ui();
    void update_shared_encoder_visibility();

    SceneSlot::Config cfg_;

    QLineEdit*  name_edit_   = nullptr;
    QComboBox*  scene_combo_ = nullptr;
    QSpinBox*   w_spin_      = nullptr;
    QSpinBox*   h_spin_      = nullptr;
    QSpinBox*   fps_num_spin_ = nullptr;
    QSpinBox*   fps_den_spin_ = nullptr;
    QLineEdit*  path_edit_   = nullptr;
    QComboBox*  container_combo_ = nullptr;
    QComboBox*  venc_combo_   = nullptr;
    QComboBox*  aenc_combo_   = nullptr;   // audio encoder
    QComboBox*  rc_combo_     = nullptr;   // rate control mode
    QLabel*     rc_value_label_ = nullptr;
    QSpinBox*   rc_value_spin_  = nullptr; // bitrate OR quality value
    QSpinBox*   abitrate_spin_  = nullptr;
    QCheckBox*  replay_check_   = nullptr;
    QCheckBox*  replay_only_check_ = nullptr;
    QSpinBox*   replay_secs_    = nullptr;
    QLabel*     replay_mp4_warn_ = nullptr; // MP4-replay caution label
    std::array<QCheckBox*, 6> track_checks_{};

    // Main form layout reference (needed for labelForField visibility toggling).
    QFormLayout* form_ = nullptr;

    // Video encoder — user-configurable section (shown/hidden per encoder family)
    QSpinBox*   keyframe_sec_spin_       = nullptr;
    QComboBox*  encoder_preset_combo_    = nullptr;
    QComboBox*  encoder_profile_combo_   = nullptr;
    QComboBox*  encoder_tune_combo_      = nullptr;
    QComboBox*  multipass_combo_         = nullptr;
    QCheckBox*  lookahead_check_         = nullptr;
    QCheckBox*  psycho_aq_check_         = nullptr;
    QSpinBox*   b_frames_spin_           = nullptr;
    QSpinBox*   gpu_index_spin_          = nullptr;

    // Advanced section
    QCheckBox*  advanced_check_          = nullptr;
    QGroupBox*  advanced_box_            = nullptr;
    QFormLayout* adv_form_               = nullptr;  // inner layout of advanced_box_

    // Advanced — common
    QSpinBox*   max_qp_spin_             = nullptr;
    QSpinBox*   min_qp_spin_             = nullptr;

    // Advanced — x264
    QCheckBox*  cabac_check_             = nullptr;
    QLineEdit*  x264opts_edit_           = nullptr;
    QCheckBox*  mbtree_check_            = nullptr;
    QComboBox*  aq_mode_combo_           = nullptr;

    // Advanced — NVENC
    QCheckBox*  nvenc_repeat_headers_check_ = nullptr;
    QCheckBox*  nvenc_force_idr_check_      = nullptr;
    QCheckBox*  nvenc_dyn_bitrate_check_    = nullptr;

    // Advanced — AMF
    QCheckBox*  amf_enforce_hrd_check_      = nullptr;
    QCheckBox*  amf_vbaq_check_            = nullptr;
    QCheckBox*  amf_pre_analysis_check_    = nullptr;
    QCheckBox*  amf_enable_throughput_check_ = nullptr;

    // Advanced — QSV
    QSpinBox*   qsv_async_depth_spin_    = nullptr;
    QComboBox*  qsv_latency_combo_       = nullptr;

    // Advanced — VideoToolbox
    QCheckBox*  vt_realtime_check_       = nullptr;
    QSpinBox*   vt_frames_before_start_spin_ = nullptr;

    // Advanced — HEVC/AV1 codec-specific
    QComboBox*  hevc_tier_combo_         = nullptr;
    QSpinBox*   av1_tile_cols_spin_      = nullptr;
    QSpinBox*   av1_tile_rows_spin_      = nullptr;

    // Guards so programmatic combo repopulation doesn't recurse through slots.
    bool loading_ = false;
};
