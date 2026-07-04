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
	// Feature 019 (FR-001): leftmost enable/disable checkbox; every other
	// column shifts one right. This enum is the single source of column
	// indices, so the shift is complete here.
	COL_ENABLED = 0,
	COL_STATE,
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

static QTableWidgetItem *mk_item(const QString &s)
{
	auto *it = new QTableWidgetItem(s);
	it->setFlags(it->flags() & ~Qt::ItemIsEditable);
	return it;
}

static QString fmt_bytes_rate(double kbps)
{
	if (kbps <= 0.0)
		return "--";
	if (kbps >= 1000.0)
		return QString::number(kbps / 1000.0, 'f', 2) + " Mbps";
	if (kbps >= 100.0)
		return QString::number(kbps, 'f', 0) + " kbps";
	return QString::number(kbps, 'f', 1) + " kbps";
}

// ITEM C: COL_STATE cell-widget styling. Keeps the original visual semantics
// (running = red 220,80,80; stopped = gray 140,140,140) on a flat,
// label-looking button so the only change is that the cell is now clickable.
static QString state_btn_style(bool running)
{
	const char *col = running ? "rgb(220,80,80)" : "rgb(140,140,140)";
	return QString("QPushButton{border:none;background:transparent;"
		       "font-weight:bold;color:%1;}")
		.arg(col);
}

static QString state_btn_text(bool running, bool replay_only)
{
	return running ? (replay_only ? "RPL" : "REC") : "off";
}

MultiSceneRecordDock::MultiSceneRecordDock(QWidget *parent) : QWidget(parent)
{
	table_ = new QTableWidget(0, COL_COUNT, this);
	table_->setHorizontalHeaderLabels(
		{"", "", "Name", "Scene", "Res/FPS", "Encoder", "Frames", "Dropped", "Bitrate", "Replay"});
	table_->verticalHeader()->setVisible(false);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setStretchLastSection(false);
	table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(COL_NAME, QHeaderView::Stretch);
	table_->horizontalHeader()->setSectionResizeMode(COL_SCENE, QHeaderView::Stretch);

	add_btn_ = new QPushButton("Add");
	edit_btn_ = new QPushButton("Edit");
	rm_btn_ = new QPushButton("Remove");
	start_btn_ = new QPushButton("Start selected");
	stop_btn_ = new QPushButton("Stop selected");
	save_btn_ = new QPushButton("Save replay");
	stats_chk_ = new QCheckBox("Show stats");

	auto *row1 = new QHBoxLayout;
	row1->addWidget(add_btn_);
	row1->addWidget(edit_btn_);
	row1->addWidget(rm_btn_);

	auto *row2 = new QHBoxLayout;
	row2->addWidget(start_btn_);
	row2->addWidget(stop_btn_);
	row2->addWidget(save_btn_);
	row2->addStretch();
	row2->addWidget(stats_chk_);

	auto *lay = new QVBoxLayout(this);
	lay->addWidget(table_);
	lay->addLayout(row1);
	lay->addLayout(row2);

	connect(add_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_add);
	connect(edit_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_edit);
	connect(rm_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_remove);
	connect(start_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_start_selected);
	connect(stop_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_stop_selected);
	connect(save_btn_, &QPushButton::clicked, this, &MultiSceneRecordDock::on_save_replay);
	connect(stats_chk_, &QCheckBox::toggled, this, &MultiSceneRecordDock::on_stats_toggled);
	connect(table_, &QTableWidget::cellDoubleClicked, this, &MultiSceneRecordDock::on_cell_double_clicked);

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

	refresh();
}

int MultiSceneRecordDock::current_row() const
{
	return table_->currentRow();
}

void MultiSceneRecordDock::stop_timer()
{
	if (stats_timer_)
		stats_timer_->stop();
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
	auto &mgr = SlotManager::instance();
	auto snap = mgr.snapshot_slots();
	last_generation_ = snap.generation;
	const int n = (int)snap.items.size();

	table_->setRowCount(n);

	// O-001: full rebuild — reset the render cache; it is refilled row by
	// row below as the base rendering is applied.
	row_cache_.assign((size_t)n, RowRenderCache{});

	auto set_text = [this](int row, int col, const QString &text) {
		if (auto *it = table_->item(row, col)) {
			it->setText(text);
		} else {
			table_->setItem(row, col, mk_item(text));
		}
	};

	for (int i = 0; i < n; ++i) {
		SceneSlot *s = snap.items[i].get();
		if (!s)
			continue;
		const auto &c = s->config();

		const bool running = s->is_running();
		const bool enabled = c.enabled;

		// Feature 019 (research D6): leftmost enable/disable checkbox.
		// Same cell-widget reuse discipline as the COL_STATE button below.
		// Programmatic sync happens with signals blocked so rebuilds never
		// re-enter the toggle handler — every user toggle ends in
		// refresh(), which re-reads config().enabled and repaints the true
		// state (rapid-toggle convergence).
		{
			auto *wrap = table_->cellWidget(i, COL_ENABLED);
			QCheckBox *cb = wrap ? wrap->findChild<QCheckBox *>() : nullptr;
			if (!cb) {
				wrap = new QWidget;
				auto *cbl = new QHBoxLayout(wrap);
				cbl->setContentsMargins(0, 0, 0, 0);
				cbl->setAlignment(Qt::AlignCenter);
				cb = new QCheckBox(wrap);
				cbl->addWidget(cb);
				connect(cb, &QCheckBox::toggled, this,
					[this, i](bool on) { on_enabled_toggled(i, on); });
				table_->setCellWidget(i, COL_ENABLED, wrap);
			}
			cb->blockSignals(true);
			cb->setChecked(enabled);
			cb->blockSignals(false);
		}

		// ITEM C: COL_STATE is a clickable per-row start/stop toggle. Reuse
		// the existing cell widget when present (F2): only allocate a new
		// QPushButton when the row was just added. The lambda captures `i`
		// by value and remains valid across refreshes because setRowCount()
		// truncates extra rows rather than shifting — rows above a deletion
		// never move, rows below are dropped, so a row's index never changes
		// for the lifetime of its button.
		{
			// Feature 019 (FR-012): a disabled row's button shows "off" in
			// the stopped style but STAYS clickable — a click routes to
			// start(), whose gate ignores it with the FR-004 log line.
			auto *sb = qobject_cast<QPushButton *>(table_->cellWidget(i, COL_STATE));
			if (sb) {
				sb->setText(state_btn_text(running && enabled, c.replay_only));
				sb->setStyleSheet(state_btn_style(running && enabled));
			} else {
				sb = new QPushButton(state_btn_text(running && enabled, c.replay_only));
				sb->setFlat(true);
				sb->setCursor(Qt::PointingHandCursor);
				sb->setStyleSheet(state_btn_style(running && enabled));
				connect(sb, &QPushButton::clicked, this, [this, i]() { on_state_clicked(i); });
				table_->setCellWidget(i, COL_STATE, sb);
			}
		}
		set_text(i, COL_NAME, QString::fromStdString(c.name));
		set_text(i, COL_SCENE, QString::fromStdString(c.scene_name));
		// Feature 019 (FR-012): disabled rows dim name/scene; re-enabled
		// rows must reset to the default palette because items are reused
		// across refreshes.
		for (int col : {COL_NAME, COL_SCENE}) {
			if (auto *it = table_->item(i, col)) {
				if (enabled)
					it->setData(Qt::ForegroundRole, QVariant());
				else
					it->setForeground(QBrush(QColor(140, 140, 140)));
			}
		}
		set_text(i, COL_RES, QString("%1x%2 @ %3").arg(c.width).arg(c.height).arg(c.fps_num));
		QString enc_display;
		if (!c.shared_encoder_slot_id.empty()) {
			std::string primary_name = mgr.slot_name_by_id(c.shared_encoder_slot_id);
			enc_display = primary_name.empty() ? QString::fromUtf8("\xe2\x86\x92 [deleted]")
							   : QString::fromUtf8("\xe2\x86\x92 ") +
								     QString::fromStdString(primary_name);
		} else {
			enc_display = QString::fromStdString(c.video_encoder_id);
		}
		set_text(i, COL_ENC, enc_display);
		// O-001: apply the base (non-fallback) foreground here so a
		// no-transition tick can skip styling entirely; record what this
		// row now shows.
		if (auto *enc = table_->item(i, COL_ENC))
			enc->setForeground(QBrush(QColor(140, 140, 140)));
		{
			RowRenderCache &rc = row_cache_[(size_t)i];
			rc.running = running;
			rc.replay_only = c.replay_only;
			rc.enabled = enabled;
			rc.enc_display = enc_display;
			rc.fallback = false;
			rc.dropped_warn = false;
		}
		set_text(i, COL_FRAMES, "--");
		set_text(i, COL_DROPPED, "--");
		// O-001: base (non-warning) dropped-frames foreground, matching
		// rc.dropped_warn = false above.
		if (auto *dr = table_->item(i, COL_DROPPED))
			dr->setForeground(QBrush(QColor(140, 140, 140)));
		set_text(i, COL_KBPS, "--");
		set_text(i, COL_REPLAY, c.replay_enabled ? QString("%1s").arg(c.replay_seconds) : QString("--"));
	}

	// F1: gate the 1 Hz stats QTimer on real activity. refresh() runs after
	// every state transition (add/remove/edit/start/stop, hotkey toggle,
	// external stop via slot.cpp's queued invokeMethod), so the timer stays
	// in sync without per-event bookkeeping. When stats are disabled or no
	// slot is running, the timer is paused so stopped slots cost nothing.
	if (stats_enabled_ && mgr.any_running()) {
		if (!stats_timer_->isActive())
			stats_timer_->start();
	} else {
		if (stats_timer_->isActive())
			stats_timer_->stop();
	}

	refresh_stats();
}

void MultiSceneRecordDock::refresh_stats()
{
	if (!stats_enabled_)
		return;
	auto &mgr = SlotManager::instance();

	auto snap = mgr.snapshot_slots();
	if (snap.generation != last_generation_) {
		refresh();
		return;
	}

	const int n = (int)snap.items.size();
	if (table_->rowCount() != n) {
		refresh();
		return;
	}

	for (int i = 0; i < n; ++i) {
		SceneSlot *s = snap.items[i].get();
		if (!s)
			continue;
		const auto &c = s->config();
		SceneSlot::Stats st = s->stats();

		// O-001: the 1 Hz tick only STYLES and resolves owner names on
		// transitions. Cheap inputs (is_running, encoder_fallback,
		// dropped>0) are compared against the per-row cache; when nothing
		// changed, no setStyleSheet/setForeground/slot_name_by_id runs.
		// Renames and list changes funnel through refresh(), which rebuilds
		// the cache, so those repaint correctly on their transition tick.
		if ((size_t)i >= row_cache_.size()) {
			refresh();
			return;
		}
		RowRenderCache &rc = row_cache_[(size_t)i];

		// Feature 019 (O-001): disabled rows can't run — skip the 1 Hz
		// styling work entirely; refresh()'s base "--" rendering stands.
		if (!rc.enabled)
			continue;

		const bool running = s->is_running();

		// ITEM C: COL_STATE is now a cell widget, not a QTableWidgetItem.
		// Mutate the existing button in place (no rebuild) so the polled
		// stats tick keeps the toggle's label/color in sync with true state.
		if (running != rc.running || c.replay_only != rc.replay_only) {
			if (auto *sb = qobject_cast<QPushButton *>(table_->cellWidget(i, COL_STATE))) {
				sb->setText(state_btn_text(running, c.replay_only));
				sb->setStyleSheet(state_btn_style(running));
				rc.running = running;
				rc.replay_only = c.replay_only;
			}
		}

		if (st.encoder_fallback != rc.fallback) {
			if (auto *enc = table_->item(i, COL_ENC)) {
				QString base;
				if (!c.shared_encoder_slot_id.empty()) {
					std::string pn = mgr.slot_name_by_id(c.shared_encoder_slot_id);
					base = pn.empty() ? QString::fromUtf8("\xe2\x86\x92 [deleted]")
							  : QString::fromUtf8("\xe2\x86\x92 ") +
								    QString::fromStdString(pn);
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
				rc.enc_display = base;
				rc.fallback = st.encoder_fallback;
			}
		}
		if (auto *it = table_->item(i, COL_FRAMES))
			it->setText(running ? QString::number(st.total_frames) : "--");
		if (auto *it = table_->item(i, COL_DROPPED)) {
			it->setText(running ? QString::number(st.dropped_frames) : "--");
			const bool dropped_warn = st.dropped_frames > 0;
			if (dropped_warn != rc.dropped_warn) {
				it->setForeground(QBrush(dropped_warn ? QColor(220, 140, 60) : QColor(140, 140, 140)));
				rc.dropped_warn = dropped_warn;
			}
		}
		if (auto *it = table_->item(i, COL_KBPS))
			it->setText(s->is_running() ? fmt_bytes_rate(st.kbps) : "--");

		if (auto *it = table_->item(i, COL_REPLAY)) {
			if (!c.replay_enabled)
				it->setText("--");
			else if (st.replay_active)
				it->setText(QString("armed %1s").arg(c.replay_seconds));
			else
				it->setText(QString("off %1s").arg(c.replay_seconds));
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
	if (row < 0)
		return;
	auto *s = SlotManager::instance().slot_at((size_t)row);
	if (!s)
		return;
	// F-005: capture the slot's stable identity BEFORE the modal. The slot
	// list can be rebuilt while the dialog is open (scene-collection switch,
	// second control path), so the row index must not be trusted afterwards:
	// the manager re-resolves the id atomically and refuses when it's gone.
	const std::string slot_id = s->config().id;
	SlotEditor dlg(this, s->config());
	if (dlg.exec() == QDialog::Accepted) {
		if (!SlotManager::instance().update_slot_by_id(slot_id, dlg.result())) {
			QMessageBox::warning(this, tr("Slots changed"),
					     tr("Slots changed while the dialog was open — no changes applied."));
		}
		refresh();
	}
}

void MultiSceneRecordDock::on_remove()
{
	int row = current_row();
	if (row < 0)
		return;
	auto *s = SlotManager::instance().slot_at((size_t)row);
	if (!s)
		return;
	// F-005: same identity capture as on_edit — the confirmation box is
	// modal, and the row under it can move or vanish before the user clicks.
	const std::string slot_id = s->config().id;
	if (QMessageBox::question(this, "Remove", "Remove this slot?") != QMessageBox::Yes)
		return;
	if (!SlotManager::instance().remove_slot_by_id(slot_id)) {
		QMessageBox::warning(this, tr("Slots changed"),
				     tr("Slots changed while the dialog was open — no changes applied."));
	}
	refresh();
}

void MultiSceneRecordDock::on_start_selected()
{
	// Validate that every ENABLED slot has an output directory configured
	// before attempting to start. A slot with an empty path will silently
	// fail to record but (prior to the slot.cpp fix) appear as running in
	// the UI. Disabled slots are excluded (research D7): they aren't being
	// started, so a missing path there must not block the selected subset.
	auto &mgr = SlotManager::instance();
	auto snap = mgr.snapshot_slots();
	size_t enabled_count = 0;
	QStringList no_path;
	for (const auto &sp : snap.items) {
		if (!sp)
			continue;
		const auto &c = sp->config();
		if (!c.enabled)
			continue;
		++enabled_count;
		if (c.path.empty())
			no_path << QString::fromStdString(c.name);
	}

	// FR-011 (research D7): slots exist but none is enabled — tell the
	// user why nothing will start; no state changes. The hotkey path logs
	// instead (a dialog must never pop from a hotkey).
	if (!snap.items.empty() && enabled_count == 0) {
		QMessageBox::information(this, tr("No slots enabled"),
					 tr("No slots are enabled — check at least one slot to use Start selected."));
		return;
	}

	if (!no_path.isEmpty()) {
		QMessageBox::critical(this, tr("Missing output directory"),
				      tr("The following slots have no output directory configured "
					 "and cannot be started:\n\n%1\n\n"
					 "Edit each slot and set an output directory before starting.")
					      .arg(no_path.join(QStringLiteral("\n"))));
		return;
	}

	mgr.start_selected();
	refresh();
}
void MultiSceneRecordDock::on_stop_selected()
{
	SlotManager::instance().stop_selected();
	refresh();
}

void MultiSceneRecordDock::on_save_replay()
{
	int row = current_row();
	if (row < 0)
		return;
	auto *s = SlotManager::instance().slot_at((size_t)row);
	if (s)
		s->save_replay();
}

void MultiSceneRecordDock::on_cell_double_clicked(int /*row*/, int /*col*/)
{
	on_edit();
}

void MultiSceneRecordDock::on_state_clicked(int row)
{
	auto &mgr = SlotManager::instance();
	SceneSlot *s = mgr.slot_at((size_t)row);
	if (!s)
		return;

	// A cell-widget click does not move the table selection by itself; make
	// this row current so Edit / Remove / Save replay (current_row()) still
	// have a usable target. COL_NAME is a plain item cell (not the button),
	// and with SelectRows this selects the whole row.
	table_->setCurrentCell(row, COL_NAME);

	// Mirror on_record_hotkey's discipline: slot_at() already took and
	// released SlotManager::mtx_, so call start()/stop() with NO manager lock
	// held. Synchronous, exactly like Start all / Stop all; no cascade.
	if (s->is_running())
		s->stop();
	else
		s->start();

	// Reflect the slot's ACTUAL post-toggle state: start() returns false and
	// resets running_ on an empty/missing output path, so a failed start
	// leaves the row showing "off" rather than a fake running state. Reuse
	// the existing refresh() path (same as Start all / Stop all).
	refresh();
}

void MultiSceneRecordDock::on_enabled_toggled(int row, bool checked)
{
	auto &mgr = SlotManager::instance();
	SceneSlot *s = mgr.slot_at((size_t)row);
	if (!s) {
		refresh();
		return;
	}
	// F-005: capture the stable id BEFORE any modal — the slot list can be
	// rebuilt while the confirmation is open, so neither the row index nor
	// the raw pointer may be trusted afterwards.
	const std::string slot_id = s->config().id;

	if (checked) {
		mgr.set_slot_enabled_by_id(slot_id, true);
		refresh();
		return;
	}

	// FR-014: unchecking a recording slot stops it first — with the user's
	// confirmation. Unchecking an idle slot never prompts.
	if (s->is_running()) {
		if (QMessageBox::question(this, tr("Disable slot"),
					  tr("Are you sure you want to disable and stop this slot?")) !=
		    QMessageBox::Yes) {
			// Cancelled: re-sync the checkbox to the slot's actual state.
			refresh();
			return;
		}
	}

	// Re-resolve by id after the (possible) modal; the shared_ptr keeps the
	// slot alive across the stop. stop() is idempotent — a slot that
	// stopped on its own while the prompt was open is a clean no-op.
	auto snap = mgr.snapshot_slots();
	for (const auto &sp : snap.items) {
		if (sp && sp->config().id == slot_id) {
			sp->stop();
			break;
		}
	}
	mgr.set_slot_enabled_by_id(slot_id, false); // refuses if the id vanished
	refresh();
}

void MultiSceneRecordDock::on_stats_toggled(bool on)
{
	stats_enabled_ = on;
	save_pref_stats_enabled(on);
	apply_stats_visibility();

	if (on) {
		// Reset bitrate samplers so the first reading is fresh, not a stale delta.
		auto &mgr = SlotManager::instance();
		for (size_t i = 0; i < mgr.slot_count(); ++i) {
			if (auto *s = mgr.slot_at(i))
				s->reset_stats_sampler();
		}
		if (mgr.any_running())
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
				if (auto *it = table_->item(i, c)) {
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
	table_->setColumnHidden(COL_FRAMES, !show);
	table_->setColumnHidden(COL_DROPPED, !show);
	table_->setColumnHidden(COL_KBPS, !show);
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
