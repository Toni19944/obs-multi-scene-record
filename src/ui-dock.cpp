#include "ui-dock.hpp"
#include "ui-slot-editor.hpp"
#include "manager.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QTimer>
#include <QBrush>
#include <QColor>
#include <QSettings>

enum Col {
    COL_STATE = 0,
    COL_NAME,
    COL_SCENE,
    COL_RES,
    COL_ENC,
    COL_FRAMES,
    COL_DROPPED,
    COL_KBPS,
    COL_REPLAY,
    COL_COUNT
};

static QTableWidgetItem* mk_item(const QString& s)
{
    auto* it = new QTableWidgetItem(s);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    return it;
}

static QString fmt_bytes_rate(double kbps)
{
    if (kbps <= 0.0) return "--";
    if (kbps >= 1000.0) return QString::number(kbps / 1000.0, 'f', 2) + " Mbps";
    return QString::number(kbps, 'f', 0) + " kbps";
}

// ITEM C: COL_STATE cell-widget styling. Keeps the original visual semantics
// (running = red 220,80,80; stopped = gray 140,140,140) on a flat,
// label-looking button so the only change is that the cell is now clickable.
static QString state_btn_style(bool running)
{
    const char* col = running ? "rgb(220,80,80)" : "rgb(140,140,140)";
    return QString("QPushButton{border:none;background:transparent;"
                   "font-weight:bold;color:%1;}").arg(col);
}

static QString state_btn_text(bool running, bool replay_only)
{
    return running ? (replay_only ? "RPL" : "REC") : "off";
}

MultiSceneRecordDock::MultiSceneRecordDock(QWidget* parent) : QWidget(parent)
{
    table_ = new QTableWidget(0, COL_COUNT, this);
    table_->setHorizontalHeaderLabels(
        {"", "Name", "Scene", "Res/FPS", "Encoder",
         "Frames", "Dropped", "Bitrate", "Replay"});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(COL_NAME,  QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(COL_SCENE, QHeaderView::Stretch);

    add_btn_   = new QPushButton("Add");
    edit_btn_  = new QPushButton("Edit");
    rm_btn_    = new QPushButton("Remove");
    start_btn_ = new QPushButton("Start all");
    stop_btn_  = new QPushButton("Stop all");
    save_btn_  = new QPushButton("Save replay");
    stats_chk_ = new QCheckBox("Show stats");

    auto* row1 = new QHBoxLayout;
    row1->addWidget(add_btn_); row1->addWidget(edit_btn_); row1->addWidget(rm_btn_);

    auto* row2 = new QHBoxLayout;
    row2->addWidget(start_btn_); row2->addWidget(stop_btn_); row2->addWidget(save_btn_);
    row2->addStretch();
    row2->addWidget(stats_chk_);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(table_);
    lay->addLayout(row1);
    lay->addLayout(row2);

    connect(add_btn_,   &QPushButton::clicked, this, &MultiSceneRecordDock::on_add);
    connect(edit_btn_,  &QPushButton::clicked, this, &MultiSceneRecordDock::on_edit);
    connect(rm_btn_,    &QPushButton::clicked, this, &MultiSceneRecordDock::on_remove);
    connect(start_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_start_all);
    connect(stop_btn_,  &QPushButton::clicked, this, &MultiSceneRecordDock::on_stop_all);
    connect(save_btn_,  &QPushButton::clicked, this, &MultiSceneRecordDock::on_save_replay);
    connect(stats_chk_, &QCheckBox::toggled,   this, &MultiSceneRecordDock::on_stats_toggled);
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &MultiSceneRecordDock::on_cell_double_clicked);

    stats_timer_ = new QTimer(this);
    stats_timer_->setInterval(1000);
    connect(stats_timer_, &QTimer::timeout, this, &MultiSceneRecordDock::refresh_stats);

    // Restore user preference. Default ON. blockSignals so the initial
    // setChecked doesn't bounce through on_stats_toggled.
    stats_enabled_ = load_pref_stats_enabled();
    stats_chk_->blockSignals(true);
    stats_chk_->setChecked(stats_enabled_);
    stats_chk_->blockSignals(false);
    apply_stats_visibility();
    if (stats_enabled_) stats_timer_->start();

    refresh();
}

int MultiSceneRecordDock::current_row() const
{
    return table_->currentRow();
}

void MultiSceneRecordDock::stop_timer()
{
    if (stats_timer_) stats_timer_->stop();
}

MultiSceneRecordDock::~MultiSceneRecordDock()
{
    // Stop the polling timer explicitly so it cannot fire after our members
    // are in a partially-destroyed state. Qt will then delete the timer object
    // itself (it is a child widget), so we do not delete it here.
    stop_timer();
}

void MultiSceneRecordDock::refresh()
{
    auto& mgr = SlotManager::instance();
    // Sync the generation so the refresh_stats() that runs at the end of
    // this function (and the next polled one) does not re-trigger a rebuild.
    last_generation_ = mgr.generation();
    const int n = (int)mgr.slot_count();

    table_->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        SceneSlot* s = mgr.slot_at((size_t)i);
        if (!s) continue;
        const auto& c = s->config();

        // ITEM C: COL_STATE is a clickable per-row start/stop toggle. Use a
        // flat button cell widget (recreated on each discrete refresh; the
        // 1 s stats poll mutates it in place via refresh_stats(), it does not
        // rebuild). A cell widget also naturally resolves the single/double-
        // click conflict: the button consumes its own clicks so a double-
        // click here does NOT reach QTableWidget::cellDoubleClicked, leaving
        // double-click-to-edit intact for every OTHER column.
        {
            const bool running = s->is_running();
            auto* sb = new QPushButton(
                state_btn_text(running, c.replay_only));
            sb->setFlat(true);
            sb->setCursor(Qt::PointingHandCursor);
            sb->setStyleSheet(state_btn_style(running));
            connect(sb, &QPushButton::clicked, this,
                    [this, i]() { on_state_clicked(i); });
            table_->setCellWidget(i, COL_STATE, sb);
        }
        table_->setItem(i, COL_NAME,   mk_item(QString::fromStdString(c.name)));
        table_->setItem(i, COL_SCENE,  mk_item(QString::fromStdString(c.scene_name)));
        table_->setItem(i, COL_RES,
            mk_item(QString("%1x%2 @ %3").arg(c.width).arg(c.height).arg(c.fps_num)));
        if (!c.shared_encoder_slot_id.empty()) {
            std::string primary_name = SlotManager::instance().slot_name_by_id(c.shared_encoder_slot_id);
            QString display = primary_name.empty()
                ? QString::fromUtf8("\xe2\x86\x92 [deleted]")
                : QString::fromUtf8("\xe2\x86\x92 ") + QString::fromStdString(primary_name);
            table_->setItem(i, COL_ENC, mk_item(display));
        } else {
            table_->setItem(i, COL_ENC, mk_item(QString::fromStdString(c.video_encoder_id)));
        }
        table_->setItem(i, COL_FRAMES, mk_item("--"));
        table_->setItem(i, COL_DROPPED, mk_item("--"));
        table_->setItem(i, COL_KBPS,   mk_item("--"));
        table_->setItem(i, COL_REPLAY,
            mk_item(c.replay_enabled
                    ? QString("%1s").arg(c.replay_seconds)
                    : QString("--")));
    }
    refresh_stats();
}

void MultiSceneRecordDock::refresh_stats()
{
    if (!stats_enabled_) return;
    auto& mgr = SlotManager::instance();

    // If slots_ was rebuilt (e.g. scene-collection load on another thread),
    // any cached SceneSlot* is stale. Rebuild the whole table instead of
    // poking per-row stats. refresh() resyncs last_generation_.
    const size_t gen = mgr.generation();
    if (gen != last_generation_) { refresh(); return; }

    const int n = (int)mgr.slot_count();
    if (table_->rowCount() != n) { refresh(); return; }

    for (int i = 0; i < n; ++i) {
        SceneSlot* s = mgr.slot_at((size_t)i);
        if (!s) continue;
        SceneSlot::Stats st = s->stats();

        // ITEM C: COL_STATE is now a cell widget, not a QTableWidgetItem.
        // Mutate the existing button in place (no rebuild) so the polled
        // stats tick keeps the toggle's label/color in sync with true state.
        if (auto* sb = qobject_cast<QPushButton*>(
                table_->cellWidget(i, COL_STATE))) {
            const bool running = s->is_running();
            sb->setText(state_btn_text(running, s->config().replay_only));
            sb->setStyleSheet(state_btn_style(running));
        }

        // Encoder column: warn when the configured encoder was unavailable
        // and we silently fell back to obs_x264/CBR.
        if (auto* enc = table_->item(i, COL_ENC)) {
            const auto& c = s->config();
            QString base;
            if (!c.shared_encoder_slot_id.empty()) {
                std::string pn = mgr.slot_name_by_id(c.shared_encoder_slot_id);
                base = pn.empty()
                    ? QString::fromUtf8("\xe2\x86\x92 [deleted]")
                    : QString::fromUtf8("\xe2\x86\x92 ") + QString::fromStdString(pn);
            } else {
                base = QString::fromStdString(c.video_encoder_id);
            }
            if (st.encoder_fallback) {
                enc->setText(base + " [CBR fallback]");
                enc->setForeground(QBrush(QColor(220, 140, 60)));
            } else {
                enc->setText(base);
                enc->setForeground(QBrush(QColor(140, 140, 140)));
            }
        }
        if (auto* it = table_->item(i, COL_FRAMES))
            it->setText(s->is_running() ? QString::number(st.total_frames) : "--");
        if (auto* it = table_->item(i, COL_DROPPED)) {
            it->setText(s->is_running() ? QString::number(st.dropped_frames) : "--");
            it->setForeground(QBrush(st.dropped_frames > 0
                                     ? QColor(220, 140, 60)
                                     : QColor(140, 140, 140)));
        }
        if (auto* it = table_->item(i, COL_KBPS))
            it->setText(s->is_running() ? fmt_bytes_rate(st.kbps) : "--");

        // Replay column: show "armed" if replay output is actively buffering.
        if (auto* it = table_->item(i, COL_REPLAY)) {
            const auto& c = s->config();
            if (!c.replay_enabled) it->setText("--");
            else if (st.replay_active) it->setText(QString("armed %1s").arg(c.replay_seconds));
            else it->setText(QString("off %1s").arg(c.replay_seconds));
        }
    }
}

void MultiSceneRecordDock::on_add()
{
    SceneSlot::Config c;
    c.name = "Slot " + std::to_string(SlotManager::instance().slot_count() + 1);
    SlotEditor dlg(this, c);
    if (dlg.exec() == QDialog::Accepted) {
        SlotManager::instance().add_slot(dlg.result());
        refresh();
    }
}

void MultiSceneRecordDock::on_edit()
{
    int row = current_row();
    if (row < 0) return;
    auto* s = SlotManager::instance().slot_at((size_t)row);
    if (!s) return;
    SlotEditor dlg(this, s->config());
    if (dlg.exec() == QDialog::Accepted) {
        SlotManager::instance().update_slot((size_t)row, dlg.result());
        refresh();
    }
}

void MultiSceneRecordDock::on_remove()
{
    int row = current_row();
    if (row < 0) return;
    if (QMessageBox::question(this, "Remove", "Remove this slot?") != QMessageBox::Yes) return;
    SlotManager::instance().remove_slot((size_t)row);
    refresh();
}

void MultiSceneRecordDock::on_start_all()
{
    // Validate that every slot has an output directory configured before
    // attempting to start. A slot with an empty path will silently fail to
    // record but (prior to the slot.cpp fix) appear as running in the UI.
    auto& mgr = SlotManager::instance();
    QStringList no_path;
    for (size_t i = 0; i < mgr.slot_count(); ++i) {
        const SceneSlot* s = mgr.slot_at(i);
        if (s && s->config().path.empty())
            no_path << QString::fromStdString(s->config().name);
    }
    if (!no_path.isEmpty()) {
        QMessageBox::critical(
            this,
            tr("Missing output directory"),
            tr("The following slots have no output directory configured "
               "and cannot be started:\n\n%1\n\n"
               "Edit each slot and set an output directory before starting.")
                .arg(no_path.join(QStringLiteral("\n"))));
        return;
    }

    mgr.start_all();
    refresh();
}
void MultiSceneRecordDock::on_stop_all()  { SlotManager::instance().stop_all();  refresh(); }

void MultiSceneRecordDock::on_save_replay()
{
    int row = current_row();
    if (row < 0) return;
    auto* s = SlotManager::instance().slot_at((size_t)row);
    if (s) s->save_replay();
}

void MultiSceneRecordDock::on_cell_double_clicked(int /*row*/, int /*col*/) { on_edit(); }

void MultiSceneRecordDock::on_state_clicked(int row)
{
    auto& mgr = SlotManager::instance();
    SceneSlot* s = mgr.slot_at((size_t)row);
    if (!s) return;

    // A cell-widget click does not move the table selection by itself; make
    // this row current so Edit / Remove / Save replay (current_row()) still
    // have a usable target. COL_NAME is a plain item cell (not the button),
    // and with SelectRows this selects the whole row.
    table_->setCurrentCell(row, COL_NAME);

    // Mirror on_record_hotkey's discipline: slot_at() already took and
    // released SlotManager::mtx_, so call start()/stop() with NO manager lock
    // held. Synchronous, exactly like Start all / Stop all; no cascade.
    if (s->is_running()) s->stop();
    else                 s->start();

    // Reflect the slot's ACTUAL post-toggle state: start() returns false and
    // resets running_ on an empty/missing output path, so a failed start
    // leaves the row showing "off" rather than a fake running state. Reuse
    // the existing refresh() path (same as Start all / Stop all).
    refresh();
}

void MultiSceneRecordDock::on_stats_toggled(bool on)
{
    stats_enabled_ = on;
    save_pref_stats_enabled(on);
    apply_stats_visibility();

    if (on) {
        // Reset bitrate samplers so the first reading is fresh, not a stale delta.
        auto& mgr = SlotManager::instance();
        for (size_t i = 0; i < mgr.slot_count(); ++i) {
            if (auto* s = mgr.slot_at(i)) s->reset_stats_sampler();
        }
        stats_timer_->start();
        // First tick immediately so the user sees fresh values without a 1s wait.
        // (We still need the second tick before bitrate is non-zero since it's
        // a delta; this just populates frames/dropped without delay.)
        refresh_stats();
    } else {
        stats_timer_->stop();
        // Clear stat cell text so it's clear that values are no longer live.
        const int rows = table_->rowCount();
        for (int i = 0; i < rows; ++i) {
            for (int c : {COL_FRAMES, COL_DROPPED, COL_KBPS}) {
                if (auto* it = table_->item(i, c)) {
                    it->setText("--");
                    it->setForeground(QBrush(QColor(140, 140, 140)));
                }
            }
        }
    }
}

void MultiSceneRecordDock::apply_stats_visibility()
{
    const bool show = stats_enabled_;
    table_->setColumnHidden(COL_FRAMES,  !show);
    table_->setColumnHidden(COL_DROPPED, !show);
    table_->setColumnHidden(COL_KBPS,    !show);
    // State + Replay columns stay visible — they're driven by refresh() on
    // discrete events (start/stop/add/remove) and don't require polling.
}

void MultiSceneRecordDock::save_pref_stats_enabled(bool on)
{
    QSettings s("OBS Studio", "multi-scene-record");
    s.setValue("dock/stats_enabled", on);
}

bool MultiSceneRecordDock::load_pref_stats_enabled() const
{
    QSettings s("OBS Studio", "multi-scene-record");
    return s.value("dock/stats_enabled", true).toBool();
}
