#include "gui/sitesdialog.hpp"

#include "core/download.hpp"
#include "core/profile.hpp"

namespace bg::gui {

enum { S_CHECK, S_NAME, S_URL, S_IS_SITE, S_ICON, S_COLS };

static void fillSites(GtkTreeStore* store, const StringList& checked, const std::string& filter) {
    gtk_tree_store_clear(store);
    Profile* p = app().profile;
    for(auto& [name, src] : p->sources()) {
        std::vector<std::shared_ptr<Site>> sites;
        for(auto& [url, site] : p->sites())
            if(site->source() == src.get() && (filter.empty() || contains(toLower(url), filter) || contains(toLower(name), filter)))
                sites.push_back(site);
        if(sites.empty()) continue;
        GdkPixbuf* icon = gdk_pixbuf_new_from_file_at_size(src->iconPath().c_str(), 16, 16, nullptr);
        GtkTreeIter parent;
        gtk_tree_store_append(store, &parent, nullptr);
        gtk_tree_store_set(store, &parent, S_CHECK, FALSE, S_NAME, name.c_str(), S_URL, "", S_IS_SITE, FALSE, S_ICON, icon, -1);
        bool all = true;
        for(auto& s : sites) {
            GtkTreeIter it;
            bool on = contains(checked, s->url());
            all = all && on;
            gtk_tree_store_append(store, &it, &parent);
            std::string label = s->name() == s->url() ? s->url() : s->name() + " (" + s->url() + ")";
            gtk_tree_store_set(store, &it, S_CHECK, on, S_NAME, label.c_str(), S_URL, s->url().c_str(), S_IS_SITE, TRUE, S_ICON, icon, -1);
        }
        gtk_tree_store_set(store, &parent, S_CHECK, all, -1);
        if(icon) g_object_unref(icon);
    }
}

static StringList checkedSites(GtkTreeStore* store) {
    StringList out;
    GtkTreeIter parent;
    GtkTreeModel* m = GTK_TREE_MODEL(store);
    if(!gtk_tree_model_get_iter_first(m, &parent)) return out;
    do {
        GtkTreeIter child;
        if(!gtk_tree_model_iter_children(m, &child, &parent)) continue;
        do {
            gboolean on = FALSE;
            gchar* url = nullptr;
            gtk_tree_model_get(m, &child, S_CHECK, &on, S_URL, &url, -1);
            if(on && url) out.push_back(url);
            g_free(url);
        } while(gtk_tree_model_iter_next(m, &child));
    } while(gtk_tree_model_iter_next(m, &parent));
    return out;
}

static std::string selectedUrl(GtkWidget* view) {
    GtkTreeIter it;
    GtkTreeModel* m;
    if(!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &m, &it)) return "";
    gchar* url = nullptr;
    gtk_tree_model_get(m, &it, S_URL, &url, -1);
    std::string r = url ? url : "";
    g_free(url);
    return r;
}

bool chooseSitesDialog(GtkWindow* parent, StringList& sites) {
    GtkWidget* d = dialog(parent, "Sites", 520, 600);
    auto checked = std::make_shared<StringList>(sites);
    GtkTreeStore* store = gtk_tree_store_new(S_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF);
    GtkWidget* filter = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(filter), "Filter sites");
    GtkWidget* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
    GtkCellRenderer* toggle = gtk_cell_renderer_toggle_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), gtk_tree_view_column_new_with_attributes("", toggle, "active", S_CHECK, nullptr));
    GtkTreeViewColumn* nameCol = gtk_tree_view_column_new();
    GtkCellRenderer* iconR = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer* textR = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(nameCol, iconR, FALSE);
    gtk_tree_view_column_pack_start(nameCol, textR, TRUE);
    gtk_tree_view_column_add_attribute(nameCol, iconR, "pixbuf", S_ICON);
    gtk_tree_view_column_add_attribute(nameCol, textR, "text", S_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), nameCol);
    on<void, GtkCellRendererToggle*, gchar*>(toggle, "toggled", [store, checked](GtkCellRendererToggle*, gchar* pathStr) {
        GtkTreeModel* m = GTK_TREE_MODEL(store);
        GtkTreeIter it;
        if(!gtk_tree_model_get_iter_from_string(m, &it, pathStr)) return;
        gboolean on = FALSE, isSite = FALSE;
        gtk_tree_model_get(m, &it, S_CHECK, &on, S_IS_SITE, &isSite, -1);
        on = !on;
        gtk_tree_store_set(store, &it, S_CHECK, on, -1);
        auto apply = [&](GtkTreeIter* row) {
            gchar* url = nullptr;
            gtk_tree_model_get(m, row, S_URL, &url, -1);
            if(url && *url) {
                removeAll(*checked, url);
                if(on) checked->push_back(url);
            }
            g_free(url);
        };
        if(isSite) {
            apply(&it);
        } else {
            GtkTreeIter child;
            if(gtk_tree_model_iter_children(m, &child, &it)) do {
                    gtk_tree_store_set(store, &child, S_CHECK, on, -1);
                    apply(&child);
                } while(gtk_tree_model_iter_next(m, &child));
        }
    });
    auto refill = [store, checked, filter, view] {
        fillSites(store, *checked, toLower(entryText(filter)));
        gtk_tree_view_expand_all(GTK_TREE_VIEW(view));
    };
    onChanged(filter, refill);
    refill();
    GtkWidget* sw = scrolled(view);
    gtk_widget_set_vexpand(sw, TRUE);

    GtkWidget* buttons = hbox(4);
    GtkWidget* add = button("_Add site…", "list-add-symbolic");
    onClicked(add, [d, refill] {
        if(addSiteDialog(GTK_WINDOW(d))) refill();
    });
    GtkWidget* settings = button("Site _settings…", "emblem-system-symbolic");
    onClicked(settings, [d, view] {
        std::string url = selectedUrl(view);
        if(auto s = app().profile->site(url)) siteSettingsDialog(GTK_WINDOW(d), s.get());
    });
    GtkWidget* remove = button("_Remove site", "list-remove-symbolic");
    onClicked(remove, [d, view, checked, refill] {
        std::string url = selectedUrl(view);
        if(url.empty() || !confirm(GTK_WINDOW(d), "Remove " + url + "?", "The site is removed from its source list. Its settings are kept.")) return;
        app().profile->removeSite(url);
        removeAll(*checked, url);
        refill();
    });
    GtkWidget* none = button("Select none");
    onClicked(none, [checked, refill] {
        checked->clear();
        refill();
    });
    for(GtkWidget* w : {add, settings, remove}) gtk_box_pack_start(GTK_BOX(buttons), w, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), none, FALSE, FALSE, 0);
    for(GtkWidget* w : {filter, sw, buttons}) gtk_box_pack_start(GTK_BOX(dialogContent(d)), w, w == sw, w == sw, 0);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
    if(ok) {
        // Keep the user's order for already selected sites, then add new ones
        StringList now = checkedSites(store), out;
        for(auto& s : *checked)
            if(app().profile->site(s)) out.push_back(s);
        for(auto& s : now)
            if(!contains(out, s)) out.push_back(s);
        sites = out;
    }
    gtk_widget_destroy(d);
    g_object_unref(store);
    return ok;
}

// ---- site settings ----------------------------------------------------------------------

static GtkWidget* textArea(const std::string& text) {
    GtkWidget* tv = gtk_text_view_new();
    addClass(tv, "bg-mono");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text.c_str(), -1);
    return tv;
}

static std::string textOf(GtkWidget* tv) {
    GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gchar* t = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    std::string r = t;
    g_free(t);
    return r;
}

void siteSettingsDialog(GtkWindow* parent, Site* site) {
    Settings& s = site->settings();
    GtkWidget* d = dialog(parent, "Settings of " + site->url(), 620, 560);
    GtkWidget* nb = gtk_notebook_new();

    // General
    GtkWidget* general = grid();
    gtk_container_set_border_width(GTK_CONTAINER(general), 10);
    int r = 0;
    GtkWidget* name = entry(s.get("name", site->url()));
    gridRow(general, r++, "Name", name);
    GtkWidget* ssl = gtk_check_button_new_with_label("Use HTTPS");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ssl), s.getBool("ssl", false));
    gtk_grid_attach(GTK_GRID(general), ssl, 1, r++, 1, 1);
    StringList refIds{"none", "host", "page", "image", "details"}, refLabels{"None", "Site root", "Search page", "Image itself", "Image page"};
    GtkWidget* referer = combo(refIds, refLabels, s.get("referer", "none"));
    gridRow(general, r++, "Referer for pages", referer);
    GtkWidget* refPreview = combo(refIds, refLabels, s.get("referer_preview", "page"));
    gridRow(general, r++, "Referer for thumbnails", refPreview);
    GtkWidget* refImage = combo(refIds, refLabels, s.get("referer_image", "details"));
    gridRow(general, r++, "Referer for images", refImage);
    GtkWidget* added = entry(s.get("added_tags"), "Tags added to every search");
    gridRow(general, r++, "Added tags", added);
    GtkWidget* removed = entry(s.get("removed_tags"), "Tags removed from every search");
    gridRow(general, r++, "Removed tags", removed);
    GtkWidget* ignoreFirst = spin(0, 100, (double)s.getInt("ignore/1", 0));
    gridRow(general, r++, "Skip images on page 1", ignoreFirst, "Some HTML pages show unrelated images first (HTML API only)");
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), general, label("General"));

    // Login
    GtkWidget* login = vbox(8);
    gtk_container_set_border_width(GTK_CONTAINER(login), 10);
    StringList authIds{"disabled"}, authLabels{"Disabled"};
    for(auto& [id, auth] : site->source()->auths()) {
        authIds.push_back(id);
        authLabels.push_back(id + " (" + auth->type + (auth->authType.empty() ? "" : ", " + auth->authType) + ")");
    }
    GtkWidget* authType = combo(authIds, authLabels, s.get("login/type", authIds.size() > 1 ? authIds[1] : "disabled"));
    GtkWidget* loginGrid = grid();
    gridRow(loginGrid, 0, "Method", authType);
    GtkWidget* fieldsGrid = grid();
    auto fieldEntries = std::make_shared<std::map<std::string, GtkWidget*>>();
    auto buildFields = [fieldsGrid, authType, site, fieldEntries, &s] {
        GList* children = gtk_container_get_children(GTK_CONTAINER(fieldsGrid));
        for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);
        fieldEntries->clear();
        auto it = site->source()->auths().find(comboId(authType));
        if(it == site->source()->auths().end()) return;
        static const std::map<std::string, std::string> names{
            {"pseudo", "Username"},         {"password", "Password"},          {"salt", "Salt"},
            {"apiKey", "API key"},          {"consumerKey", "Client ID"},      {"consumerSecret", "Client secret"},
            {"accessToken", "Access token"}, {"refreshToken", "Refresh token"}, {"userId", "User ID"}};
        int row = 0;
        for(auto& f : it->second->settingFields()) {
            GtkWidget* e = entry(s.get("auth/" + f.id, f.def));
            gtk_entry_set_visibility(GTK_ENTRY(e), !f.isPassword);
            auto n = names.find(f.id);
            gridRow(fieldsGrid, row++, n != names.end() ? n->second : f.id, e);
            (*fieldEntries)[f.id] = e;
        }
        if(it->second->type == "oauth2" && (it->second->authType == "authorization_code" || it->second->authType == "pkce")) {
            GtkWidget* hint = label(it->second->urlProtocol.empty()
                                        ? "Logging in opens the browser; the site redirects back to a local port (127.0.0.1:58923)."
                                        : "Logging in opens the browser; the site redirects back through a " + it->second->urlProtocol +
                                              ":// link handled by Booru Grabber (installed desktop file required).");
            gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
            gtk_grid_attach(GTK_GRID(fieldsGrid), hint, 0, row++, 2, 1);
        }
        gtk_widget_show_all(fieldsGrid);
    };
    onChanged(authType, buildFields);
    buildFields();
    GtkWidget* testRow = hbox(6);
    GtkWidget* test = button("_Test login", "network-transmit-receive-symbolic");
    GtkWidget* testResult = label("");
    GtkWidget* clearCookies = button("Clear cookies", "edit-clear-symbolic");
    onClicked(clearCookies, [site, testResult] {
        site->clearCookies();
        gtk_label_set_text(GTK_LABEL(testResult), "Cookies cleared");
    });
    gtk_box_pack_start(GTK_BOX(testRow), test, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(testRow), clearCookies, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(testRow), testResult, TRUE, TRUE, 0);
    for(GtkWidget* w : {loginGrid, fieldsGrid, testRow}) gtk_box_pack_start(GTK_BOX(login), w, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), login, label("Login"));

    // APIs
    GtkWidget* apis = grid();
    gtk_container_set_border_width(GTK_CONTAINER(apis), 10);
    GtkWidget* useDefault = gtk_check_button_new_with_label("Use the default order of the settings");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(useDefault), s.getBool("sources/usedefault", true));
    gtk_grid_attach(GTK_GRID(apis), useDefault, 0, 0, 2, 1);
    StringList apiIds{"", "xml", "json", "regex", "rss"}, apiLabels{"—", "XML", "JSON", "HTML", "RSS"};
    std::vector<GtkWidget*> order;
    StringList defaults{"xml", "json", "regex", "rss"};
    for(int i = 0; i < 4; i++) {
        GtkWidget* c = combo(apiIds, apiLabels, toLower(s.get("sources/source_" + std::to_string(i + 1), defaults[(size_t)i])));
        gridRow(apis, i + 1, "API " + std::to_string(i + 1), c);
        order.push_back(c);
    }
    StringList available;
    for(auto& a : site->source()->apis()) available.push_back(a->name());
    GtkWidget* availLabel = label("This source provides: " + join(available, ", "));
    gtk_grid_attach(GTK_GRID(apis), availLabel, 0, 5, 2, 1);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), apis, label("APIs"));

    // Network
    GtkWidget* net = grid();
    gtk_container_set_border_width(GTK_CONTAINER(net), 10);
    std::string headers;
    for(auto& k : s.childKeys("Headers"))
        if(k != "User-Agent" || s.containsOwn("Headers/User-Agent")) headers += k + ": " + s.get("Headers/" + k) + "\n";
    GtkWidget* headersView = textArea(headers);
    GtkWidget* hsw = scrolled(headersView, true);
    gtk_widget_set_size_request(hsw, -1, 90);
    gridRow(net, 0, "Headers", hsw, "One \"Name: value\" per line");
    GtkWidget* cookiesView = textArea(join(s.getList("cookies"), "\n"));
    GtkWidget* csw = scrolled(cookiesView, true);
    gtk_widget_set_size_request(csw, -1, 70);
    gridRow(net, 1, "Cookies", csw, "One \"name=value\" per line, sent with every request");
    GtkWidget* simultaneous = spin(1, 50, (double)s.getInt("download/simultaneous", 10));
    gridRow(net, 2, "Simultaneous requests", simultaneous);
    struct Throttle {
        const char* key;
        const char* label;
        double def;
    } throttles[] = {{"download/throttle_page", "Seconds between pages", 1}, {"download/throttle_image", "Seconds between images", 1},
                     {"download/throttle_thumbnail", "Seconds between thumbnails", 0}, {"download/throttle_details", "Seconds between details", 1},
                     {"download/throttle_retry", "Seconds before retrying", 60}};
    std::vector<GtkWidget*> throttleSpins;
    int row = 3;
    for(auto& t : throttles) {
        GtkWidget* sp = spin(0, 3600, s.getDouble(t.key, t.def), 0.5);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(sp), 1);
        gridRow(net, row++, t.label, sp);
        throttleSpins.push_back(sp);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), net, label("Network"));

    auto save = [&] {
        s.setWithDefault("name", entryText(name), site->url());
        s.setBool("ssl", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ssl)));
        s.setWithDefault("referer", comboId(referer), "none");
        s.setWithDefault("referer_preview", comboId(refPreview), "page");
        s.setWithDefault("referer_image", comboId(refImage), "details");
        s.setWithDefault("added_tags", entryText(added), "");
        s.setWithDefault("removed_tags", entryText(removed), "");
        s.setWithDefault("ignore/1", std::to_string(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ignoreFirst))), "0");
        s.set("login/type", comboId(authType));
        for(auto& [id, e] : *fieldEntries) s.set("auth/" + id, entryText(e));
        s.setBool("sources/usedefault", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(useDefault)));
        for(int i = 0; i < 4; i++) s.set("sources/source_" + std::to_string(i + 1), comboId(order[(size_t)i]));
        for(auto& k : s.childKeys("Headers"))
            if(s.containsOwn("Headers/" + k)) s.remove("Headers/" + k);
        for(auto& line : split(textOf(headersView), '\n', true)) {
            size_t colon = line.find(':');
            if(colon == std::string::npos) continue;
            std::string k = trim(line.substr(0, colon)), v = trim(line.substr(colon + 1));
            if(!k.empty()) s.set("Headers/" + k, v);
        }
        StringList cookies;
        for(auto& c : split(textOf(cookiesView), '\n', true))
            if(!trim(c).empty()) cookies.push_back(trim(c));
        if(cookies.empty())
            s.remove("cookies");
        else
            s.setList("cookies", cookies);
        s.setInt("download/simultaneous", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(simultaneous)));
        for(size_t i = 0; i < throttleSpins.size(); i++) s.setDouble(throttles[i].key, gtk_spin_button_get_value(GTK_SPIN_BUTTON(throttleSpins[i])));
        s.sync();
        site->loadConfig();
    };
    onClicked(test, [&, testResult] {
        save();
        gtk_label_set_text(GTK_LABEL(testResult), "Logging in…");
        std::weak_ptr<Site> weak = site->weak_from_this();
        site->login(true, [testResult, weak](LoginResult res) {
            if(!weak.lock()) return;
            const char* text = res == LoginResult::Success ? "Logged in" : res == LoginResult::Impossible ? "This login method can't be tested" : "Login failed (see the Log tab)";
            if(GTK_IS_LABEL(testResult)) gtk_label_set_text(GTK_LABEL(testResult), text);
        });
    });

    gtk_box_pack_start(GTK_BOX(dialogContent(d)), nb, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    if(gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) save();
    gtk_widget_destroy(d);
}

// ---- adding sites --------------------------------------------------------------------------

std::shared_ptr<Site> addSiteDialog(GtkWindow* parent) {
    Profile* p = app().profile;
    GtkWidget* d = dialog(parent, "Add a site", 480);
    StringList ids;
    for(auto& [name, _] : p->sources()) ids.push_back(name);
    GtkWidget* source = combo(ids, ids, ids.empty() ? "" : ids[0]);
    GtkWidget* url = entry("", "e.g. danbooru.donmai.us");
    GtkWidget* ssl = gtk_check_button_new_with_label("Use HTTPS");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ssl), TRUE);
    GtkWidget* guess = button("_Detect source", "system-search-symbolic", "Try every source's check request on this site");
    GtkWidget* status = label("");
    GtkWidget* gr = grid();
    gridRow(gr, 0, "Address", url);
    GtkWidget* srcRow = hbox(4);
    gtk_box_pack_start(GTK_BOX(srcRow), source, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(srcRow), guess, FALSE, FALSE, 0);
    gridRow(gr, 1, "Source", srcRow);
    gtk_grid_attach(GTK_GRID(gr), ssl, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(gr), status, 1, 3, 1, 1);
    auto alive = std::make_shared<bool>(true);
    std::weak_ptr<bool> weakAlive = alive;
    onClicked(guess, [p, url, source, status, guess, weakAlive] {
        std::string u = trim(entryText(url));
        if(u.empty()) return;
        gtk_label_set_text(GTK_LABEL(status), "Detecting…");
        gtk_widget_set_sensitive(guess, FALSE);
        guessSource(p, u, [source, status, guess, weakAlive](Source* src) {
            if(!weakAlive.lock()) return;
            gtk_widget_set_sensitive(guess, TRUE);
            if(src) {
                gtk_combo_box_set_active_id(GTK_COMBO_BOX(source), src->name().c_str());
                gtk_label_set_text(GTK_LABEL(status), ("Detected: " + src->name()).c_str());
            } else {
                gtk_label_set_text(GTK_LABEL(status), "No source recognized this site");
            }
        });
    });
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), gr, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    std::shared_ptr<Site> site;
    if(gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        Source* src = p->source(comboId(source));
        std::string u = trim(entryText(url));
        bool https = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ssl)) || startsWith(u, "https://");
        if(src && !u.empty()) {
            site = p->addSite(src, u);
            if(site) {
                site->settings().setBool("ssl", https);
                site->settings().sync();
                site->loadConfig();
            }
        }
    }
    alive.reset();
    gtk_widget_destroy(d);
    return site;
}

}  // namespace bg::gui
