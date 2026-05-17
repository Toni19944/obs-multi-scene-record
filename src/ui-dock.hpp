#pragma once

#include <QWidget>

#include <cstddef>

class QTableWidget;
class QPushButton;
class QCheckBox;
class QTimer;

class MultiSceneRecordDock : public QWidget {
    Q_OBJECT
public:
    explicit MultiSceneRecordDock(QWidget* parent = nullptr);
    ~MultiSceneRecordDock();

    // Stops the stats polling timer. Called on module unload before the
    // widget is removed so the timer can't fire into unloaded plugin code.
    void stop_timer();

public slots:
    void refresh();         // rebuild rows (slot list changed)
    void refresh_stats();   // update stats columns only

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
    int  current_row() const;
    void apply_stats_visibility();
    void save_pref_stats_enabled(bool on);
    bool load_pref_stats_enabled() const;

    QTableWidget* table_   = nullptr;
    QPushButton*  add_btn_ = nullptr;
    QPushButton*  edit_btn_ = nullptr;
    QPushButton*  rm_btn_   = nullptr;
    QPushButton*  start_btn_= nullptr;
    QPushButton*  stop_btn_ = nullptr;
    QPushButton*  save_btn_ = nullptr;
    QCheckBox*    stats_chk_ = nullptr;
    QTimer*       stats_timer_ = nullptr;

    bool stats_enabled_ = true;

    // Last SlotManager generation observed by refresh_stats(); a mismatch
    // means slots_ was rebuilt and cached SceneSlot* are stale.
    size_t last_generation_ = 0;
};
