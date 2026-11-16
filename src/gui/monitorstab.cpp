#include "gui/monitorstab.hpp"

#include "core/monitoring.hpp"
#include "core/profile.hpp"
#include "gui/dialogs.hpp"

namespace bg::gui {

enum { M_SEARCH, M_SITES, M_INTERVAL, M_LAST, M_NEXT, M_NEW, M_DOWNLOAD, M_STATE, M_COLS };

static std::string formatInterval(int secs) {
    if(secs % 86400 == 0) return std::to_string(secs / 86400) + " d";
    if(secs % 3600 == 0) return std::to_string(secs / 3600) + " h";
    return std::to_string(secs / 60) + " min";
}

MonitorsTab::MonitorsTab() {
    m_root = vbox(0);
    addClass(m_root, "bg");
    GtkWidget* bar = hbox(4);
    addClass(bar, "bg-toolbar");
    GtkWidget* add = button("_Add…", "list-add-symbolic", "Monitor a search for new images");
    onClicked(add, [this] {
        Monitor m;
        Settings& s = app().profile->settings();
        m.interval = (int)s.getInt("Monitoring/defaultInterval", 86400);
        m.delay = (int)s.getInt("Monitoring/defaultDelay", 0);
        m.download = s.getBool("Monitoring/defaultDownloadEnabled", false);
        m.notify = s.getBool("Monitoring/defaultNotificationEnabled", true);
        m.sites = s.getList("sites");
        m.lastSuccess = DateTime::nowUtc();
        if(editMonitorDialog(toplevel(m_root), m, true)) {
            app().profile->monitors().push_back(m);
            app().profile->syncMonitors();
            app().profile->monitorsChanged.emit();
        }
    });
    GtkWidget* edit = button("_Edit…", "document-edit-symbolic");
    onClicked(edit, [this] {
        Ref r;
        if(!selected(r)) return;
        Monitor* m = nullptr;
        if(r.favorite) {
            Favorite* f = app().profile->favorite(r.favoriteName);
            if(f && r.index < f->monitors.size()) m = &f->monitors[r.index];
        } else if(r.index < app().profile->monitors().size()) {
            m = &app().profile->monitors()[r.index];
        }
        if(!m) return;
        Monitor copy = *m;
        if(!editMonitorDialog(toplevel(m_root), copy, !r.favorite)) return;
        *m = copy;
        if(r.favorite)
            app().profile->syncFavorites();
        else
            app().profile->syncMonitors();
        refresh();
    });
    GtkWidget* remove = button("_Remove", "list-remove-symbolic");
    onClicked(remove, [this] {
        Ref r;
        if(!selected(r)) return;
        if(!confirm(toplevel(m_root), "Remove monitor", "Stop monitoring this search?")) return;
        if(r.favorite) {
            Favorite* f = app().profile->favorite(r.favoriteName);
            if(f && r.index < f->monitors.size()) f->monitors.erase(f->monitors.begin() + (long)r.index);
            app().profile->syncFavorites();
        } else {
            auto& ms = app().profile->monitors();
            if(r.index < ms.size()) ms.erase(ms.begin() + (long)r.index);
            app().profile->syncMonitors();
        }
        refresh();
    });
    GtkWidget* check = button("_Check now", "view-refresh-symbolic", "Check the selected monitor (or all when none is selected)");
    onClicked(check, [this] {
        Ref r;
        if(!selected(r)) {
            app().monitoring->checkAll();
            return;
        }
        if(r.favorite)
            app().monitoring->checkFavorite(r.favoriteName, r.index);
        else
            app().monitoring->checkStandalone(r.index);
    });
    GtkWidget* reset = button("Reset count", "edit-clear-symbolic", "Reset the number of new images");
    onClicked(reset, [this] {
        for(auto& m : app().profile->monitors()) m.cumulated = 0;
        for(auto& f : app().profile->favorites())
            for(auto& m : f.monitors) m.cumulated = 0;
        app().profile->syncMonitors();
        app().profile->syncFavorites();
        refresh();
    });
    for(GtkWidget* w : {add, edit, remove, check, reset}) gtk_box_pack_start(GTK_BOX(bar), w, FALSE, FALSE, 0);
    m_status = label("");
    gtk_box_pack_end(GTK_BOX(bar), m_status, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(m_root), bar, FALSE, FALSE, 0);

    m_store = gtk_list_store_new(M_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING);
    m_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m_store));
    const char* titles[] = {"Search", "Sites", "Interval", "Last check", "Next check", "New images", "Download", "State"};
    for(int i = 0; i < M_COLS; i++) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_expand(c, i == 0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(m_view), c);
    }
    GtkWidget* sw = scrolled(m_view, true);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_pack_start(GTK_BOX(m_root), sw, TRUE, TRUE, 0);
    GtkWidget* hint = label("Monitors check searches periodically for images newer than their last check, then notify you and/or download them. "
                            "Favorites can have monitors too (edit a favorite).");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    addClass(hint, "bg-statusbar");
    gtk_box_pack_start(GTK_BOX(m_root), hint, FALSE, FALSE, 0);
    gtk_widget_show_all(m_root);

    Profile* p = app().profile;
    m_connections.push_back({0, p->monitorsChanged.connect([this] { refresh(); })});
    m_connections.push_back({1, p->favoritesChanged.connect([this] { refresh(); })});
    m_connections.push_back({2, app().monitoring->statusChanged.connect([this] {
                                 gtk_label_set_text(GTK_LABEL(m_status), app().monitoring->checking() ? "Checking…" : "");
                                 refresh();
                             })});
    m_timer = g_timeout_add_seconds(30, [](gpointer d) -> gboolean {
        static_cast<MonitorsTab*>(d)->refresh();
        return G_SOURCE_CONTINUE;
    }, this);
    refresh();
}

MonitorsTab::~MonitorsTab() {
    if(m_timer) g_source_remove(m_timer);
    for(auto& [which, id] : m_connections) {
        if(which == 0) app().profile->monitorsChanged.disconnect(id);
        if(which == 1) app().profile->favoritesChanged.disconnect(id);
        if(which == 2) app().monitoring->statusChanged.disconnect(id);
    }
}

bool MonitorsTab::selected(Ref& ref) const {
    GtkTreeIter it;
    GtkTreeModel* model;
    if(!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(m_view)), &model, &it)) return false;
    GtkTreePath* path = gtk_tree_model_get_path(model, &it);
    size_t row = (size_t)gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if(row >= m_refs.size()) return false;
    ref = m_refs[row];
    return true;
}

void MonitorsTab::refresh() {
    gtk_list_store_clear(m_store);
    m_refs.clear();
    auto add = [this](const Monitor& m, const std::string& search, Ref ref) {
        GtkTreeIter it;
        gtk_list_store_append(m_store, &it);
        std::string next = m.lastCheck.valid() ? m.nextCheck().toDisplayString() : "soon";
        std::string news = std::to_string(m.cumulated) + (m.preciseCumulated ? "" : "+");
        gtk_list_store_set(m_store, &it, M_SEARCH, search.c_str(), M_SITES, join(m.sites, ", ").c_str(), M_INTERVAL, formatInterval(m.interval).c_str(),
                           M_LAST, m.lastCheck.valid() ? m.lastCheck.toDisplayString().c_str() : "never", M_NEXT, next.c_str(), M_NEW, news.c_str(),
                           M_DOWNLOAD, m.download ? "yes" : "no", M_STATE, m.lastState.c_str(), -1);
        m_refs.push_back(ref);
    };
    auto& monitors = app().profile->monitors();
    for(size_t i = 0; i < monitors.size(); i++) add(monitors[i], monitors[i].query.toString(), {false, "", i});
    for(auto& f : app().profile->favorites())
        for(size_t i = 0; i < f.monitors.size(); i++) add(f.monitors[i], f.name + " (favorite)", {true, f.name, i});
}

}  // namespace bg::gui
