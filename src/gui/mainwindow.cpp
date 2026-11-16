#include "gui/mainwindow.hpp"

#include "config.hpp"
#include "core/logger.hpp"
#include "core/monitoring.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"
#include "gui/dialogs.hpp"
#include "gui/downloadstab.hpp"
#include "gui/favoritestab.hpp"
#include "gui/logtab.hpp"
#include "gui/monitorstab.hpp"
#include "gui/searchtab.hpp"
#include "gui/settingsdialog.hpp"
#include "gui/sitesdialog.hpp"
#include "gui/style.hpp"
#include "gui/tray.hpp"
#include "gui/viewer.hpp"

namespace bg::gui {

MainWindow::MainWindow(GtkApplication* gtkApp) {
    Settings& s = app().profile->settings();
    m_window = gtk_application_window_new(gtkApp);
    addClass(m_window, "bg");
    gtk_window_set_title(GTK_WINDOW(m_window), BG_APP_NAME);
    gtk_window_set_icon_name(GTK_WINDOW(m_window), "booru-grabber");
    gtk_window_set_default_size(GTK_WINDOW(m_window), (int)s.getInt("Main/width", 1280), (int)s.getInt("Main/height", 820));
    if(s.getBool("Main/maximized", false)) gtk_window_maximize(GTK_WINDOW(m_window));
    on<gboolean, GtkWidget*, GdkEvent*>(m_window, "delete-event", [this](GtkWidget*, GdkEvent*) -> gboolean { return onDelete(); });
    onKeyPress(m_window, [this](GdkEventKey* e) { return onKey(e); });

    GtkWidget* root = vbox(0);
    buildMenu(root);

    m_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    buildSidebar();
    // The sidebar never shrinks below its minimum width (a collapsed pane is invisible and was saved as 1 px)
    gtk_paned_pack1(GTK_PANED(m_paned), m_sidebar, FALSE, FALSE);
    m_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(m_notebook), TRUE);
    GtkWidget* newTab = iconButton("tab-new-symbolic", "New tab (Ctrl+T)");
    gtk_button_set_relief(GTK_BUTTON(newTab), GTK_RELIEF_NONE);
    onClicked(newTab, [this] { addSearchTab({}, {}, true, nullptr, false); });
    gtk_widget_show(newTab);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(m_notebook), newTab, GTK_PACK_END);
    gtk_paned_pack2(GTK_PANED(m_paned), m_notebook, TRUE, FALSE);
    gtk_widget_set_size_request(m_sidebar, 160, -1);
    int sidebarWidth = std::max(160, (int)s.getInt("Main/sidebarWidth", 240));
    // Only on the first map: restoring the window from the tray must keep the position the user dragged to
    on<void, GtkWidget*>(m_window, "map", [this, sidebarWidth](GtkWidget*) {
        if(m_sidebarPlaced) return;
        m_sidebarPlaced = true;
        gtk_paned_set_position(GTK_PANED(m_paned), sidebarWidth);
    });
    gtk_box_pack_start(GTK_BOX(root), m_paned, TRUE, TRUE, 0);

    GtkWidget* status = hbox(8);
    addClass(status, "bg-statusbar");
    m_status = label("");
    gtk_label_set_ellipsize(GTK_LABEL(m_status), PANGO_ELLIPSIZE_END);
    m_downloadStatus = label("");
    gtk_box_pack_start(GTK_BOX(status), m_status, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status), m_downloadStatus, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(m_window), root);

    // Fixed tabs: Favorites first; Downloads, Monitors and Log at the end
    m_favorites = std::make_unique<FavoritesTab>(this);
    gtk_notebook_append_page(GTK_NOTEBOOK(m_notebook), m_favorites->widget(), label("Favorites"));
    m_downloads = std::make_unique<DownloadsTab>();
    gtk_notebook_append_page(GTK_NOTEBOOK(m_notebook), m_downloads->widget(), m_downloads->tabLabel());
    m_monitors = std::make_unique<MonitorsTab>();
    gtk_notebook_append_page(GTK_NOTEBOOK(m_notebook), m_monitors->widget(), label("Monitors"));
    m_log = std::make_unique<LogTab>();
    gtk_notebook_append_page(GTK_NOTEBOOK(m_notebook), m_log->widget(), label("Log"));
    m_fixedTabs = 3;
    on<void, GtkNotebook*, GtkWidget*, guint>(m_notebook, "switch-page", [this](GtkNotebook*, GtkWidget*, guint) {
        defer([this] { updateTags(); });
    });

    Profile* p = app().profile;
    auto refreshLists = [this] {
        updateSidebarLists();
        for(auto* t : m_tabs) t->refreshStates();
        updateTags();
    };
    m_connections.push_back({&p->favoritesChanged, p->favoritesChanged.connect(refreshLists)});
    m_connections.push_back({&p->keptForLaterChanged, p->keptForLaterChanged.connect(refreshLists)});
    m_connections.push_back({&p->blacklistChanged, p->blacklistChanged.connect(refreshLists)});
    m_connections.push_back({&p->ignoredChanged, p->ignoredChanged.connect(refreshLists)});
    m_connections.push_back({&app().queue->changed, app().queue->changed.connect([this] {
                                 DownloadQueue* q = app().queue;
                                 gtk_label_set_text(GTK_LABEL(m_downloadStatus), q->running() ? "Downloading…" : "");
                             })});
    app().monitoring->notify.connect([this](const std::string& title, const std::string& body) { notify(title, body); });

    gtk_widget_show_all(m_window);
    updateSidebarLists();
    restoreSession();
}

MainWindow::~MainWindow() {
    for(auto& [sig, id] : m_connections) sig->disconnect(id);
}

void MainWindow::buildMenu(GtkWidget* box) {
    GtkWidget* bar = gtk_menu_bar_new();
    auto top = [&](const char* name) {
        GtkWidget* item = gtk_menu_item_new_with_mnemonic(name);
        GtkWidget* menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
        return menu;
    };
    GtkWidget* file = top("_File");
    menuItem(file, "_New tab\tCtrl+T", [this] { addSearchTab({}, {}, true, nullptr, false); });
    menuItem(file, "Reopen closed tab\tCtrl+Shift+T", [this] {
        if(m_closedTabs.empty()) return;
        nlohmann::json j = m_closedTabs.back();
        m_closedTabs.pop_back();
        restoreTab(j);
    });
    menuItem(file, "_Close tab\tCtrl+W", [this] {
        if(auto* t = currentSearchTab()) closeTab(t);
    });
    menuSeparator(file);
    menuItem(file, "_Import Grabber profile…", [this] { importGrabberDialog(window()); });
    menuItem(file, "Open _download folder", [] { openUri(app().profile->savePath()); });
    menuItem(file, "Open _profile folder", [] { openUri(paths::configDir()); });
    menuSeparator(file);
    menuItem(file, "_Quit\tCtrl+Q", [this] { requestQuit(); });

    GtkWidget* view = top("_View");
    checkMenuItem(view, "Sidebar\tF9", true, [this](bool on) { gtk_widget_set_visible(m_sidebar, on); });
    menuItem(view, "_Favorites", [this] { showFavorites(); });
    menuItem(view, "_Downloads", [this] { showDownloads(); });
    menuItem(view, "_Monitors", [this] { gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), m_monitors->widget())); });
    menuItem(view, "_Log", [this] { gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), m_log->widget())); });

    GtkWidget* sites = top("_Sites");
    menuItem(sites, "_Choose sites…", [this] {
        if(SearchTab* t = currentSearchTab()) {
            StringList chosen = t->sites();
            if(chooseSitesDialog(window(), chosen) && !chosen.empty()) {
                t->setSites(chosen);
                app().profile->settings().setList("sites", chosen);
                t->search(1);
            }
        } else {
            StringList chosen = app().profile->settings().getList("sites");
            if(chooseSitesDialog(window(), chosen) && !chosen.empty()) app().profile->settings().setList("sites", chosen);
        }
    });
    menuItem(sites, "_Add site…", [this] {
        if(addSiteDialog(window())) reloadSites();
    });
    menuItem(sites, "Site _settings…", [this] {
        SearchTab* t = currentSearchTab();
        StringList s = t ? t->sites() : app().profile->settings().getList("sites");
        if(s.empty()) return;
        if(auto site = app().profile->site(s[0])) siteSettingsDialog(window(), site.get());
    });

    GtkWidget* tools = top("_Tools");
    menuItem(tools, "_Settings…\tCtrl+P", [this] { settingsDialog(window()); });
    menuSeparator(tools);
    menuItem(tools, "_MD5 database…", [this] { md5DatabaseDialog(window()); });
    menuItem(tools, "Remove _empty folders…", [this] { emptyFoldersDialog(window()); });
    menuItem(tools, "Load _tag database…", [this] { tagLoaderDialog(window()); });
    menuItem(tools, "Clear search _history", [] { app().profile->clearHistory(); });

    GtkWidget* help = top("_Help");
    menuItem(help, "Filename _format", [] { openUri(std::string(BG_HOMEPAGE) + "/blob/master/docs/filenames.md"); });
    menuItem(help, "_Project page", [] { openUri(BG_HOMEPAGE); });
    menuItem(help, "_About", [this] { showAbout(window()); });
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
}

static GtkWidget* sidebarList(GtkWidget** list) {
    *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(*list), GTK_SELECTION_NONE);
    return scrolled(*list);
}

void MainWindow::buildSidebar() {
    m_sidebar = vbox(0);
    addClass(m_sidebar, "bg-sidebar");
    GtkWidget* stack = gtk_stack_new();
    GtkWidget* switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(switcher, 4);
    gtk_widget_set_margin_bottom(switcher, 4);
    gtk_stack_add_titled(GTK_STACK(stack), sidebarList(&m_tagsList), "tags", "Tags");
    gtk_stack_add_titled(GTK_STACK(stack), sidebarList(&m_favoritesList), "favorites", "Favs");
    gtk_stack_add_titled(GTK_STACK(stack), sidebarList(&m_kflList), "kfl", "Later");
    m_wikiLabel = label("");
    gtk_label_set_line_wrap(GTK_LABEL(m_wikiLabel), TRUE);
    // Long words and URLs break too, otherwise they set the minimum width of the whole sidebar
    gtk_label_set_line_wrap_mode(GTK_LABEL(m_wikiLabel), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(m_wikiLabel), TRUE);
    gtk_widget_set_valign(m_wikiLabel, GTK_ALIGN_START);
    gtk_widget_set_margin_start(m_wikiLabel, 8);
    gtk_widget_set_margin_end(m_wikiLabel, 8);
    gtk_stack_add_titled(GTK_STACK(stack), scrolled(m_wikiLabel), "wiki", "Wiki");
    gtk_box_pack_start(GTK_BOX(m_sidebar), switcher, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_sidebar), stack, TRUE, TRUE, 0);
}

static void clearList(GtkWidget* list) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(list));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static void addTagRow(MainWindow* w, GtkWidget* list, const std::string& tag, const std::string& markup, const std::string& tooltip = "") {
    GtkWidget* l = label(markup, 0, true);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    GtkWidget* ev = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(ev), l);
    if(!tooltip.empty()) gtk_widget_set_tooltip_text(ev, tooltip.c_str());
    onButtonPress(ev, [w, tag](GdkEventButton* e) {
        if(e->type != GDK_BUTTON_PRESS) return false;
        bool ctrl = e->state & GDK_CONTROL_MASK;
        if(e->button == 1) w->openSearch(tag, ctrl, w->currentSearchTab());
        if(e->button == 2) w->openSearch(tag, true);
        if(e->button == 3) w->tagMenu(tag, (GdkEvent*)e, w->currentSearchTab());
        return true;
    });
    gtk_list_box_insert(GTK_LIST_BOX(list), ev, -1);
}

void MainWindow::updateSidebarLists() {
    clearList(m_favoritesList);
    std::vector<Favorite> favs = app().profile->favorites();
    std::sort(favs.begin(), favs.end(), [](const Favorite& a, const Favorite& b) { return naturalCompare(toLower(a.name), toLower(b.name)) < 0; });
    for(auto& f : favs) addTagRow(this, m_favoritesList, f.name, htmlEscape(replaceAll(f.name, "_", " ")));
    clearList(m_kflList);
    for(auto& t : app().profile->keptForLater()) addTagRow(this, m_kflList, t, htmlEscape(replaceAll(t, "_", " ")));
    gtk_widget_show_all(m_favoritesList);
    gtk_widget_show_all(m_kflList);
}

void MainWindow::updateTags() {
    clearList(m_tagsList);
    SearchTab* tab = currentSearchTab();
    if(!tab) {
        gtk_label_set_text(GTK_LABEL(m_wikiLabel), "");
        return;
    }
    TagList tags = tab->tags();
    sortTags(tags, app().profile->settings().get("tagsorder", "type") == "count" ? "count" : "type");
    bool showCount = true;
    for(auto& t : tags) addTagRow(this, m_tagsList, t.text(), tagMarkup(t.text(), t.type().name(), t.count(), showCount), t.type().name());
    gtk_widget_show_all(m_tagsList);
    std::string wiki = tab->wiki();
    gtk_label_set_text(GTK_LABEL(m_wikiLabel), trim(decodeHtmlEntities(stripTags(replaceAll(replaceAll(wiki, "<br>", "\n"), "<br/>", "\n")))).c_str());
    gtk_window_set_title(GTK_WINDOW(m_window), (tab->title() + " — " BG_APP_NAME).c_str());
}

// ---- tabs ----------------------------------------------------------------------------

SearchTab* MainWindow::addSearchTab(const StringList& tags, const StringList& sites, bool focus, const ImagePtr& gallery, bool searchNow) {
    auto* tab = new SearchTab(this, tags, sites, gallery);
    m_tabs.push_back(tab);
    int pos = gtk_notebook_get_n_pages(GTK_NOTEBOOK(m_notebook)) - m_fixedTabs;
    gtk_notebook_insert_page(GTK_NOTEBOOK(m_notebook), tab->widget(), tab->tabLabel(), pos);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(m_notebook), tab->widget(), TRUE);
    tab->onClose = [this, tab] { closeTab(tab); };
    if(focus) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), pos);
        tab->focusSearch();
    }
    if(searchNow && (!tags.empty() || gallery)) tab->search(1);
    return tab;
}

void MainWindow::closeTab(SearchTab* tab) {
    auto it = std::find(m_tabs.begin(), m_tabs.end(), tab);
    if(it == m_tabs.end()) return;
    m_closedTabs.push_back(tab->toJson());
    if(m_closedTabs.size() > 20) m_closedTabs.erase(m_closedTabs.begin());
    m_tabs.erase(it);
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), tab->widget());
    if(page >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(m_notebook), page);
    delete tab;
    updateTags();
}

SearchTab* MainWindow::currentSearchTab() const {
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(m_notebook));
    GtkWidget* w = gtk_notebook_get_nth_page(GTK_NOTEBOOK(m_notebook), page);
    for(auto* t : m_tabs)
        if(t->widget() == w) return t;
    return nullptr;
}

void MainWindow::tabChanged(SearchTab* tab) {
    if(tab == currentSearchTab()) updateTags();
}

void MainWindow::showDownloads() {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), m_downloads->widget()));
}

bool MainWindow::showPage(const std::string& name) {
    GtkWidget* page = name == "favorites" ? m_favorites->widget()
                    : name == "downloads" ? m_downloads->widget()
                    : name == "monitors"  ? m_monitors->widget()
                    : name == "log"       ? m_log->widget()
                                          : nullptr;
    if(!page) return false;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), page));
    return true;
}

void MainWindow::showFavorites() {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), m_favorites->widget()));
}

void MainWindow::present() {
    gtk_widget_show(m_window);
    gtk_window_present(GTK_WINDOW(m_window));
}

void MainWindow::setStatus(const std::string& text) { gtk_label_set_text(GTK_LABEL(m_status), text.c_str()); }

void MainWindow::notify(const std::string& title, const std::string& body) {
    GNotification* n = g_notification_new(title.c_str());
    g_notification_set_body(n, body.c_str());
    GIcon* icon = g_themed_icon_new("booru-grabber");
    g_notification_set_icon(n, icon);
    g_object_unref(icon);
    g_application_send_notification(G_APPLICATION(app().gtk), nullptr, n);
    g_object_unref(n);
}

// ---- actions ------------------------------------------------------------------------------

void MainWindow::openViewer(const std::vector<ImagePtr>& images, size_t index, SearchTab*) {
    if(images.empty()) return;
    if(ViewerWindow* single = ViewerWindow::single()) {
        single->setImages(images, index);
        return;
    }
    new ViewerWindow(this, images, index);  // deletes itself when closed
}

void MainWindow::openSearch(const std::string& tags, bool newTab, SearchTab* from) {
    if(newTab || !from) {
        addSearchTab(split(tags, ' ', true), from ? from->sites() : StringList(), true);
        return;
    }
    from->setQuery(tags);
    from->search(1);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), from->widget()));
}

void MainWindow::tagMenu(const std::string& tag, GdkEvent* event, SearchTab* from) {
    Profile* p = app().profile;
    GtkWidget* menu = gtk_menu_new();
    menuItem(menu, "Open in a _new tab", [this, tag, from] { openSearch(tag, true, from); });
    if(from) {
        menuItem(menu, "_Add to the search", [from, tag] {
            from->setQuery(trim(from->query()) + " " + tag);
            from->search(1);
        });
        menuItem(menu, "E_xclude from the search", [from, tag] {
            from->setQuery(trim(from->query()) + " -" + tag);
            from->search(1);
        });
    }
    menuSeparator(menu);
    if(p->favorite(tag))
        menuItem(menu, "Remove from _favorites", [p, tag] { p->removeFavorite(tag); });
    else
        menuItem(menu, "Add to _favorites", [this, tag] { addFavorite(window(), tag); });
    if(contains(p->keptForLater(), tag))
        menuItem(menu, "Remove from _view later", [p, tag] { p->removeKeptForLater(tag); });
    else
        menuItem(menu, "_View later", [p, tag] { p->addKeptForLater(tag); });
    if(p->blacklist().contains(replaceAll(tag, ":", "::")) || p->blacklist().contains(tag))
        menuItem(menu, "Remove from the _blacklist", [p, tag] { p->removeBlacklistedTag(tag); });
    else
        menuItem(menu, "Add to the _blacklist", [p, tag] { p->addBlacklistedTag(tag); });
    if(containsCi(p->ignored(), tag))
        menuItem(menu, "Stop _ignoring", [p, tag] { p->removeIgnored(tag); });
    else
        menuItem(menu, "_Ignore (treat as general tag in filenames)", [p, tag] { p->addIgnored(tag); });
    menuItem(menu, "_Monitor…", [this, tag, from] {
        Monitor m;
        Settings& s = app().profile->settings();
        m.query.tags = split(tag, ' ', true);
        m.sites = from ? from->sites() : s.getList("sites");
        m.interval = (int)s.getInt("Monitoring/defaultInterval", 86400);
        m.download = s.getBool("Monitoring/defaultDownloadEnabled", false);
        m.notify = s.getBool("Monitoring/defaultNotificationEnabled", true);
        m.lastSuccess = DateTime::nowUtc();
        if(editMonitorDialog(window(), m, true)) {
            app().profile->monitors().push_back(m);
            app().profile->syncMonitors();
            app().profile->monitorsChanged.emit();
        }
    });
    menuSeparator(menu);
    menuItem(menu, "_Copy", [tag] { copyToClipboard(tag); });
    popupMenu(menu, event);
}

void MainWindow::addImageToDownloads(const ImagePtr& img) {
    DownloadQueryImage q;
    q.image = img;
    q.site = img->sitePtr();
    q.filename = app().profile->settings().get("Save/filename", "%md5%.%ext%");
    q.path = app().profile->savePath();
    app().queue->addUnique(q);
    setStatus("Added to the downloads: " + img->url());
}

void MainWindow::addSearchToDownloads(SearchTab* tab, bool allPages) {
    if(!allPages) {
        for(auto& img : tab->images()) addImageToDownloads(img);
        setStatus("Added " + std::to_string(tab->images().size()) + " images to the downloads");
        return;
    }
    Settings& s = app().profile->settings();
    for(auto& [url, page] : tab->pages()) {
        DownloadQueryGroup g;
        g.query = page->query();
        g.site = page->sitePtr();
        g.page = 1;
        g.perpage = std::max(tab->perPage(), page->highLimit() > 0 ? std::min(page->highLimit(), 200) : tab->perPage());
        int count = page->imagesCount();
        g.total = count > 0 ? count : -1;
        g.postFiltering = tab->postFilters();
        g.getBlacklisted = s.getBool("downloadblacklist", false);
        g.filename = s.get("Save/filename", "%md5%.%ext%");
        g.path = app().profile->savePath();
        app().queue->addGroup(g);
    }
    showDownloads();
}

void MainWindow::reloadSites() {
    for(auto* t : m_tabs) t->setSites(t->sites());
}

// ---- keyboard, closing, session ------------------------------------------------------------

bool MainWindow::onKey(GdkEventKey* e) {
    bool ctrl = e->state & GDK_CONTROL_MASK, shift = e->state & GDK_SHIFT_MASK;
    SearchTab* tab = currentSearchTab();
    if(ctrl && (e->keyval == GDK_KEY_t || e->keyval == GDK_KEY_T)) {
        if(shift && !m_closedTabs.empty()) {
            nlohmann::json j = m_closedTabs.back();
            m_closedTabs.pop_back();
            restoreTab(j);
        } else if(!shift) {
            addSearchTab({}, {}, true, nullptr, false);
        }
        return true;
    }
    if(ctrl && e->keyval == GDK_KEY_w) {
        if(tab) closeTab(tab);
        return true;
    }
    if(ctrl && e->keyval == GDK_KEY_q) {
        requestQuit();
        return true;
    }
    if(ctrl && e->keyval == GDK_KEY_p) {
        settingsDialog(window());
        return true;
    }
    if(ctrl && e->keyval == GDK_KEY_l && tab) {
        tab->focusSearch();
        return true;
    }
    if(e->keyval == GDK_KEY_F5 && tab) {
        tab->search(tab->page());
        return true;
    }
    if(e->keyval == GDK_KEY_F9) {
        gtk_widget_set_visible(m_sidebar, !gtk_widget_get_visible(m_sidebar));
        return true;
    }
    if(ctrl && (e->keyval == GDK_KEY_Page_Down || e->keyval == GDK_KEY_Tab)) {
        gtk_notebook_next_page(GTK_NOTEBOOK(m_notebook));
        return true;
    }
    if(ctrl && (e->keyval == GDK_KEY_Page_Up || e->keyval == GDK_KEY_ISO_Left_Tab)) {
        gtk_notebook_prev_page(GTK_NOTEBOOK(m_notebook));
        return true;
    }
    return false;
}

bool MainWindow::onDelete() {
    Settings& s = app().profile->settings();
    if(!m_quitting && s.getBool("Monitoring/enableTray", false) && s.getBool("Monitoring/closeToTray", false) && trayAvailable()) {
        gtk_widget_hide(m_window);
        return true;
    }
    requestQuit();
    return true;
}

void MainWindow::requestQuit() {
    Settings& s = app().profile->settings();
    if(app().queue->running() && s.getBool("confirm_close", true) &&
       !confirm(window(), "Quit while downloading?", "Downloads are still running. They will resume where they stopped on the next start."))
        return;
    m_quitting = true;
    saveSession();
    int w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(m_window), &w, &h);
    bool maximized = gtk_window_is_maximized(GTK_WINDOW(m_window));
    if(!maximized) {
        s.setInt("Main/width", w);
        s.setInt("Main/height", h);
    }
    s.setBool("Main/maximized", maximized);
    // A hidden or never-shown sidebar has no meaningful width
    int sidebarWidth = gtk_paned_get_position(GTK_PANED(m_paned));
    if(m_sidebarPlaced && gtk_widget_get_visible(m_sidebar) && sidebarWidth >= 160) s.setInt("Main/sidebarWidth", sidebarWidth);
    app().queue->pause();
    g_application_quit(G_APPLICATION(app().gtk));
}

void MainWindow::saveSession() {
    nlohmann::json tabs = nlohmann::json::array();
    int current = 0, index = 0;
    SearchTab* cur = currentSearchTab();
    for(int i = 0; i < gtk_notebook_get_n_pages(GTK_NOTEBOOK(m_notebook)); i++) {
        GtkWidget* w = gtk_notebook_get_nth_page(GTK_NOTEBOOK(m_notebook), i);
        for(auto* t : m_tabs) {
            if(t->widget() != w) continue;
            if(t == cur) current = index;
            tabs.push_back(t->toJson());
            index++;
        }
    }
    writeFile(buildPath(paths::dataDir(), "tabs.json"), nlohmann::json{{"version", 2}, {"current", current}, {"tabs", tabs}}.dump(4));
}

void MainWindow::restoreTab(const nlohmann::json& j) {
    StringList tags, sites;
    if(j.contains("tags") && j["tags"].is_array())
        for(auto& t : j["tags"])
            if(t.is_string()) tags.push_back(t.get<std::string>());
    if(j.contains("sites") && j["sites"].is_array())
        for(auto& t : j["sites"])
            if(t.is_string()) sites.push_back(t.get<std::string>());
    ImagePtr gallery;
    if(j.value("type", "tag") == "gallery" && j.contains("gallery")) {
        gallery = std::make_shared<Image>(app().profile);
        if(!gallery->read(j["gallery"])) return;
    }
    SearchTab* tab = addSearchTab(tags, sites, false, gallery, false);
    tab->fromJson(j);
    if(!tags.empty() || gallery) tab->search(tab->page());
}

void MainWindow::restoreSession() {
    Settings& s = app().profile->settings();
    bool restore = s.get("start", "restore") == "restore";
    auto data = readFile(buildPath(paths::dataDir(), "tabs.json"));
    int current = 0;
    if(restore && data) {
        try {
            auto j = nlohmann::json::parse(*data);
            if(j.contains("tabs") && j["tabs"].is_array())
                for(auto& t : j["tabs"]) restoreTab(t);
            current = j.value("current", 0);
        } catch(...) {
            log("Could not restore the previous tabs", LogLevel::Warning);
        }
    }
    if(m_tabs.empty()) addSearchTab({}, {}, true, nullptr, false);
    if(current >= 0 && current < (int)m_tabs.size()) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(m_notebook), gtk_notebook_page_num(GTK_NOTEBOOK(m_notebook), m_tabs[(size_t)current]->widget()));
        m_tabs[(size_t)current]->focusSearch();
    }
}

}  // namespace bg::gui
