#pragma once

// Forward-declared so slot.cpp can post a UI refresh onto the dock without
// pulling in the full Qt widget definition here.
class MultiSceneRecordDock;

// Returns the live dock instance, or nullptr if it has not been created yet
// (or has been torn down on module unload). Owned by OBS's main window.
// UI-THREAD ONLY: g_dock is written on the UI thread with no synchronization,
// so calling this from any other thread is a data race. Non-UI threads that
// want a dock refresh must go through notify_dock_refresh() instead.
MultiSceneRecordDock *get_dock();

// Queue a dock refresh onto the OBS UI task queue. Safe to call from ANY
// thread: the dock pointer is only read inside the queued task (which runs
// on the UI thread), and a dock destroyed before the task runs is skipped
// via the in-task null check.
void notify_dock_refresh();
