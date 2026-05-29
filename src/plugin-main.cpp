#include <obs.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "manager.hpp"
#include "ui-dock.hpp"
#include "plugin-main.hpp"

#include <QMainWindow>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multi-scene-record", "en-US")

MODULE_EXPORT const char* obs_module_name(void)        { return "Multi-Scene Record"; }
MODULE_EXPORT const char* obs_module_description(void) { return "Independent per-scene recording and replay buffers"; }

static MultiSceneRecordDock* g_dock = nullptr;

MultiSceneRecordDock* get_dock() { return g_dock; }

// Named (non-lambda) so it can be passed to obs_frontend_remove_event_callback
// on module unload. A lambda has no stable address to unregister.
static void dock_create_cb(enum obs_frontend_event event, void*)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING) return;
    if (g_dock) return;
    auto* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
    if (!main_window) return;
    g_dock = new MultiSceneRecordDock(main_window);
    QObject::connect(g_dock, &QObject::destroyed, [](){ g_dock = nullptr; });
    obs_frontend_add_dock_by_id(
        "multi_scene_record_dock", "Multi-Scene Record", g_dock);
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[multi-scene-rec] loading v%s", PLUGIN_VERSION);
    SlotManager::instance().init();

    // Create dock once the frontend is up.
    obs_frontend_add_event_callback(&dock_create_cb, nullptr);

    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[multi-scene-rec] unloading");
    SlotManager::instance().shutdown();

    // Remove the event callback so it cannot fire into unmapped code after unload.
    // Do NOT touch g_dock here — Qt owns the widget (parented via add_dock_by_id)
    // and may have already destroyed it as a child of the closing main window.
    // The dock stops its own timer in its destructor.
    obs_frontend_remove_event_callback(&dock_create_cb, nullptr);
    g_dock = nullptr;
}
