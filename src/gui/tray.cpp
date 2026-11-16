#include "gui/tray.hpp"

#include "core/profile.hpp"
#include "gui/mainwindow.hpp"
#include "gui/widgets.hpp"

#ifdef BG_HAVE_TRAY
#include <libayatana-appindicator/app-indicator.h>
#endif

namespace bg::gui {

#ifdef BG_HAVE_TRAY
static AppIndicator* g_indicator = nullptr;

bool trayAvailable() { return g_indicator != nullptr; }

void setupTray() {
    if(g_indicator || !app().profile->settings().getBool("Monitoring/enableTray", false)) return;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    g_indicator = app_indicator_new("booru-grabber", "booru-grabber", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    G_GNUC_END_IGNORE_DEPRECATIONS
    app_indicator_set_title(g_indicator, "Booru Grabber");
    GtkWidget* menu = gtk_menu_new();
    menuItem(menu, "_Show", [] {
        if(app().window) app().window->present();
    });
    menuItem(menu, "_Hide", [] {
        if(app().window) gtk_widget_hide(GTK_WIDGET(app().window->window()));
    });
    menuSeparator(menu);
    menuItem(menu, "_Quit", [] {
        if(app().window) app().window->requestQuit();
    });
    gtk_widget_show_all(menu);
    app_indicator_set_menu(g_indicator, GTK_MENU(menu));
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);
}

void removeTray() {
    if(!g_indicator) return;
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_PASSIVE);
    g_object_unref(g_indicator);
    g_indicator = nullptr;
}
#else
bool trayAvailable() { return false; }
void setupTray() {}
void removeTray() {}
#endif

}  // namespace bg::gui
