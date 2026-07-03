#pragma once

#include <QWidget>

#include <cstddef>
#include <vector>

class QTableWidget;
class QPushButton;
class QCheckBox;
class QTimer;

class MultiSceneRecordDock : public QWidget {
	Q_OBJECT
public:
	explicit MultiSceneRecordDock(QWidget *parent = nullptr);
	~MultiSceneRecordDock();

	// Stops the stats polling timer. Called on module unload before the
	// widget is removed so the timer can't fire into unloaded plugin code.
	void stop_timer();

public slots:
	void refresh();       // rebuild rows (slot list changed)
	void refresh_stats(); // update stats columns only

private slots:
	void on_add();
	void on_edit();
	void on_remove();
	void on_start_all();
	void on_stop_all();
	void on_save_replay();
	void on_cell_double_clicked(int row, int col);
	void on_stats_toggled(bool on);

private:
	// ITEM C: COL_STATE is a per-row clickable toggle (cell widget). Clicking
	// row `row`'s control starts it if stopped / stops it if running, then
	// refreshes so the row shows the slot's true state.
	void on_state_clicked(int row);

	int current_row() const;
	void apply_stats_visibility();
	void save_pref_stats_enabled(bool on);
	bool load_pref_stats_enabled() const;

	QTableWidget *table_ = nullptr;
	QPushButton *add_btn_ = nullptr;
	QPushButton *edit_btn_ = nullptr;
	QPushButton *rm_btn_ = nullptr;
	QPushButton *start_btn_ = nullptr;
	QPushButton *stop_btn_ = nullptr;
	QPushButton *save_btn_ = nullptr;
	QCheckBox *stats_chk_ = nullptr;
	QTimer *stats_timer_ = nullptr;

	bool stats_enabled_ = true;

	// Last SlotManager generation observed by refresh_stats(); a mismatch
	// means slots_ was rebuilt and cached SceneSlot* are stale.
	size_t last_generation_ = 0;

	// O-001: last-rendered state per row so the 1 Hz stats tick only styles
	// and resolves owner names on actual transitions. refresh() rebuilds the
	// cache (and applies the base rendering), so renames, fallback flips and
	// list changes — which all funnel through refresh() — repaint correctly;
	// steady-state ticks match the cache and touch nothing.
	struct RowRenderCache {
		bool running = false;
		bool replay_only = false;
		QString enc_display; // base encoder text (no fallback tag)
		bool fallback = false;
		bool dropped_warn = false; // orange dropped-frames foreground active
	};
	std::vector<RowRenderCache> row_cache_;
};
