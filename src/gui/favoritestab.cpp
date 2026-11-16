#include "gui/favoritestab.hpp"

#include <algorithm>

#include "core/monitoring.hpp"
#include "core/profile.hpp"
#include "gui/dialogs.hpp"
#include "gui/mainwindow.hpp"

namespace bg::gui {

FavoritesTab::FavoritesTab(MainWindow* window) : m_window(window) {
    Settings& s = app().profile->settings();
    m_root = vbox(0);
    addClass(m_root, "bg");
    GtkWidget* bar = hbox(6);
    addClass(bar, "bg-toolbar");
    m_filter = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(m_filter), "Filter favorites");
    onChanged(m_filter, [this] { refresh(); });
    gtk_box_pack_start(GTK_BOX(bar), m_filter, TRUE, TRUE, 0);
    m_sort = combo({"name", "note", "lastviewed"}, {"Name", "Note", "Last viewed"}, s.get("Favorites/order", "name"));
    gtk_widget_set_tooltip_text(m_sort, "Sort by");
    onChanged(m_sort, [this] {
        app().profile->settings().set("Favorites/order", comboId(m_sort));
        refresh();
    });
    gtk_box_pack_start(GTK_BOX(bar), m_sort, FALSE, FALSE, 0);
    m_reverse = gtk_check_button_new_with_label("Reverse");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_reverse), s.getBool("Favorites/reverse", false));
    onToggled(m_reverse, [this] {
        app().profile->settings().setBool("Favorites/reverse", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_reverse)));
        refresh();
    });
    gtk_box_pack_start(GTK_BOX(bar), m_reverse, FALSE, FALSE, 0);
    GtkWidget* add = button("_Add…", "list-add-symbolic", "Add a favorite search");
    onClicked(add, [this] {
        std::string tag;
        if(askText(toplevel(m_root), "Add favorite", "Search to add to the favorites:", tag) && !trim(tag).empty()) addFavorite(toplevel(m_root), trim(tag));
    });
    GtkWidget* checkAll = button("Check monitors", "view-refresh-symbolic", "Check every monitor now");
    onClicked(checkAll, [] { app().monitoring->checkAll(); });
    GtkWidget* getAll = button("Download all", "document-save-symbolic", "Add every favorite search to the downloads");
    onClicked(getAll, [] {
        Profile* p = app().profile;
        for(auto& fav : p->favorites()) {
            StringList sites = fav.sites.empty() ? p->settings().getList("sites") : fav.sites;
            for(auto& url : sites) {
                DownloadQueryGroup g;
                g.query.tags = split(fav.name, ' ', true);
                g.site = p->site(url);
                if(!g.site) continue;
                g.perpage = (int)p->settings().getInt("limit", 20);
                g.total = -1;
                g.postFiltering = fav.postFiltering;
                g.filename = p->settings().get("Save/filename_favorites", p->settings().get("Save/filename"));
                g.path = expandHome(p->settings().get("Save/path_favorites", p->savePath()));
                if(g.path.empty() || g.path == "/") g.path = p->savePath();
                app().queue->addGroup(g);
            }
        }
        app().window->showDownloads();
    });
    for(GtkWidget* w : {add, checkAll, getAll}) gtk_box_pack_end(GTK_BOX(bar), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), bar, FALSE, FALSE, 0);

    m_flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(m_flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(m_flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(m_flow), 64);
    gtk_widget_set_valign(m_flow, GTK_ALIGN_START);
    GtkWidget* sw = scrolled(m_flow);
    addClass(sw, "bg-results");
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_pack_start(GTK_BOX(m_root), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(m_root);
    m_connection = app().profile->favoritesChanged.connect([this] { refresh(); });
    refresh();
}

FavoritesTab::~FavoritesTab() { app().profile->favoritesChanged.disconnect(m_connection); }

void FavoritesTab::refresh() {
    GList* children = gtk_container_get_children(GTK_CONTAINER(m_flow));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    std::vector<Favorite> favs = app().profile->favorites();
    std::string order = comboId(m_sort);
    std::stable_sort(favs.begin(), favs.end(), [&](const Favorite& a, const Favorite& b) {
        if(order == "note") return a.note < b.note;
        if(order == "lastviewed") return a.lastViewed < b.lastViewed;
        return naturalCompare(toLower(a.name), toLower(b.name)) < 0;
    });
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_reverse))) std::reverse(favs.begin(), favs.end());
    std::string filter = toLower(entryText(m_filter));
    for(auto& f : favs) {
        if(!filter.empty() && !contains(toLower(f.name), filter)) continue;
        GtkWidget* ev = gtk_event_box_new();
        GtkWidget* card = vbox(4);
        addClass(card, "bg-favorite");
        int cumulated = 0;
        for(auto& m : f.monitors) cumulated += m.cumulated;
        if(cumulated > 0) addClass(card, "new-images");
        GtkWidget* img;
        if(!f.imagePath.empty() && fileExists(f.imagePath)) {
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(f.imagePath.c_str(), 150, 150, TRUE, nullptr);
            img = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new_from_icon_name("starred-symbolic", GTK_ICON_SIZE_DIALOG);
            if(pb) g_object_unref(pb);
        } else {
            img = gtk_image_new_from_icon_name("starred-symbolic", GTK_ICON_SIZE_DIALOG);
        }
        gtk_widget_set_size_request(img, 150, 150);
        gtk_box_pack_start(GTK_BOX(card), img, FALSE, FALSE, 0);
        GtkWidget* name = label("<b>" + htmlEscape(replaceAll(f.name, "_", " ")) + "</b>", 0.5f, true);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 20);
        gtk_box_pack_start(GTK_BOX(card), name, FALSE, FALSE, 0);
        std::string info = std::to_string(f.note) + "% · " + (f.lastViewed.valid() ? f.lastViewed.toLocal().format("yyyy-MM-dd") : "never");
        if(cumulated > 0) info += " · " + std::to_string(cumulated) + " new";
        GtkWidget* sub = label(info, 0.5f);
        addClass(sub, "dim-label");
        gtk_box_pack_start(GTK_BOX(card), sub, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(ev), card);
        gtk_widget_set_tooltip_text(ev, f.name.c_str());
        std::string favName = f.name;
        onButtonPress(ev, [this, favName](GdkEventButton* e) {
            if(e->type != GDK_BUTTON_PRESS) return false;
            if(e->button == 1 || e->button == 2) {
                Favorite* fav = app().profile->favorite(favName);
                if(!fav) return true;
                fav->lastViewed = DateTime::nowUtc();
                for(auto& m : fav->monitors) m.cumulated = 0;
                StringList sites = fav->sites;
                std::string name = fav->name;
                app().profile->syncFavorites();
                SearchTab* tab = m_window->addSearchTab(split(name, ' ', true), sites, e->button == 1);
                (void)tab;
                defer([] { app().profile->favoritesChanged.emit(); });
                return true;
            }
            if(e->button == 3) {
                favoriteMenu(favName, (GdkEvent*)e);
                return true;
            }
            return false;
        });
        gtk_container_add(GTK_CONTAINER(m_flow), ev);
    }
    if(favs.empty()) {
        GtkWidget* l = label("No favorites yet. Right-click a tag and choose \"Add to favorites\".", 0.5f);
        addClass(l, "bg-placeholder");
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
        gtk_container_add(GTK_CONTAINER(m_flow), l);
    }
    gtk_widget_show_all(m_flow);
}

void FavoritesTab::favoriteMenu(const std::string& name, GdkEvent* e) {
    GtkWidget* menu = gtk_menu_new();
    menuItem(menu, "_Open", [this, name] { m_window->openSearch(name, true); });
    menuItem(menu, "_Edit…", [this, name] {
        Favorite* f = app().profile->favorite(name);
        if(!f) return;
        Favorite copy = *f;
        if(editFavoriteDialog(toplevel(m_root), copy, false)) {
            if(copy.name != name) app().profile->removeFavorite(name);
            app().profile->addFavorite(copy);
        }
    });
    menuItem(menu, "Mark as _viewed", [name] {
        Favorite* f = app().profile->favorite(name);
        if(!f) return;
        f->lastViewed = DateTime::nowUtc();
        for(auto& m : f->monitors) m.cumulated = 0;
        app().profile->syncFavorites();
        app().profile->favoritesChanged.emit();
    });
    menuItem(menu, "Check _monitors now", [name] {
        Favorite* f = app().profile->favorite(name);
        if(!f) return;
        for(size_t i = 0; i < f->monitors.size(); i++) app().monitoring->checkFavorite(name, i);
    });
    menuItem(menu, "Copy", [name] { copyToClipboard(name); });
    menuSeparator(menu);
    menuItem(menu, "_Remove", [this, name] {
        if(confirm(toplevel(m_root), "Remove favorite", "Remove \"" + name + "\" from the favorites?")) app().profile->removeFavorite(name);
    });
    popupMenu(menu, e);
}

}  // namespace bg::gui
