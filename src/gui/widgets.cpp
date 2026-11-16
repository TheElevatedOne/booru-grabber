#include "gui/widgets.hpp"

#include "core/profile.hpp"
#include "core/paths.hpp"

namespace bg::gui {

int runDialog(GtkDialog* dialog) { return app().dialogRunner ? app().dialogRunner(dialog) : gtk_dialog_run(dialog); }

App& app() {
    static App a;
    return a;
}

// ---- signals ---------------------------------------------------------------

gulong onClicked(GtkWidget* w, std::function<void()> f) {
    return on<void, GtkWidget*>(w, "clicked", [f](GtkWidget*) { f(); });
}
gulong onActivate(GtkWidget* w, std::function<void()> f) {
    return on<void, GtkWidget*>(w, "activate", [f](GtkWidget*) { f(); });
}
gulong onChanged(GtkWidget* w, std::function<void()> f) {
    return on<void, GtkWidget*>(w, "changed", [f](GtkWidget*) { f(); });
}
gulong onToggled(GtkWidget* w, std::function<void()> f) {
    return on<void, GtkWidget*>(w, "toggled", [f](GtkWidget*) { f(); });
}
gulong onDestroy(GtkWidget* w, std::function<void()> f) {
    return on<void, GtkWidget*>(w, "destroy", [f](GtkWidget*) { f(); });
}
gulong onButtonPress(GtkWidget* w, std::function<bool(GdkEventButton*)> f) {
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK);
    return on<gboolean, GtkWidget*, GdkEventButton*>(w, "button-press-event", [f](GtkWidget*, GdkEventButton* e) -> gboolean { return f(e); });
}
gulong onKeyPress(GtkWidget* w, std::function<bool(GdkEventKey*)> f) {
    return on<gboolean, GtkWidget*, GdkEventKey*>(w, "key-press-event", [f](GtkWidget*, GdkEventKey* e) -> gboolean { return f(e); });
}

// ---- builders --------------------------------------------------------------

void addClass(GtkWidget* w, const char* cls) { gtk_style_context_add_class(gtk_widget_get_style_context(w), cls); }
void removeClass(GtkWidget* w, const char* cls) { gtk_style_context_remove_class(gtk_widget_get_style_context(w), cls); }

GtkWidget* label(const std::string& text, float xalign, bool markup) {
    GtkWidget* l = gtk_label_new(nullptr);
    if(markup)
        gtk_label_set_markup(GTK_LABEL(l), text.c_str());
    else
        gtk_label_set_text(GTK_LABEL(l), text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

GtkWidget* button(const std::string& text, const char* icon, const std::string& tooltip) {
    GtkWidget* b = gtk_button_new_with_mnemonic(text.c_str());
    if(icon) {
        gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
        gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    }
    if(!tooltip.empty()) gtk_widget_set_tooltip_text(b, tooltip.c_str());
    return b;
}

GtkWidget* iconButton(const char* icon, const std::string& tooltip) {
    GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tooltip.c_str());
    return b;
}

GtkWidget* hbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }
GtkWidget* vbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }

GtkWidget* scrolled(GtkWidget* child, bool hscroll) {
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), hscroll ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    if(child) gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

GtkWidget* section(const std::string& title) {
    GtkWidget* l = label("<b>" + htmlEscape(title) + "</b>", 0, true);
    addClass(l, "bg-section");
    return l;
}

GtkWidget* menuItem(GtkWidget* menu, const std::string& text, std::function<void()> f) {
    GtkWidget* item = gtk_menu_item_new_with_mnemonic(text.c_str());
    onActivate(item, std::move(f));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show(item);
    return item;
}

GtkWidget* checkMenuItem(GtkWidget* menu, const std::string& text, bool active, std::function<void(bool)> f) {
    GtkWidget* item = gtk_check_menu_item_new_with_mnemonic(text.c_str());
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
    on<void, GtkCheckMenuItem*>(item, "toggled", [f](GtkCheckMenuItem* i) { f(gtk_check_menu_item_get_active(i)); });
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    gtk_widget_show(item);
    return item;
}

void menuSeparator(GtkWidget* menu) {
    GtkWidget* s = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), s);
    gtk_widget_show(s);
}

void popupMenu(GtkWidget* menu, GdkEvent* event) {
    gtk_widget_show_all(menu);
    addClass(menu, "bg-menu");
    // The menu destroys itself once hidden
    g_object_ref_sink(menu);
    on<void, GtkWidget*>(menu, "deactivate", [menu](GtkWidget*) {
        g_idle_add([](gpointer m) -> gboolean {
            gtk_widget_destroy(GTK_WIDGET(m));
            g_object_unref(m);
            return G_SOURCE_REMOVE;
        }, menu);
    });
    gtk_menu_popup_at_pointer(GTK_MENU(menu), event);
}

GtkWidget* spin(double min, double max, double value, double step) {
    GtkWidget* s = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    return s;
}

GtkWidget* entry(const std::string& text, const std::string& placeholder) {
    GtkWidget* e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text.c_str());
    if(!placeholder.empty()) gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder.c_str());
    return e;
}

std::string entryText(GtkWidget* e) { return gtk_entry_get_text(GTK_ENTRY(e)); }

GtkWidget* combo(const StringList& ids, const StringList& labels, const std::string& active) {
    GtkWidget* c = gtk_combo_box_text_new();
    for(size_t i = 0; i < ids.size(); i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), ids[i].c_str(), (i < labels.size() ? labels[i] : ids[i]).c_str());
    if(!gtk_combo_box_set_active_id(GTK_COMBO_BOX(c), active.c_str()) && !ids.empty()) gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    return c;
}

std::string comboId(GtkWidget* c) {
    const char* id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(c));
    return id ? id : "";
}

GtkWidget* grid() {
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 12);
    return g;
}

void gridRow(GtkWidget* g, int row, const std::string& text, GtkWidget* widget, const std::string& tooltip) {
    GtkWidget* l = label(text);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(g), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(g), widget, 1, row, 1, 1);
    if(!tooltip.empty()) {
        gtk_widget_set_tooltip_text(l, tooltip.c_str());
        gtk_widget_set_tooltip_text(widget, tooltip.c_str());
    }
}

// ---- dialogs ---------------------------------------------------------------

GtkWindow* toplevel(GtkWidget* w) {
    if(!w) return nullptr;
    GtkWidget* t = gtk_widget_get_toplevel(w);
    return GTK_IS_WINDOW(t) ? GTK_WINDOW(t) : nullptr;
}

void message(GtkWindow* parent, const std::string& title, const std::string& text, GtkMessageType type) {
    GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", title.c_str());
    if(!text.empty()) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text.c_str());
    addClass(d, "bg");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

bool confirm(GtkWindow* parent, const std::string& title, const std::string& text) {
    GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", title.c_str());
    if(!text.empty()) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text.c_str());
    addClass(d, "bg");
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return ok;
}

GtkWidget* dialog(GtkWindow* parent, const std::string& title, int width, int height) {
    GtkWidget* d = gtk_dialog_new_with_buttons(title.c_str(), parent, (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                               "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    if(width > 0 || height > 0) gtk_window_set_default_size(GTK_WINDOW(d), width, height);
    addClass(d, "bg");
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);
    return d;
}

GtkWidget* dialogContent(GtkWidget* d) { return gtk_dialog_get_content_area(GTK_DIALOG(d)); }

bool askText(GtkWindow* parent, const std::string& title, const std::string& text, std::string& value) {
    GtkWidget* d = dialog(parent, title, 420);
    GtkWidget* e = entry(value);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), label(text), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), e, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
    if(ok) value = entryText(e);
    gtk_widget_destroy(d);
    return ok;
}

std::string chooseFolder(GtkWindow* parent, const std::string& title, const std::string& initial) {
    GtkFileChooserNative* n = gtk_file_chooser_native_new(title.c_str(), parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
    std::string init = expandHome(initial);
    if(!init.empty() && isDir(init)) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(n), init.c_str());
    std::string result;
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(n)) == GTK_RESPONSE_ACCEPT) {
        gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(n));
        if(f) result = f;
        g_free(f);
    }
    g_object_unref(n);
    return result;
}

std::string chooseFile(GtkWindow* parent, const std::string& title, bool save, const std::string& initial, const std::string& filterName,
                       const std::string& pattern) {
    GtkFileChooserNative* n = gtk_file_chooser_native_new(title.c_str(), parent, save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
                                                          save ? "_Save" : "_Open", "_Cancel");
    if(save) gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(n), TRUE);
    std::string init = expandHome(initial);
    if(!init.empty()) {
        if(isDir(init))
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(n), init.c_str());
        else {
            std::string dir = dirName(init);
            if(isDir(dir)) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(n), dir.c_str());
            if(save) gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(n), baseName(init).c_str());
        }
    }
    if(!pattern.empty()) {
        GtkFileFilter* f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, filterName.c_str());
        for(auto& p : split(pattern, ';', true)) gtk_file_filter_add_pattern(f, p.c_str());
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(n), f);
    }
    std::string result;
    if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(n)) == GTK_RESPONSE_ACCEPT) {
        gchar* f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(n));
        if(f) result = f;
        g_free(f);
    }
    g_object_unref(n);
    return result;
}

void copyToClipboard(const std::string& text) {
    GtkClipboard* c = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(c, text.c_str(), (gint)text.size());
}

// ---- images ----------------------------------------------------------------

GdkPixbuf* scaleToFit(GdkPixbuf* src, int maxW, int maxH, bool upscale) {
    if(!src) return nullptr;
    int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
    if(maxW <= 0 || maxH <= 0 || (!upscale && w <= maxW && h <= maxH)) return GDK_PIXBUF(g_object_ref(src));
    double scale = std::min((double)maxW / w, (double)maxH / h);
    int nw = std::max(1, (int)(w * scale)), nh = std::max(1, (int)(h * scale));
    return gdk_pixbuf_scale_simple(src, nw, nh, GDK_INTERP_BILINEAR);
}

GdkPixbuf* pixbufFromData(const std::string& data, int maxW, int maxH) {
    if(data.empty()) return nullptr;
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    GError* err = nullptr;
    gdk_pixbuf_loader_write(loader, (const guchar*)data.data(), data.size(), &err);
    if(err) g_clear_error(&err);
    gdk_pixbuf_loader_close(loader, &err);
    if(err) g_clear_error(&err);
    GdkPixbuf* pb = gdk_pixbuf_loader_get_pixbuf(loader);
    GdkPixbuf* out = nullptr;
    if(pb) {
        GdkPixbuf* rotated = gdk_pixbuf_apply_embedded_orientation(pb);
        out = scaleToFit(rotated ? rotated : pb, maxW, maxH);
        if(rotated) g_object_unref(rotated);
    }
    g_object_unref(loader);
    return out;
}

GdkPixbuf* appIcon(int size) {
    GtkIconTheme* theme = gtk_icon_theme_get_default();
    GdkPixbuf* pb = gtk_icon_theme_load_icon(theme, "booru-grabber", size, (GtkIconLookupFlags)0, nullptr);
    if(pb) return pb;
    std::string path = paths::findData("booru-grabber.png");
    if(path.empty()) return nullptr;
    return gdk_pixbuf_new_from_file_at_size(path.c_str(), size, size, nullptr);
}

// ---- tag colours -------------------------------------------------------------

std::string tagColor(const std::string& key) {
    static const std::map<std::string, std::string> defaults{
        {"artists", "#aa0000"}, {"circles", "#55bbff"},   {"copyrights", "#aa00aa"},   {"characters", "#00aa00"},
        {"species", "#ee6600"}, {"metas", "#ee6600"},     {"models", "#00aaff"},       {"generals", ""},
        {"favorites", "#ffaaff"}, {"keptForLater", "#aaaa00"}, {"blacklisteds", "#444444"}, {"ignoreds", "#777777"}};
    auto it = defaults.find(key);
    std::string def = it == defaults.end() ? "" : it->second;
    Profile* p = app().profile;
    std::string v = p ? p->settings().get("Coloring/Colors/" + key, def) : def;
    // General tags follow the theme's text colour unless the user picked one explicitly
    if(key == "generals" && (v == "#000000" || v == "#ffffff") && !(p && p->settings().containsOwn("Coloring/Colors/generals"))) return "";
    return v;
}

std::string tagColorKey(const std::string& tag, const std::string& type) {
    Profile* p = app().profile;
    if(p) {
        for(auto& f : p->favorites())
            if(f.name == tag) return "favorites";
        if(contains(p->keptForLater(), tag)) return "keptForLater";
        if(containsCi(p->ignored(), tag)) return "ignoreds";
        if(p->blacklist().contains(tag)) return "blacklisteds";
    }
    std::string plural = type + (endsWith(type, "s") ? "" : "s");
    static const StringList known{"artists", "circles", "copyrights", "characters", "species", "metas", "models", "generals"};
    return contains(known, plural) ? plural : "generals";
}

std::string tagMarkup(const std::string& text, const std::string& type, int count, bool showCount) {
    std::string color = tagColor(tagColorKey(text, type));
    std::string display = htmlEscape(replaceAll(text, "_", " "));
    std::string out = color.empty() ? display : "<span foreground=\"" + color + "\">" + display + "</span>";
    if(showCount && count > 0) out += " <small><span alpha=\"60%\">" + std::to_string(count) + "</span></small>";
    return out;
}

}  // namespace bg::gui
