#include "gui/searchtab.hpp"

#include <algorithm>

#include "core/logger.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"
#include "gui/dialogs.hpp"
#include "gui/mainwindow.hpp"
#include "gui/sitesdialog.hpp"
#include "gui/thumbnails.hpp"

namespace bg::gui {

// ---- tag completion (completes the word under the cursor) ------------------------------

static std::string lastWord(const std::string& text) {
    size_t sp = text.find_last_of(' ');
    std::string w = sp == std::string::npos ? text : text.substr(sp + 1);
    while(!w.empty() && (w[0] == '-' || w[0] == '~')) w = w.substr(1);
    return w;
}

static gboolean completionMatch(GtkEntryCompletion* completion, const gchar* key, GtkTreeIter* iter, gpointer) {
    std::string word = lastWord(key);
    if(word.size() < 2) return FALSE;
    GtkTreeModel* model = gtk_entry_completion_get_model(completion);
    gchar* value = nullptr;
    gtk_tree_model_get(model, iter, 0, &value, -1);
    bool ok = value && g_str_has_prefix(value, toLower(word).c_str());
    g_free(value);
    return ok;
}

static void attachCompletion(GtkWidget* entryWidget) {
    static GtkListStore* store = nullptr;
    if(!store) {
        store = gtk_list_store_new(1, G_TYPE_STRING);
        GtkTreeIter it;
        for(auto& w : app().profile->autoComplete()) {
            gtk_list_store_append(store, &it);
            gtk_list_store_set(store, &it, 0, w.c_str(), -1);
        }
    }
    GtkEntryCompletion* c = gtk_entry_completion_new();
    gtk_entry_completion_set_model(c, GTK_TREE_MODEL(store));
    gtk_entry_completion_set_text_column(c, 0);
    gtk_entry_completion_set_match_func(c, completionMatch, nullptr, nullptr);
    gtk_entry_completion_set_minimum_key_length(c, 2);
    gtk_entry_completion_set_popup_set_width(c, FALSE);
    on<gboolean, GtkEntryCompletion*, GtkTreeModel*, GtkTreeIter*>(
        c, "match-selected", [entryWidget](GtkEntryCompletion*, GtkTreeModel* model, GtkTreeIter* iter) -> gboolean {
            gchar* value = nullptr;
            gtk_tree_model_get(model, iter, 0, &value, -1);
            std::string text = entryText(entryWidget);
            size_t sp = text.find_last_of(' ');
            std::string prefix = sp == std::string::npos ? "" : text.substr(0, sp + 1);
            std::string word = sp == std::string::npos ? text : text.substr(sp + 1);
            std::string modifier;
            while(!word.empty() && (word[0] == '-' || word[0] == '~')) {
                modifier += word[0];
                word = word.substr(1);
            }
            std::string result = prefix + modifier + (value ? value : "") + " ";
            g_free(value);
            gtk_entry_set_text(GTK_ENTRY(entryWidget), result.c_str());
            gtk_editable_set_position(GTK_EDITABLE(entryWidget), -1);
            return TRUE;
        });
    gtk_entry_set_completion(GTK_ENTRY(entryWidget), c);
    g_object_unref(c);
}

// ---- construction -------------------------------------------------------------------

SearchTab::SearchTab(MainWindow* window, const StringList& tags, const StringList& sites, const ImagePtr& gallery)
    : m_window(window), m_sites(sites), m_gallery(gallery) {
    if(m_sites.empty()) m_sites = app().profile->settings().getList("sites", {"safebooru.org"});
    build();
    if(!tags.empty()) gtk_entry_set_text(GTK_ENTRY(m_entry), (join(tags, " ") + " ").c_str());
    setSites(m_sites);
}

SearchTab::~SearchTab() {
    *m_alive = false;
    for(auto& [_, p] : m_pages) p->abort();
}

void SearchTab::build() {
    Profile* profile = app().profile;
    m_root = vbox(0);
    addClass(m_root, "bg");

    // Query bar
    GtkWidget* bar = hbox(6);
    addClass(bar, "bg-toolbar");
    m_entry = entry("", m_gallery ? "Gallery" : "Tags, e.g. cat_ears rating:general -comic");
    if(m_gallery) {
        gtk_entry_set_text(GTK_ENTRY(m_entry), ("Gallery: " + m_gallery->name()).c_str());
        gtk_widget_set_sensitive(m_entry, FALSE);
    } else {
        attachCompletion(m_entry);
    }
    onActivate(m_entry, [this] { search(1); });
    gtk_box_pack_start(GTK_BOX(bar), m_entry, TRUE, TRUE, 0);
    m_sitesButton = button("Sites", "network-server-symbolic", "Choose the sites to search");
    onClicked(m_sitesButton, [this] { chooseSites(); });
    gtk_box_pack_start(GTK_BOX(bar), m_sitesButton, FALSE, FALSE, 0);
    m_postFilterEntry = entry("", "Post-filter");
    gtk_widget_set_tooltip_text(m_postFilterEntry,
                                "Filters applied to the results after loading, e.g. \"width:>=1920 -rating:explicit score:>10\"");
    gtk_entry_set_width_chars(GTK_ENTRY(m_postFilterEntry), 18);
    onActivate(m_postFilterEntry, [this] { search(1); });
    gtk_box_pack_start(GTK_BOX(bar), m_postFilterEntry, FALSE, FALSE, 0);
    GtkWidget* go = button("_Search", "system-search-symbolic");
    onClicked(go, [this] { search(1); });
    gtk_box_pack_start(GTK_BOX(bar), go, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), bar, FALSE, FALSE, 0);

    // Results
    m_resultsBox = vbox(0);
    addClass(m_resultsBox, "bg-results");
    m_scroll = scrolled(m_resultsBox);
    addClass(m_scroll, "bg-results");
    gtk_widget_set_vexpand(m_scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(m_root), m_scroll, TRUE, TRUE, 0);

    // Bottom bar: pagination and actions
    GtkWidget* bottom = hbox(4);
    addClass(bottom, "bg-bottombar");
    m_firstBtn = iconButton("go-first-symbolic", "First page");
    m_prevBtn = iconButton("go-previous-symbolic", "Previous page");
    m_pageSpin = spin(1, 100000, 1);
    gtk_widget_set_tooltip_text(m_pageSpin, "Page");
    gtk_entry_set_width_chars(GTK_ENTRY(m_pageSpin), 5);
    m_nextBtn = iconButton("go-next-symbolic", "Next page");
    m_lastBtn = iconButton("go-last-symbolic", "Last page");
    onClicked(m_firstBtn, [this] { navigate(1); });
    onClicked(m_prevBtn, [this] { navigate(m_page - 1); });
    onClicked(m_nextBtn, [this] { navigate(m_page + 1); });
    onClicked(m_lastBtn, [this] {
        int last = maxPage();
        if(last > 0) navigate(last);
    });
    onActivate(m_pageSpin, [this] { navigate(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(m_pageSpin))); });
    for(GtkWidget* w : {m_firstBtn, m_prevBtn, m_pageSpin, m_nextBtn, m_lastBtn}) gtk_box_pack_start(GTK_BOX(bottom), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    m_perPageSpin = spin(1, 1000, (double)profile->settings().getInt("limit", 20));
    gtk_widget_set_tooltip_text(m_perPageSpin, "Images per page");
    gtk_entry_set_width_chars(GTK_ENTRY(m_perPageSpin), 4);
    gtk_box_pack_start(GTK_BOX(bottom), m_perPageSpin, FALSE, FALSE, 0);
    m_mergeCheck = gtk_check_button_new_with_label("Merge");
    gtk_widget_set_tooltip_text(m_mergeCheck, "Show the results of all selected sites in one grid, without duplicates");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_mergeCheck), profile->settings().getBool("mergeresults", false));
    onToggled(m_mergeCheck, [this] { rebuildGrid(); });
    gtk_box_pack_start(GTK_BOX(bottom), m_mergeCheck, FALSE, FALSE, 4);
    m_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(bottom), m_spinner, FALSE, FALSE, 4);
    m_status = label("");
    gtk_label_set_ellipsize(GTK_LABEL(m_status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(bottom), m_status, TRUE, TRUE, 4);

    GtkWidget* getSel = iconButton("object-select-symbolic", "Get selected: add the selected images (Ctrl+click) to the downloads");
    onClicked(getSel, [this] {
        for(auto& img : selectedImages()) m_window->addImageToDownloads(img);
        m_selected.clear();
        refreshStates();
    });
    GtkWidget* getPage = button("Get page", "document-save-symbolic", "Add the images of this page to the downloads");
    onClicked(getPage, [this] { m_window->addSearchToDownloads(this, false); });
    GtkWidget* getAll = button("Get all", "document-save-as-symbolic", "Add every result of this search to the downloads");
    onClicked(getAll, [this] { m_window->addSearchToDownloads(this, true); });
    for(GtkWidget* w : {getSel, getPage, getAll}) gtk_box_pack_end(GTK_BOX(bottom), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), bottom, FALSE, FALSE, 0);

    // Tab label
    m_tabLabel = hbox(4);
    m_titleLabel = label(m_gallery ? "Gallery" : "Search");
    gtk_label_set_ellipsize(GTK_LABEL(m_titleLabel), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(m_titleLabel), 24);
    gtk_label_set_width_chars(GTK_LABEL(m_titleLabel), 6);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close tab");
    onClicked(close, [this] {
        if(onClose) onClose();
    });
    // Middle click on the tab closes it too
    GtkWidget* evbox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(evbox), m_titleLabel);
    onButtonPress(evbox, [this](GdkEventButton* e) {
        if(e->button == 2 && onClose) {
            onClose();
            return true;
        }
        return false;
    });
    gtk_box_pack_start(GTK_BOX(m_tabLabel), evbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(m_tabLabel), close, FALSE, FALSE, 0);
    gtk_widget_show_all(m_tabLabel);
    gtk_widget_show_all(m_root);
    gtk_widget_hide(m_spinner);
    updatePagination();
}

std::string SearchTab::title() const {
    if(m_gallery) return "Gallery: " + m_gallery->name();
    std::string q = trim(query());
    return q.empty() ? "Search" : q;
}

std::string SearchTab::query() const { return entryText(m_entry); }
void SearchTab::setQuery(const std::string& tags) { gtk_entry_set_text(GTK_ENTRY(m_entry), (trim(tags) + " ").c_str()); }
void SearchTab::focusSearch() {
    gtk_widget_grab_focus(m_entry);
    gtk_editable_set_position(GTK_EDITABLE(m_entry), -1);
}

int SearchTab::perPage() const { return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(m_perPageSpin)); }
StringList SearchTab::postFilters() const { return split(entryText(m_postFilterEntry), ' ', true); }

void SearchTab::setSites(const StringList& sites) {
    m_sites = sites;
    std::string text = sites.size() == 1 ? sites[0] : std::to_string(sites.size()) + " sites";
    gtk_button_set_label(GTK_BUTTON(m_sitesButton), text.c_str());
    gtk_widget_set_tooltip_text(m_sitesButton, join(sites, "\n").c_str());
}

void SearchTab::chooseSites() {
    StringList chosen = m_sites;
    if(!chooseSitesDialog(toplevel(m_root), chosen)) return;
    if(chosen.empty()) return;
    setSites(chosen);
    app().profile->settings().setList("sites", chosen);
    search(1);
}

// ---- searching -----------------------------------------------------------------------

void SearchTab::clearResults() {
    for(auto& [_, p] : m_pages) p->abort();
    m_pages.clear();
    m_errors.clear();
    m_tags.clear();
    m_shown.clear();
    m_selected.clear();
    m_thumbHandles.clear();
    m_thumbWidgets.clear();
    GList* children = gtk_container_get_children(GTK_CONTAINER(m_resultsBox));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

void SearchTab::navigate(int page) {
    if(page < 1) return;
    int last = maxPage();
    if(last > 0 && page > last) page = last;
    // Remember the current page boundaries so sources paging by ID can continue from them
    m_lastInfo.clear();
    for(auto& [url, p] : m_pages)
        if(p->isLoaded()) m_lastInfo[url] = p->pageInformation();
    search(page);
}

void SearchTab::search(int page) {
    Profile* profile = app().profile;
    bool continuing = !m_lastInfo.empty();
    std::map<std::string, PageInformation> lastInfo = continuing ? m_lastInfo : std::map<std::string, PageInformation>();
    m_lastInfo.clear();
    clearResults();
    m_page = std::max(1, page);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(m_pageSpin), m_page);
    gtk_label_set_text(GTK_LABEL(m_titleLabel), title().c_str());
    gtk_widget_set_tooltip_text(m_titleLabel, title().c_str());

    SearchQuery q;
    if(m_gallery)
        q.gallery = m_gallery;
    else
        q.tags = split(query(), ' ', true);
    auto sites = profile->filteredSites(m_sites);
    if(sites.empty()) {
        gtk_label_set_text(GTK_LABEL(m_status), "No valid site selected");
        return;
    }
    if(!m_gallery && m_page == 1) profile->addHistory(q, m_sites);
    std::vector<Site*> raw;
    for(auto& s : sites) raw.push_back(s.get());
    m_pending = (int)sites.size();
    gtk_widget_show(m_spinner);
    gtk_spinner_start(GTK_SPINNER(m_spinner));
    gtk_label_set_text(GTK_LABEL(m_status), "Loading…");
    for(auto& site : sites) {
        PageInformation last;
        auto it = lastInfo.find(site->url());
        if(it != lastInfo.end()) last = it->second;
        auto page = std::make_shared<Page>(profile, site, raw, q, m_page, perPage(), postFilters(), true, last);
        std::string url = site->url();
        m_pages[url] = page;
        std::weak_ptr<bool> alive = m_alive;
        page->load([this, alive, url](Page* p, bool ok) {
            if(!alive.lock()) return;
            pageLoaded(url, p, ok);
        });
    }
    m_window->tabChanged(this);
}

void SearchTab::pageLoaded(const std::string& siteUrl, Page* page, bool ok) {
    m_pending--;
    if(!ok) m_errors[siteUrl] = page->errors().empty() ? "Error" : page->errors().back();
    // Merge tags of all sites (keeping the highest count)
    std::map<std::string, size_t> index;
    for(size_t i = 0; i < m_tags.size(); i++) index[m_tags[i].text()] = i;
    for(auto& t : page->tags()) {
        auto it = index.find(t.text());
        if(it == index.end()) {
            index[t.text()] = m_tags.size();
            m_tags.push_back(t);
        } else if(t.count() > m_tags[it->second].count()) {
            m_tags[it->second].setCount(t.count());
        }
    }
    if(!page->tags().empty()) app().profile->addAutoComplete(page->tags());
    rebuildGrid();
    updatePagination();
    if(m_pending <= 0) {
        gtk_spinner_stop(GTK_SPINNER(m_spinner));
        gtk_widget_hide(m_spinner);
    }
    m_window->tabChanged(this);
}

int SearchTab::maxPage() const {
    int max = -1;
    for(auto& [_, p] : m_pages) {
        int n = p->pagesCount();
        if(n <= 0) n = p->maxPagesCount();
        max = std::max(max, n);
    }
    return max;
}

void SearchTab::updatePagination() {
    int last = maxPage();
    bool hasNext = false;
    for(auto& [_, p] : m_pages) hasNext = hasNext || p->hasNext();
    gtk_widget_set_sensitive(m_firstBtn, m_page > 1);
    gtk_widget_set_sensitive(m_prevBtn, m_page > 1);
    gtk_widget_set_sensitive(m_nextBtn, hasNext || m_pages.empty());
    gtk_widget_set_sensitive(m_lastBtn, last > m_page);
    long long total = 0;
    bool known = false;
    for(auto& [_, p] : m_pages) {
        int c = p->imagesCount();
        if(c > 0) {
            total += c;
            known = true;
        }
    }
    std::string status = std::to_string(m_shown.size()) + " shown";
    if(known) status += " · " + std::to_string(total) + " results";
    status += " · page " + std::to_string(m_page) + (last > 0 ? " of " + std::to_string(last) : "");
    if(!m_errors.empty()) status += " · " + std::to_string(m_errors.size()) + " site(s) failed";
    if(m_pending > 0) status += " · loading…";
    gtk_label_set_text(GTK_LABEL(m_status), status.c_str());
    gtk_widget_set_tooltip_text(m_status, status.c_str());
}

std::vector<ImagePtr> SearchTab::images() const { return m_shown; }

std::vector<ImagePtr> SearchTab::selectedImages() const {
    std::vector<ImagePtr> out;
    for(auto& img : m_shown)
        if(m_selected.count(img.get())) out.push_back(img);
    return out;
}

std::string SearchTab::wiki() const {
    for(auto& [_, p] : m_pages)
        if(!p->wiki().empty()) return p->wiki();
    return "";
}

// ---- results grid ---------------------------------------------------------------------------

static GtkWidget* newFlowBox() {
    GtkWidget* fb = gtk_flow_box_new();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), TRUE);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), 64);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 4);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(fb), FALSE);
    gtk_widget_set_valign(fb, GTK_ALIGN_START);
    return fb;
}

void SearchTab::rebuildGrid() {
    Profile* profile = app().profile;
    bool hideBlacklisted = profile->settings().getBool("hideblacklisted", false);
    bool merge = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_mergeCheck));
    GList* children = gtk_container_get_children(GTK_CONTAINER(m_resultsBox));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    m_thumbHandles.clear();
    m_thumbWidgets.clear();
    m_shown.clear();

    auto visible = [&](const ImagePtr& img) { return !hideBlacklisted || img->state() != "blacklisted"; };
    GtkWidget* mergedBox = nullptr;
    std::set<std::string> seenMd5;
    for(auto& url : m_sites) {
        auto pit = m_pages.find(url);
        if(pit == m_pages.end()) continue;
        Page* page = pit->second.get();
        auto errIt = m_errors.find(url);
        if(!merge || m_sites.size() == 1) {
            std::string header = page->site()->name();
            if(errIt != m_errors.end())
                header += " — " + errIt->second;
            else if(!page->isLoaded())
                header += " — loading…";
            else {
                int count = page->imagesCount();
                header += " — " + std::to_string(page->images().size()) + " images" + (count > 0 ? " of " + std::to_string(count) : "");
                if(page->filteredImageCount() > 0) header += ", " + std::to_string(page->filteredImageCount()) + " filtered";
            }
            if(m_sites.size() > 1 || errIt != m_errors.end()) {
                GtkWidget* h = label(header);
                addClass(h, "bg-site-header");
                if(errIt != m_errors.end()) addClass(h, "error");
                gtk_box_pack_start(GTK_BOX(m_resultsBox), h, FALSE, FALSE, 0);
            }
        }
        GtkWidget* fb = nullptr;
        if(merge) {
            if(!mergedBox) {
                mergedBox = newFlowBox();
                gtk_box_pack_start(GTK_BOX(m_resultsBox), mergedBox, FALSE, FALSE, 0);
            }
            fb = mergedBox;
        } else {
            fb = newFlowBox();
            gtk_box_pack_start(GTK_BOX(m_resultsBox), fb, FALSE, FALSE, 0);
        }
        for(auto& img : page->images()) {
            if(!visible(img)) continue;
            if(merge && !img->md5().empty() && !seenMd5.insert(img->md5()).second) continue;
            size_t index = m_shown.size();
            m_shown.push_back(img);
            gtk_container_add(GTK_CONTAINER(fb), makeThumbnail(img, index));
        }
    }
    if(m_shown.empty() && m_pending <= 0) {
        GtkWidget* l = label(m_errors.empty() ? "No results" : "No results — see the Log tab for details", 0.5f);
        addClass(l, "bg-placeholder");
        gtk_widget_set_margin_top(l, 40);
        gtk_box_pack_start(GTK_BOX(m_resultsBox), l, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(m_resultsBox);
}

GtkWidget* SearchTab::makeThumbnail(const ImagePtr& img, size_t index) {
    int size = (int)app().profile->settings().getInt("Interface/thumbnailSize", 150);
    GtkWidget* ev = gtk_event_box_new();
    GtkWidget* frame = vbox(0);
    addClass(frame, "bg-thumb");
    std::string state = img->state();
    if(!state.empty()) addClass(frame, state.c_str());
    if(m_selected.count(img.get())) addClass(frame, "selected");
    GtkWidget* overlay = gtk_overlay_new();
    GtkWidget* image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(image, size, size);
    gtk_container_add(GTK_CONTAINER(overlay), image);
    std::string badge;
    if(img->isGallery())
        badge = img->galleryCount() > 0 ? std::to_string(img->galleryCount()) + " ▤" : "▤";
    else if(img->isVideo())
        badge = "▶";
    else if(!img->isAnimated().empty())
        badge = "GIF";
    if(!badge.empty()) {
        GtkWidget* b = label(badge);
        addClass(b, "bg-badge");
        gtk_widget_set_halign(b, GTK_ALIGN_END);
        gtk_widget_set_valign(b, GTK_ALIGN_END);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), b);
    }
    gtk_box_pack_start(GTK_BOX(frame), overlay, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ev), frame);
    gtk_widget_set_has_tooltip(ev, TRUE);
    std::weak_ptr<Image> weakImg = img;
    on<gboolean, GtkWidget*, gint, gint, gboolean, GtkTooltip*>(ev, "query-tooltip", [weakImg](GtkWidget*, gint, gint, gboolean, GtkTooltip* tip) -> gboolean {
        auto i = weakImg.lock();
        if(!i) return FALSE;
        gtk_tooltip_set_markup(tip, i->tooltip().c_str());
        return TRUE;
    });
    onButtonPress(ev, [this, img, index, frame](GdkEventButton* e) {
        if(e->type != GDK_BUTTON_PRESS) return false;
        if(e->button == 1 && (e->state & GDK_CONTROL_MASK)) {
            if(m_selected.count(img.get())) {
                m_selected.erase(img.get());
                removeClass(frame, "selected");
            } else {
                m_selected.insert(img.get());
                addClass(frame, "selected");
            }
            return true;
        }
        if(e->button == 1) {
            if(img->isGallery())
                m_window->addSearchTab({}, {img->site()->url()}, true, img);
            else
                m_window->openViewer(m_shown, index, this);
            return true;
        }
        if(e->button == 2) {
            m_window->addImageToDownloads(img);
            return true;
        }
        if(e->button == 3) {
            imageMenu(img, (GdkEvent*)e);
            return true;
        }
        return false;
    });
    m_thumbWidgets[img.get()] = frame;
    std::weak_ptr<bool> alive = m_alive;
    m_thumbHandles.push_back(app().thumbnails->request(img, size, [image, alive](GdkPixbuf* pb) {
        if(!alive.lock()) return;
        if(pb)
            gtk_image_set_from_pixbuf(GTK_IMAGE(image), pb);
        else
            gtk_image_set_from_icon_name(GTK_IMAGE(image), "image-missing-symbolic", GTK_ICON_SIZE_DIALOG);
    }));
    return ev;
}

void SearchTab::refreshStates() {
    for(auto& [img, frame] : m_thumbWidgets) {
        for(const char* cls : {"blacklisted", "favorited", "parent", "children", "pending", "selected"}) removeClass(frame, cls);
        std::string state = img->state();
        if(!state.empty()) addClass(frame, state.c_str());
        if(m_selected.count(img)) addClass(frame, "selected");
    }
}

void SearchTab::imageMenu(const ImagePtr& img, GdkEvent* e) {
    GtkWidget* menu = gtk_menu_new();
    std::vector<ImagePtr> shown = m_shown;
    size_t index = std::find(shown.begin(), shown.end(), img) - shown.begin();
    menuItem(menu, "_Open", [this, shown, index] { m_window->openViewer(shown, index, this); });
    if(img->isGallery()) menuItem(menu, "Open gallery in a new tab", [this, img] { m_window->addSearchTab({}, {img->site()->url()}, true, img); });
    menuItem(menu, "Open in _browser", [img] { openUri(img->pageUrl().empty() ? img->url() : img->pageUrl()); });
    if(!img->sources().empty()) menuItem(menu, "Open _source", [img] { openUri(img->sources()[0]); });
    menuSeparator(menu);
    menuItem(menu, "_Save", [img] { quickSave(img, false); });
    menuItem(menu, "Add to _downloads", [this, img] { m_window->addImageToDownloads(img); });
    menuItem(menu, "Add selected to downloads", [this] {
        for(auto& i : selectedImages()) m_window->addImageToDownloads(i);
    });
    menuSeparator(menu);
    menuItem(menu, "Copy file _URL", [img] { copyToClipboard(img->url()); });
    menuItem(menu, "Copy _page URL", [img] { copyToClipboard(img->pageUrl()); });
    menuItem(menu, "Copy tags", [img] { copyToClipboard(join(img->tagNames(), " ")); });
    GtkWidget* reverse = gtk_menu_item_new_with_mnemonic("_Reverse image search");
    GtkWidget* sub = gtk_menu_new();
    for(auto& [name, url] : webServices())
        menuItem(sub, name, [img, url = url] { openUri(replaceAll(url, "{url}", urlEncode(img->url(SizeType::Sample).empty() ? img->url() : img->url(SizeType::Sample)))); });
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(reverse), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), reverse);
    popupMenu(menu, e);
}

// ---- session ------------------------------------------------------------------------

nlohmann::json SearchTab::toJson() const {
    nlohmann::json j;
    j["type"] = m_gallery ? "gallery" : "tag";
    j["tags"] = split(query(), ' ', true);
    j["sites"] = m_sites;
    j["page"] = m_page;
    j["perpage"] = perPage();
    j["postFiltering"] = postFilters();
    j["mergeResults"] = (bool)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_mergeCheck));
    if(m_gallery) j["gallery"] = m_gallery->write();
    return j;
}

void SearchTab::fromJson(const nlohmann::json& j) {
    if(j.contains("tags") && j["tags"].is_array() && !m_gallery) {
        StringList tags;
        for(auto& t : j["tags"])
            if(t.is_string()) tags.push_back(t.get<std::string>());
        if(!tags.empty()) setQuery(join(tags, " "));
    }
    if(j.contains("perpage") && j["perpage"].is_number()) gtk_spin_button_set_value(GTK_SPIN_BUTTON(m_perPageSpin), j["perpage"].get<int>());
    if(j.contains("postFiltering") && j["postFiltering"].is_array()) {
        StringList pf;
        for(auto& t : j["postFiltering"])
            if(t.is_string()) pf.push_back(t.get<std::string>());
        gtk_entry_set_text(GTK_ENTRY(m_postFilterEntry), join(pf, " ").c_str());
    }
    if(j.contains("mergeResults") && j["mergeResults"].is_boolean())
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m_mergeCheck), j["mergeResults"].get<bool>());
    if(j.contains("page") && j["page"].is_number()) m_page = std::max(1, j["page"].get<int>());
    gtk_label_set_text(GTK_LABEL(m_titleLabel), title().c_str());
}

}  // namespace bg::gui
