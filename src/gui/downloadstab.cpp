#include "gui/downloadstab.hpp"

#include "core/profile.hpp"
#include "core/site.hpp"
#include "gui/dialogs.hpp"
#include "gui/mainwindow.hpp"

namespace bg::gui {

enum { G_SEARCH, G_SITE, G_PAGE, G_PERPAGE, G_TOTAL, G_FILENAME, G_FOLDER, G_FILTER, G_PROGRESS, G_PROGRESS_TEXT, G_STATUS, G_COLS };
enum { U_SITE, U_ID, U_FILENAME, U_FOLDER, U_STATUS, U_COLS };

static const char* statusName(DownloadQueue::Group::Status s) {
    switch(s) {
        case DownloadQueue::Group::Status::Pending: return "Waiting";
        case DownloadQueue::Group::Status::Running: return "Downloading";
        case DownloadQueue::Group::Status::Paused: return "Paused";
        case DownloadQueue::Group::Status::Done: return "Done";
        case DownloadQueue::Group::Status::Failed: return "Failed";
    }
    return "";
}

static GtkWidget* treeView(GtkListStore* store, const std::vector<std::pair<const char*, int>>& columns, int progressCol = -1, int progressTextCol = -1) {
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), GTK_SELECTION_MULTIPLE);
    for(auto& [title, col] : columns) {
        GtkTreeViewColumn* c;
        if(col == progressCol) {
            GtkCellRenderer* r = gtk_cell_renderer_progress_new();
            c = gtk_tree_view_column_new_with_attributes(title, r, "value", col, "text", progressTextCol, nullptr);
            gtk_tree_view_column_set_min_width(c, 140);
        } else {
            GtkCellRenderer* r = gtk_cell_renderer_text_new();
            g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, nullptr);
            gtk_tree_view_column_set_expand(c, col == 0);
            gtk_tree_view_column_set_min_width(c, 60);
        }
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
    }
    return view;
}

DownloadsTab::DownloadsTab() {
    m_root = vbox(0);
    addClass(m_root, "bg");
    GtkWidget* bar = hbox(4);
    addClass(bar, "bg-toolbar");
    GtkWidget* add = button("_Add search…", "list-add-symbolic", "Add a search to download");
    onClicked(add, [this] {
        DownloadQueryGroup g;
        Settings& s = app().profile->settings();
        StringList sites = s.getList("sites");
        g.site = sites.empty() ? nullptr : app().profile->site(sites[0]);
        g.filename = s.get("Save/filename", "%md5%.%ext%");
        g.path = app().profile->savePath();
        g.getBlacklisted = s.getBool("downloadblacklist", false);
        if(editGroupDialog(toplevel(m_root), g, "Add a search to download") && g.site) app().queue->addGroup(g);
    });
    m_startBtn = button("_Start", "media-playback-start-symbolic", "Download the selected searches, or everything when nothing is selected");
    onClicked(m_startBtn, [this] {
        auto sel = selectedGroups();
        if(sel.empty())
            app().queue->start();
        else
            app().queue->startGroups(sel);
    });
    m_pauseBtn = button("_Pause", "media-playback-pause-symbolic");
    onClicked(m_pauseBtn, [] { app().queue->pause(); });
    GtkWidget* reset = button("Reset", "view-refresh-symbolic", "Restart the selected searches from the beginning");
    onClicked(reset, [this] {
        for(auto i : selectedGroups()) app().queue->resetGroup(i);
    });
    GtkWidget* remove = button("_Remove", "list-remove-symbolic", "Remove the selected rows");
    onClicked(remove, [this] {
        auto groups = selectedGroups(), uniques = selectedUniques();
        for(auto it = groups.rbegin(); it != groups.rend(); ++it) app().queue->removeGroup(*it);
        for(auto it = uniques.rbegin(); it != uniques.rend(); ++it) app().queue->removeUnique(*it);
    });
    GtkWidget* clear = button("Clear finished", "edit-clear-symbolic");
    onClicked(clear, [] { app().queue->clearFinished(); });
    GtkWidget* folder = iconButton("folder-open-symbolic", "Open the download folder");
    onClicked(folder, [] { openUri(app().profile->savePath()); });
    for(GtkWidget* w : {add, m_startBtn, m_pauseBtn, reset, remove, clear}) gtk_box_pack_start(GTK_BOX(bar), w, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), folder, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), bar, FALSE, FALSE, 0);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    m_groupStore = gtk_list_store_new(G_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    m_groups = treeView(m_groupStore,
                        {{"Search", G_SEARCH},
                         {"Site", G_SITE},
                         {"Page", G_PAGE},
                         {"Per page", G_PERPAGE},
                         {"Total", G_TOTAL},
                         {"Filename", G_FILENAME},
                         {"Folder", G_FOLDER},
                         {"Post-filter", G_FILTER},
                         {"Progress", G_PROGRESS},
                         {"Result", G_STATUS}},
                        G_PROGRESS, G_PROGRESS_TEXT);
    on<void, GtkTreeView*, GtkTreePath*, GtkTreeViewColumn*>(m_groups, "row-activated", [this](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*) {
        editGroup((size_t)gtk_tree_path_get_indices(path)[0]);
    });
    GtkWidget* top = vbox(0);
    gtk_box_pack_start(GTK_BOX(top), section("Searches"), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(top), scrolled(m_groups, true), TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), top, TRUE, FALSE);

    m_uniqueStore = gtk_list_store_new(U_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    m_uniques = treeView(m_uniqueStore, {{"Site", U_SITE}, {"Image", U_ID}, {"Filename", U_FILENAME}, {"Folder", U_FOLDER}, {"Status", U_STATUS}});
    GtkWidget* bottom = vbox(0);
    gtk_box_pack_start(GTK_BOX(bottom), section("Single images"), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bottom), scrolled(m_uniques, true), TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), bottom, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 300);
    gtk_container_set_border_width(GTK_CONTAINER(paned), 6);
    gtk_box_pack_start(GTK_BOX(m_root), paned, TRUE, TRUE, 0);

    GtkWidget* status = hbox(8);
    addClass(status, "bg-statusbar");
    m_current = label("");
    gtk_label_set_ellipsize(GTK_LABEL(m_current), PANGO_ELLIPSIZE_MIDDLE);
    m_progress = gtk_progress_bar_new();
    gtk_widget_set_size_request(m_progress, 200, -1);
    gtk_widget_set_valign(m_progress, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(status), m_current, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status), m_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), status, FALSE, FALSE, 0);

    m_tabLabel = label("Downloads");
    gtk_widget_show_all(m_root);

    DownloadQueue* q = app().queue;
    m_connections.push_back({0, q->changed.connect([this] { refresh(); })});
    m_connections.push_back({1, q->progressChanged.connect([this] { updateProgress(); })});
    m_connections.push_back({2, q->imageProgress.connect([this](const ImagePtr& img, int64_t a, int64_t b) {
                                 if(b > 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progress), (double)a / (double)b);
                                 m_currentFile = img->url();
                                 gtk_label_set_text(GTK_LABEL(m_current), ("Downloading " + m_currentFile).c_str());
                             })});
    m_connections.push_back({3, q->imageSaved.connect([this](const ImagePtr&, const SaveResults& r) {
                                 if(r.empty()) return;
                                 gtk_label_set_text(GTK_LABEL(m_current), (std::string(saveResultName(r[0].result)) + ": " + r[0].path).c_str());
                             })});
    m_connections.push_back({4, q->finished.connect([this] {
                                 gtk_label_set_text(GTK_LABEL(m_current), "All downloads finished");
                                 gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progress), 0);
                                 if(app().window) app().window->notify("Downloads finished", "All queued downloads are done.");
                             })});
    refresh();
}

DownloadsTab::~DownloadsTab() {
    DownloadQueue* q = app().queue;
    for(auto& [which, id] : m_connections) {
        switch(which) {
            case 0: q->changed.disconnect(id); break;
            case 1: q->progressChanged.disconnect(id); break;
            case 2: q->imageProgress.disconnect(id); break;
            case 3: q->imageSaved.disconnect(id); break;
            case 4: q->finished.disconnect(id); break;
        }
    }
}

static void setGroupRow(GtkListStore* store, GtkTreeIter* it, const DownloadQueue::Group& g) {
    const auto& q = *g.query;
    int total = q.total > 0 ? q.total : std::max(1, q.progressVal);
    int pct = std::clamp(total > 0 ? q.progressVal * 100 / total : 0, 0, 100);
    if(g.status == DownloadQueue::Group::Status::Done) pct = 100;
    std::string progressText = std::to_string(q.progressVal) + " / " + (q.total > 0 ? std::to_string(q.total) : std::string("all"));
    std::string status = std::string(statusName(g.status));
    if(g.status == DownloadQueue::Group::Status::Running || g.downloaded || g.errors)
        status += " — " + std::to_string(g.downloaded) + " ok, " + std::to_string(g.alreadyExists) + " existing, " + std::to_string(g.ignored) +
                  " ignored, " + std::to_string(g.errors) + " errors";
    gtk_list_store_set(store, it, G_SEARCH, q.query.toString().c_str(), G_SITE, q.site ? q.site->url().c_str() : "", G_PAGE,
                       std::to_string(q.page).c_str(), G_PERPAGE, std::to_string(q.perpage).c_str(), G_TOTAL,
                       (q.total > 0 ? std::to_string(q.total) : std::string("all")).c_str(), G_FILENAME, q.filename.c_str(), G_FOLDER,
                       q.path.c_str(), G_FILTER, join(q.postFiltering, " ").c_str(), G_PROGRESS, pct, G_PROGRESS_TEXT, progressText.c_str(),
                       G_STATUS, status.c_str(), -1);
}

void DownloadsTab::refresh() {
    DownloadQueue* q = app().queue;
    gtk_list_store_clear(m_groupStore);
    for(auto& g : q->groups()) {
        GtkTreeIter it;
        gtk_list_store_append(m_groupStore, &it);
        setGroupRow(m_groupStore, &it, g);
    }
    gtk_list_store_clear(m_uniqueStore);
    for(auto& u : q->uniques()) {
        static const char* names[] = {"Waiting", "Downloading", "Done", "Failed"};
        std::string status = names[(int)u.status];
        if(!u.message.empty()) status += " — " + u.message;
        std::string id = u.query.image->id() ? "#" + std::to_string(u.query.image->id()) : u.query.image->url();
        GtkTreeIter it;
        gtk_list_store_append(m_uniqueStore, &it);
        gtk_list_store_set(m_uniqueStore, &it, U_SITE, u.query.site ? u.query.site->url().c_str() : "", U_ID, id.c_str(), U_FILENAME,
                           u.query.filename.c_str(), U_FOLDER, u.query.path.c_str(), U_STATUS, status.c_str(), -1);
    }
    gtk_widget_set_sensitive(m_startBtn, !q->running());
    gtk_widget_set_sensitive(m_pauseBtn, q->running());
    int pending = 0;
    for(auto& g : q->groups())
        if(g.status != DownloadQueue::Group::Status::Done) pending++;
    for(auto& u : q->uniques())
        if(u.status != DownloadQueue::Unique::Status::Done) pending++;
    gtk_label_set_text(GTK_LABEL(m_tabLabel), pending ? ("Downloads (" + std::to_string(pending) + ")").c_str() : "Downloads");
}

void DownloadsTab::updateProgress() {
    DownloadQueue* q = app().queue;
    int current = q->currentGroup();
    if(current < 0 || current >= (int)q->groups().size()) return;
    GtkTreeIter it;
    if(gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(m_groupStore), &it, nullptr, current)) setGroupRow(m_groupStore, &it, q->groups()[(size_t)current]);
}

static std::vector<size_t> selectedRows(GtkWidget* view) {
    std::vector<size_t> out;
    GList* rows = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), nullptr);
    for(GList* l = rows; l; l = l->next) out.push_back((size_t)gtk_tree_path_get_indices((GtkTreePath*)l->data)[0]);
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<size_t> DownloadsTab::selectedGroups() const { return selectedRows(m_groups); }
std::vector<size_t> DownloadsTab::selectedUniques() const { return selectedRows(m_uniques); }

void DownloadsTab::editGroup(size_t index) {
    DownloadQueue* q = app().queue;
    if(index >= q->groups().size() || (q->running() && (int)index == q->currentGroup())) return;
    DownloadQueryGroup g = *q->groups()[index].query;
    if(!editGroupDialog(toplevel(m_root), g, "Edit download")) return;
    *q->groups()[index].query = g;
    q->groups()[index].total = g.total;
    q->save();
    refresh();
}

}  // namespace bg::gui
