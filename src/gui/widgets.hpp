// GTK helpers: lambda signal connections, widget builders and small dialogs.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

#include "core/util.hpp"

namespace bg {
class Profile;
class DownloadQueue;
class MonitoringCenter;
}  // namespace bg

namespace bg::gui {

class MainWindow;
class ThumbnailCache;

struct App {
    GtkApplication* gtk = nullptr;
    Profile* profile = nullptr;
    DownloadQueue* queue = nullptr;
    MonitoringCenter* monitoring = nullptr;
    MainWindow* window = nullptr;
    ThumbnailCache* thumbnails = nullptr;
    std::string pendingImport;  // Grabber profile to import on restart
    // Replaces gtk_dialog_run() in runDialog(); set by --screenshot to render dialogs without showing them
    std::function<int(GtkDialog*)> dialogRunner;
};
App& app();
int runDialog(GtkDialog* dialog);

// ---- signals ---------------------------------------------------------------
template <typename R, typename... Args>
struct Thunk {
    static R call(Args... args, gpointer data) { return (*static_cast<std::function<R(Args...)>*>(data))(args...); }
    static void destroy(gpointer data, GClosure*) { delete static_cast<std::function<R(Args...)>*>(data); }
};

template <typename T>
struct NonDeduced {
    using type = T;
};

// on<Return, Args...>(object, "signal", lambda); the instance argument comes first, user data is handled here
template <typename R, typename... Args>
gulong on(gpointer instance, const char* signal, typename NonDeduced<std::function<R(Args...)>>::type f) {
    auto* fn = new std::function<R(Args...)>(std::move(f));
    return g_signal_connect_data(instance, signal, G_CALLBACK((&Thunk<R, Args...>::call)), fn, &Thunk<R, Args...>::destroy, (GConnectFlags)0);
}

gulong onClicked(GtkWidget* w, std::function<void()> f);   // GtkButton "clicked"
gulong onActivate(GtkWidget* w, std::function<void()> f);  // GtkMenuItem / GtkEntry "activate"
gulong onChanged(GtkWidget* w, std::function<void()> f);   // "changed"
gulong onToggled(GtkWidget* w, std::function<void()> f);   // "toggled"
gulong onDestroy(GtkWidget* w, std::function<void()> f);
gulong onButtonPress(GtkWidget* w, std::function<bool(GdkEventButton*)> f);
gulong onKeyPress(GtkWidget* w, std::function<bool(GdkEventKey*)> f);

// ---- builders --------------------------------------------------------------
void addClass(GtkWidget* w, const char* cls);
void removeClass(GtkWidget* w, const char* cls);
GtkWidget* label(const std::string& text, float xalign = 0.0f, bool markup = false);
GtkWidget* button(const std::string& text, const char* icon = nullptr, const std::string& tooltip = "");
GtkWidget* iconButton(const char* icon, const std::string& tooltip);
GtkWidget* hbox(int spacing = 6);
GtkWidget* vbox(int spacing = 6);
GtkWidget* scrolled(GtkWidget* child, bool hscroll = false);
GtkWidget* section(const std::string& title);  // bold label used as a section header
GtkWidget* menuItem(GtkWidget* menu, const std::string& text, std::function<void()> f);
GtkWidget* checkMenuItem(GtkWidget* menu, const std::string& text, bool active, std::function<void(bool)> f);
void menuSeparator(GtkWidget* menu);
void popupMenu(GtkWidget* menu, GdkEvent* event = nullptr);
GtkWidget* spin(double min, double max, double value, double step = 1);
GtkWidget* entry(const std::string& text = "", const std::string& placeholder = "");
std::string entryText(GtkWidget* e);
GtkWidget* combo(const StringList& ids, const StringList& labels, const std::string& active);
std::string comboId(GtkWidget* c);
// Grid helper: label in column 0, widget in column 1
void gridRow(GtkWidget* grid, int row, const std::string& text, GtkWidget* widget, const std::string& tooltip = "");
GtkWidget* grid();

// ---- dialogs ---------------------------------------------------------------
GtkWindow* toplevel(GtkWidget* w);
void message(GtkWindow* parent, const std::string& title, const std::string& text, GtkMessageType type = GTK_MESSAGE_INFO);
bool confirm(GtkWindow* parent, const std::string& title, const std::string& text);
bool askText(GtkWindow* parent, const std::string& title, const std::string& label, std::string& value);
std::string chooseFolder(GtkWindow* parent, const std::string& title, const std::string& initial = "");
std::string chooseFile(GtkWindow* parent, const std::string& title, bool save, const std::string& initial = "", const std::string& filterName = "",
                       const std::string& pattern = "");
GtkWidget* dialog(GtkWindow* parent, const std::string& title, int width = -1, int height = -1);
GtkWidget* dialogContent(GtkWidget* dialog);
void copyToClipboard(const std::string& text);

// ---- images ----------------------------------------------------------------
GdkPixbuf* pixbufFromData(const std::string& data, int maxW = 0, int maxH = 0);
GdkPixbuf* scaleToFit(GdkPixbuf* src, int maxW, int maxH, bool upscale = false);  // new ref
GdkPixbuf* appIcon(int size);

// Colours of tag types and image states from the settings, falling back to theme-friendly defaults
std::string tagColor(const std::string& key);
std::string tagMarkup(const std::string& text, const std::string& type, int count = 0, bool showCount = false);
std::string tagColorKey(const std::string& tag, const std::string& type);

// Pixbuf ownership helper
struct PixbufRef {
    GdkPixbuf* p = nullptr;
    PixbufRef() = default;
    explicit PixbufRef(GdkPixbuf* pb, bool take = true) : p(pb) {
        if(p && !take) g_object_ref(p);
    }
    PixbufRef(const PixbufRef& o) : p(o.p) {
        if(p) g_object_ref(p);
    }
    PixbufRef& operator=(const PixbufRef& o) {
        if(o.p) g_object_ref(o.p);
        if(p) g_object_unref(p);
        p = o.p;
        return *this;
    }
    ~PixbufRef() {
        if(p) g_object_unref(p);
    }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace bg::gui
