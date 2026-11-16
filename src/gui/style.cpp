#include "gui/style.hpp"

#include "core/profile.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

// Square corners, opaque surfaces, theme colours only (works with pywal generated GTK themes).
static const char* kCss = R"CSS(
.bg, .bg * { border-radius: 0; }
.bg { background-color: @theme_bg_color; }
.bg menu, .bg-menu, .bg-menu menuitem { border-radius: 0; }
.bg-menu { background-color: @theme_bg_color; }
.bg notebook > header { background-color: @theme_bg_color; }
.bg notebook > header tab { padding: 4px 10px; min-height: 0; }
.bg notebook > header tab button { padding: 0; min-height: 16px; min-width: 16px; }
.bg-section { margin-top: 10px; }

/* Toolbars and status bar */
.bg-toolbar { padding: 4px 6px; background-color: @theme_bg_color; border-bottom: 1px solid @borders; }
.bg-bottombar { padding: 4px 6px; background-color: @theme_bg_color; border-top: 1px solid @borders; }
.bg-statusbar { padding: 2px 8px; background-color: @theme_bg_color; border-top: 1px solid @borders; }
.bg-statusbar label { font-size: 0.92em; }

/* Sidebar */
.bg-sidebar { background-color: @theme_bg_color; border-right: 1px solid @borders; }
.bg-sidebar list, .bg-sidebar row { background-color: @theme_bg_color; }
.bg-sidebar row { padding: 1px 8px; }
.bg-sidebar row:hover { background-color: mix(@theme_bg_color, @theme_fg_color, 0.08); }
.bg-sidebar stackswitcher button { padding: 3px 4px; min-width: 0; }

/* Results */
.bg-results { background-color: @theme_base_color; }
.bg-results flowbox, .bg-results flowboxchild { background-color: @theme_base_color; padding: 0; }
.bg-site-header { padding: 6px 8px 2px 8px; font-weight: bold; }
.bg-site-header.error { color: @error_color; }
.bg-thumb { padding: 3px; border: 3px solid transparent; background-color: @theme_base_color; }
.bg-thumb:hover { border-color: mix(@theme_base_color, @theme_fg_color, 0.25); }
.bg-thumb.selected { border-color: @theme_selected_bg_color; background-color: mix(@theme_base_color, @theme_selected_bg_color, 0.25); }
.bg-badge { background-color: @theme_bg_color; color: @theme_fg_color; padding: 0 4px; font-size: 0.85em; border: 1px solid @borders; }
.bg-placeholder { color: mix(@theme_fg_color, @theme_base_color, 0.5); }

/* Viewer */
.bg-viewer-area { background-color: @theme_base_color; }
.bg-viewer-details { background-color: @theme_bg_color; border-left: 1px solid @borders; padding: 8px; }
.bg-overlay { background-color: @theme_bg_color; color: @theme_fg_color; border: 1px solid @borders; padding: 4px 10px; }
.bg-overlay.error { color: @error_color; }

/* Favorites */
.bg-favorite { padding: 6px; border: 2px solid transparent; background-color: @theme_base_color; }
.bg-favorite:hover { border-color: mix(@theme_base_color, @theme_fg_color, 0.25); }
.bg-favorite.new-images { border-color: @theme_selected_bg_color; }

/* Settings */
.bg-settings-sidebar { background-color: @theme_bg_color; border-right: 1px solid @borders; }
.bg-settings-sidebar row { padding: 7px 12px; }
.bg-settings-page { padding: 16px; }
.bg-valid { color: @success_color; }
.bg-warning { color: @warning_color; }
.bg-invalid { color: @error_color; }
textview.bg-mono, .bg-mono text { font-family: monospace; }
)CSS";

static GtkCssProvider* g_dynamic = nullptr;

void installStyle() {
    GtkCssProvider* p = gtk_css_provider_new();
    GError* e = nullptr;
    if(!gtk_css_provider_load_from_data(p, kCss, -1, &e)) {
        g_printerr("booru-grabber: CSS error: %s\n", e->message);
        g_error_free(e);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
    updateDynamicStyle();
}

// Thumbnail border colours come from the settings (Coloring/Borders/*)
void updateDynamicStyle() {
    Profile* profile = app().profile;
    if(!profile) return;
    Settings& s = profile->settings();
    std::string css;
    struct {
        const char* cls;
        const char* key;
        const char* def;
    } borders[] = {{"blacklisted", "Coloring/Borders/blacklisteds", "#444444"}, {"favorited", "Coloring/Borders/favorites", "#ffaaff"},
                   {"parent", "Coloring/Borders/parent", "#cccc00"},           {"children", "Coloring/Borders/children", "#00ff00"},
                   {"pending", "Coloring/Borders/pending", "#0000ff"}};
    for(auto& b : borders) {
        std::string color = s.get(b.key, b.def);
        GdkRGBA rgba;
        if(!gdk_rgba_parse(&rgba, color.c_str())) continue;
        css += std::string(".bg-thumb.") + b.cls + " { border-color: " + color + "; }\n";
    }
    if(!g_dynamic) {
        g_dynamic = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(g_dynamic), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_data(g_dynamic, css.c_str(), -1, nullptr);
}

GdkRGBA themeColor(GtkWidget* w, const char* name, const char* fallback) {
    GdkRGBA c;
    GtkStyleContext* ctx = gtk_widget_get_style_context(w);
    if(!gtk_style_context_lookup_color(ctx, name, &c)) gdk_rgba_parse(&c, fallback);
    return c;
}

}  // namespace bg::gui
