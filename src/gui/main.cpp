// booru-grabber: GTK 3 interface.
#include <gtk/gtk.h>
#include <unistd.h>

#include <cstdio>

#include "config.hpp"
#include "core/download.hpp"
#include "core/logger.hpp"
#include "core/monitoring.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"
#include "gui/mainwindow.hpp"
#include "gui/style.hpp"
#include "gui/thumbnails.hpp"
#include "gui/searchtab.hpp"
#include "gui/settingsdialog.hpp"
#include "gui/tray.hpp"
#include "gui/viewer.hpp"

using namespace bg;
using namespace bg::gui;

namespace {

struct GuiOptions {
    gchar* profile = nullptr;
    gchar* importGrabber = nullptr;
    gchar* screenshot = nullptr;
    gchar* screenshotPage = nullptr;
    gint screenshotDelay = 3;
    gboolean portable = FALSE, version = FALSE, minimize = FALSE, debug = FALSE, screenshotViewer = FALSE;
};
GuiOptions g_opts;

std::unique_ptr<Profile> g_profile;
std::unique_ptr<DownloadQueue> g_queue;
std::unique_ptr<MonitoringCenter> g_monitoring;
std::unique_ptr<ThumbnailCache> g_thumbnails;
std::unique_ptr<MainWindow> g_window;

// ---- Screenshots (docs and testing) --------------------------------------------
// The window contents move into offscreen windows, so nothing is shown on screen and the result
// doesn't depend on the compositor mapping the window (it doesn't on hidden workspaces).

GtkWidget* g_offscreen = nullptr;  // what the screenshot captures

GtkWidget* moveOffscreen(GtkWindow* win, int fallbackWidth, int fallbackHeight) {
    int width = 0, height = 0;
    gtk_window_get_default_size(win, &width, &height);
    if(width <= 0) width = fallbackWidth;
    if(height <= 0) height = fallbackHeight;
    GtkWidget* off = gtk_offscreen_window_new();
    GList* classes = gtk_style_context_list_classes(gtk_widget_get_style_context(GTK_WIDGET(win)));
    for(GList* l = classes; l; l = l->next) gtk_style_context_add_class(gtk_widget_get_style_context(off), (const char*)l->data);
    g_list_free(classes);
    gtk_style_context_add_class(gtk_widget_get_style_context(off), "background");
    gtk_widget_set_size_request(off, width, height);
    GtkWidget* child = gtk_bin_get_child(GTK_BIN(win));
    if(child) {
        g_object_ref(child);
        gtk_container_remove(GTK_CONTAINER(win), child);
        gtk_container_add(GTK_CONTAINER(off), child);
        g_object_unref(child);
    }
    gtk_widget_hide(GTK_WIDGET(win));
    gtk_widget_show(off);
    return off;
}

// Lets the offscreen window lay out and draw
void settle(int ms) {
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    while(g_get_monotonic_time() < end) {
        while(gtk_events_pending()) gtk_main_iteration_do(FALSE);
        g_usleep(10000);
    }
}

void writeScreenshot(GtkWidget* off, const std::string& file) {
    GdkPixbuf* pb = off ? gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(off)) : nullptr;
    if(!pb) {
        log("Screenshot failed: nothing to capture", LogLevel::Error);
        return;
    }
    GError* err = nullptr;
    if(gdk_pixbuf_save(pb, file.c_str(), "png", &err, nullptr))
        log("Screenshot written to " + file, LogLevel::Info);
    else {
        log("Screenshot failed: " + std::string(err->message), LogLevel::Error);
        g_error_free(err);
    }
    g_object_unref(pb);
}

void onStartup(GtkApplication* gtkApp) {
    installStyle();
    gtk_window_set_default_icon_name("booru-grabber");
    g_profile = std::make_unique<Profile>();
    importLegacyCookies();
    g_queue = std::make_unique<DownloadQueue>(g_profile.get());
    g_monitoring = std::make_unique<MonitoringCenter>(g_profile.get(), g_queue.get());
    g_thumbnails = std::make_unique<ThumbnailCache>();
    App& a = app();
    a.gtk = gtkApp;
    a.profile = g_profile.get();
    a.queue = g_queue.get();
    a.monitoring = g_monitoring.get();
    a.thumbnails = g_thumbnails.get();
    updateDynamicStyle();
    g_profile->purgeTemp(24 * 3600);
    g_queue->load();
    g_window = std::make_unique<MainWindow>(gtkApp);
    a.window = g_window.get();
    setupTray();
    g_monitoring->start();
    if(g_opts.minimize) gtk_window_iconify(g_window->window());
    if(g_opts.screenshot) {
        g_offscreen = moveOffscreen(g_window->window(), 1280, 820);
        // Dialogs are rendered offscreen and captured instead of being run
        a.dialogRunner = [](GtkDialog* d) -> int {
            g_offscreen = moveOffscreen(GTK_WINDOW(d), 800, 600);
            settle(1500);
            writeScreenshot(g_offscreen, g_opts.screenshot);
            g_offscreen = nullptr;
            g_window->requestQuit();
            return GTK_RESPONSE_CANCEL;
        };
    }
    if(g_opts.screenshot && g_opts.screenshotPage) {
        g_timeout_add_seconds((guint)std::max(1, g_opts.screenshotDelay / 2), [](gpointer) -> gboolean {
            std::string page = g_opts.screenshotPage;
            if(page == "settings")
                settingsDialog(g_window->window());  // runs its own loop until the screenshot closes it
            else if(!g_window->showPage(page))
                log("Unknown page for --screenshot-page: " + page, LogLevel::Warning);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
    if(g_opts.screenshot && g_opts.screenshotViewer) {
        g_timeout_add_seconds((guint)std::max(1, g_opts.screenshotDelay / 2), [](gpointer) -> gboolean {
            SearchTab* tab = g_window->currentSearchTab();
            if(tab && !tab->images().empty()) g_window->openViewer(tab->images(), 0, tab);
            if(ViewerWindow::last()) g_offscreen = moveOffscreen(ViewerWindow::last()->window(), 1100, 700);
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
    if(g_opts.screenshot) {
        std::string file = g_opts.screenshot;
        g_timeout_add_seconds((guint)std::max(1, g_opts.screenshotDelay), [](gpointer data) -> gboolean {
            auto* f = static_cast<std::string*>(data);
            if(g_offscreen) {  // still null after a dialog took the screenshot
                writeScreenshot(g_offscreen, *f);
                g_window->requestQuit();
            }
            delete f;
            return G_SOURCE_REMOVE;
        }, new std::string(file));
    }
}

int onCommandLine(GApplication*, GApplicationCommandLine* cmd) {
    int argc = 0;
    gchar** argv = g_application_command_line_get_arguments(cmd, &argc);
    for(int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if(contains(arg, "://") && !startsWith(arg, "http")) {
            if(!handleOAuthCallback(arg)) log("Unhandled link: " + arg, LogLevel::Warning);
        } else if(startsWith(arg, "http")) {
            // A link to a page: search it in a new tab (sources parse direct URLs)
            if(g_window) g_window->addSearchTab({arg}, {}, true);
        }
    }
    g_strfreev(argv);
    if(g_window && !g_opts.screenshot) g_window->present();
    return 0;
}

void shutdownAll() {
    if(g_monitoring) g_monitoring->stop();
    removeTray();
    g_window.reset();
    app().window = nullptr;
    if(g_queue) g_queue->save();
    g_monitoring.reset();
    g_queue.reset();
    g_thumbnails.reset();
    g_profile.reset();
}

}  // namespace

int main(int argc, char** argv) {
    g_set_prgname("booru-grabber");
    g_set_application_name(BG_APP_NAME);

    GOptionEntry entries[] = {
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &g_opts.profile, "Keep all settings and data in this directory", "DIR"},
        {"portable", 0, 0, G_OPTION_ARG_NONE, &g_opts.portable, "Keep settings and data next to the program", nullptr},
        {"import-grabber", 0, 0, G_OPTION_ARG_FILENAME, &g_opts.importGrabber, "Import a profile of the original Grabber first", "DIR"},
        {"minimize", 0, 0, G_OPTION_ARG_NONE, &g_opts.minimize, "Start minimized", nullptr},
        {"debug", 'd', 0, G_OPTION_ARG_NONE, &g_opts.debug, "Print debug messages", nullptr},
        {"screenshot", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_FILENAME, &g_opts.screenshot, "Save a screenshot of the window and quit", "FILE"},
        {"screenshot-viewer", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &g_opts.screenshotViewer, "Open the first result in the viewer for the screenshot", nullptr},
        {"screenshot-page", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &g_opts.screenshotPage, "Show this tab (favorites, downloads, monitors, log) or the settings for the screenshot", "PAGE"},
        {"screenshot-delay", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_INT, &g_opts.screenshotDelay, "Seconds before the screenshot", "N"},
        {"version", 'v', 0, G_OPTION_ARG_NONE, &g_opts.version, "Show the version", nullptr},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr}};
    GOptionContext* ctx = g_option_context_new("[LINK…]");
    g_option_context_add_main_entries(ctx, entries, nullptr);
    g_option_context_add_group(ctx, gtk_get_option_group(FALSE));
    g_option_context_set_summary(ctx, "Search and download images from boorus and imageboards.");
    GError* err = nullptr;
    if(!g_option_context_parse(ctx, &argc, &argv, &err)) {
        fprintf(stderr, "booru-grabber: %s\n", err->message);
        g_error_free(err);
        return 2;
    }
    g_option_context_free(ctx);
    if(g_opts.version) {
        printf("booru-grabber %s\n", BG_VERSION);
        return 0;
    }

    paths::init(g_opts.profile ? g_opts.profile : "", g_opts.portable);
    Logger& logger = Logger::instance();
    logger.setConsoleLevel(g_opts.debug ? LogLevel::Debug : LogLevel::Warning);
    logger.setLogFile(buildPath(paths::stateDir(), "logs/main.log"));
    log("Booru Grabber " BG_VERSION " starting; config in " + paths::configDir() + ", data in " + paths::dataDir(), LogLevel::Info);

    if(g_opts.importGrabber) {
        int n = importGrabberProfile(g_opts.importGrabber);
        if(n < 0) fprintf(stderr, "booru-grabber: no Grabber profile in %s\n", g_opts.importGrabber);
    }

    // Separate profiles (and portable copies) run as separate instances
    GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
    if(g_opts.profile || paths::portable() || g_opts.screenshot) flags = (GApplicationFlags)(flags | G_APPLICATION_NON_UNIQUE);
    GtkApplication* gtkApp = gtk_application_new(BG_APP_ID, flags);
    g_signal_connect(gtkApp, "startup", G_CALLBACK(+[](GtkApplication* a, gpointer) { onStartup(a); }), nullptr);
    g_signal_connect(gtkApp, "command-line", G_CALLBACK(onCommandLine), nullptr);
    int status = g_application_run(G_APPLICATION(gtkApp), argc, argv);
    std::string pendingImport = app().pendingImport;
    shutdownAll();
    g_object_unref(gtkApp);

    if(!pendingImport.empty()) {
        int n = importGrabberProfile(pendingImport);
        log("Imported " + std::to_string(n) + " files, restarting", LogLevel::Info);
        std::string exe = paths::executable();
        std::vector<char*> args{exe.data()};
        std::string profileArg, portableArg = "--portable";
        if(g_opts.profile) {
            profileArg = std::string("--profile=") + g_opts.profile;
            args.push_back(profileArg.data());
        }
        if(g_opts.portable) args.push_back(portableArg.data());
        args.push_back(nullptr);
        execv(exe.c_str(), args.data());
    }
    return status;
}
