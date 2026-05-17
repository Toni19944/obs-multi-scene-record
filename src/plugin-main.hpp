#pragma once

// Forward-declared so slot.cpp can post a UI refresh onto the dock without
// pulling in the full Qt widget definition here.
class MultiSceneRecordDock;

// Returns the live dock instance, or nullptr if it has not been created yet
// (or has been torn down on module unload). Owned by OBS's main window.
MultiSceneRecordDock* get_dock();
