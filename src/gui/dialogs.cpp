#include "gui/dialogs.hpp"

#include <glib/gstdio.h>

#include <deque>

#include "config.hpp"
#include "core/logger.hpp"
#include "core/monitoring.hpp"
#include "core/paths.hpp"
#include "core/site.hpp"
#include "gui/mainwindow.hpp"
#include "gui/sitesdialog.hpp"

namespace bg::gui {

// ---- shared actions --------------------------------------------------------------------

void quickSave(const ImagePtr& img, bool favorite, std::function<void(const SaveResults&)> done) {
    Profile* p = app().profile;
    Settings& s = p->settings();
    std::string filename = s.get("Save/filename", "%md5%.%ext%");
    std::string path = p->savePath();
    if(favorite) {
        std::string favFilename = s.get("Save/filename_favorites"), favPath = expandHome(s.get("Save/path_favorites"));
        if(!favFilename.empty()) filename = favFilename;
        if(!favPath.empty() && favPath != "/") path = favPath;
    }
    auto dl = std::make_shared<ImageDownloader>(p, img, filename, path, 1, true, true, true);
    dl->save([dl, done](const ImagePtr&, const SaveResults& r) {
        std::string what = r.empty() ? "error" : saveResultName(r[0].result);
        if(app().window) app().window->setStatus("Save: " + what + (r.empty() || r[0].path.empty() ? "" : " — " + r[0].path));
        if(done) done(r);
    });
}

std::vector<std::pair<std::string, std::string>> webServices() {
    std::vector<std::pair<std::string, std::string>> out;
    auto items = app().profile->settings().readArray("WebServices");
    std::sort(items.begin(), items.end(), [](const StringMap& a, const StringMap& b) {
        auto oa = a.find("order"), ob = b.find("order");
        return toInt(oa == a.end() ? "0" : oa->second) < toInt(ob == b.end() ? "0" : ob->second);
    });
    for(auto& i : items) {
        auto n = i.find("name"), u = i.find("url");
        if(n != i.end() && u != i.end() && !u->second.empty() && !contains(u->second, "waifu2x")) out.emplace_back(n->second, u->second);
    }
    if(out.empty())
        out = {{"IQDB", "https://iqdb.org/?url={url}"},
               {"SauceNAO", "https://saucenao.com/search.php?db=999&url={url}"},
               {"Google Lens", "https://lens.google.com/uploadbyurl?url={url}"},
               {"TinEye", "https://www.tineye.com/search/?url={url}"},
               {"Yandex", "https://yandex.com/images/search?rpt=imageview&url={url}"}};
    return out;
}

// Label showing whether a filename format is valid
static GtkWidget* filenameValidator(GtkWidget* entryWidget) {
    GtkWidget* l = label("");
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    auto update = [entryWidget, l] {
        std::string msg;
        Filename fn(entryText(entryWidget));
        bool ok = fn.isValid(&app().profile->settings(), &msg, app().profile->additionalTokens());
        gtk_label_set_text(GTK_LABEL(l), msg.c_str());
        for(const char* c : {"bg-valid", "bg-warning", "bg-invalid"}) removeClass(l, c);
        addClass(l, ok ? (msg == "Valid filename!" ? "bg-valid" : "bg-warning") : (fn.parseError().empty() && !fn.format().empty() ? "bg-warning" : "bg-invalid"));
    };
    onChanged(entryWidget, update);
    update();
    return l;
}

static GtkWidget* folderRow(GtkWidget* entryWidget, GtkWidget* parentDialog) {
    GtkWidget* row = hbox(4);
    gtk_box_pack_start(GTK_BOX(row), entryWidget, TRUE, TRUE, 0);
    GtkWidget* browse = iconButton("folder-open-symbolic", "Choose a folder");
    onClicked(browse, [entryWidget, parentDialog] {
        std::string f = chooseFolder(GTK_WINDOW(parentDialog), "Choose a folder", entryText(entryWidget));
        if(!f.empty()) gtk_entry_set_text(GTK_ENTRY(entryWidget), f.c_str());
    });
    gtk_box_pack_start(GTK_BOX(row), browse, FALSE, FALSE, 0);
    return row;
}

static GtkWidget* sitesCombo(const std::string& active) {
    StringList ids;
    for(auto& [url, _] : app().profile->sites()) ids.push_back(url);
    return combo(ids, ids, active);
}

bool editGroupDialog(GtkWindow* parent, DownloadQueryGroup& g, const std::string& title) {
    GtkWidget* d = dialog(parent, title, 560);
    GtkWidget* gr = grid();
    GtkWidget* tags = entry(g.query.toString(), "Tags to search for");
    if(g.query.gallery) gtk_widget_set_sensitive(tags, FALSE);
    GtkWidget* site = sitesCombo(g.site ? g.site->url() : "");
    GtkWidget* page = spin(1, 100000, g.page);
    GtkWidget* perpage = spin(1, 1000, g.perpage);
    GtkWidget* all = gtk_check_button_new_with_label("All results");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(all), g.total < 0);
    GtkWidget* total = spin(1, 10000000, g.total > 0 ? g.total : 100);
    gtk_widget_set_sensitive(total, g.total > 0);
    onToggled(all, [all, total] { gtk_widget_set_sensitive(total, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all))); });
    GtkWidget* totalRow = hbox(6);
    gtk_box_pack_start(GTK_BOX(totalRow), total, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(totalRow), all, FALSE, FALSE, 0);
    GtkWidget* filename = entry(g.filename);
    GtkWidget* folder = entry(g.path);
    GtkWidget* filter = entry(join(g.postFiltering, " "), "e.g. -rating:explicit width:>=1920");
    GtkWidget* blacklisted = gtk_check_button_new_with_label("Download blacklisted images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blacklisted), g.getBlacklisted);
    GtkWidget* galleries = gtk_check_button_new_with_label("Count a gallery as one result");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(galleries), g.galleriesCountAsOne);
    int r = 0;
    gridRow(gr, r++, "Search", tags);
    gridRow(gr, r++, "Site", site);
    gridRow(gr, r++, "First page", page);
    gridRow(gr, r++, "Images per page", perpage);
    gridRow(gr, r++, "Images to download", totalRow);
    gridRow(gr, r++, "Filename", filename);
    gtk_grid_attach(GTK_GRID(gr), filenameValidator(filename), 1, r++, 1, 1);
    gridRow(gr, r++, "Folder", folderRow(folder, d));
    gridRow(gr, r++, "Post-filter", filter);
    gtk_grid_attach(GTK_GRID(gr), blacklisted, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(gr), galleries, 1, r++, 1, 1);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), gr, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
    if(ok) {
        if(!g.query.gallery) g.query.tags = split(entryText(tags), ' ', true);
        g.site = app().profile->site(comboId(site));
        g.page = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page));
        g.perpage = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(perpage));
        g.total = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all)) ? -1 : gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(total));
        g.filename = entryText(filename);
        g.path = entryText(folder);
        g.postFiltering = split(entryText(filter), ' ', true);
        g.getBlacklisted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(blacklisted));
        g.galleriesCountAsOne = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(galleries));
    }
    gtk_widget_destroy(d);
    return ok && g.site;
}

// Interval editor: value + unit
struct IntervalWidgets {
    GtkWidget *box, *value, *unit;
    int seconds() const {
        int mult = comboId(unit) == "d" ? 86400 : comboId(unit) == "h" ? 3600 : 60;
        return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(value)) * mult;
    }
};
static IntervalWidgets intervalEditor(int seconds) {
    IntervalWidgets w;
    std::string unit = seconds % 86400 == 0 && seconds > 0 ? "d" : seconds % 3600 == 0 && seconds > 0 ? "h" : "m";
    int div = unit == "d" ? 86400 : unit == "h" ? 3600 : 60;
    w.value = spin(0, 100000, seconds / div);
    w.unit = combo({"m", "h", "d"}, {"minutes", "hours", "days"}, unit);
    w.box = hbox(6);
    gtk_box_pack_start(GTK_BOX(w.box), w.value, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(w.box), w.unit, FALSE, FALSE, 0);
    return w;
}

static GtkWidget* sitesButton(GtkWindow* parent, StringList* sites) {
    GtkWidget* b = gtk_button_new_with_label(sites->empty() ? "Default sites" : join(*sites, ", ").c_str());
    onClicked(b, [b, parent, sites] {
        StringList chosen = *sites;
        if(chooseSitesDialog(parent, chosen)) {
            *sites = chosen;
            gtk_button_set_label(GTK_BUTTON(b), chosen.empty() ? "Default sites" : join(chosen, ", ").c_str());
        }
    });
    return b;
}

bool editMonitorDialog(GtkWindow* parent, Monitor& m, bool standalone, std::string*) {
    GtkWidget* d = dialog(parent, "Monitor", 560);
    GtkWidget* gr = grid();
    int r = 0;
    GtkWidget* tags = entry(m.query.toString(), "Tags to monitor");
    if(standalone) gridRow(gr, r++, "Search", tags);
    StringList sites = m.sites;
    gridRow(gr, r++, "Sites", sitesButton(GTK_WINDOW(d), &sites));
    IntervalWidgets interval = intervalEditor(m.interval);
    gridRow(gr, r++, "Check every", interval.box);
    IntervalWidgets delay = intervalEditor(m.delay);
    gridRow(gr, r++, "Ignore images younger than", delay.box,
            "Images are only counted once they are older than this, so their tags are complete");
    GtkWidget* notify = gtk_check_button_new_with_label("Notify about new images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notify), m.notify);
    GtkWidget* download = gtk_check_button_new_with_label("Download new images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(download), m.download);
    GtkWidget* blacklisted = gtk_check_button_new_with_label("Download blacklisted images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blacklisted), m.getBlacklisted);
    gtk_grid_attach(GTK_GRID(gr), notify, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(gr), download, 1, r++, 1, 1);
    gtk_grid_attach(GTK_GRID(gr), blacklisted, 1, r++, 1, 1);
    GtkWidget* folder = entry(m.pathOverride, "Default download folder");
    gridRow(gr, r++, "Folder", folderRow(folder, d));
    GtkWidget* filename = entry(m.filenameOverride, "Default filename");
    gridRow(gr, r++, "Filename", filename);
    GtkWidget* filter = entry(join(m.postFilters, " "));
    if(standalone) gridRow(gr, r++, "Post-filter", filter);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), gr, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
    if(ok) {
        if(standalone) {
            m.query.tags = split(entryText(tags), ' ', true);
            m.postFilters = split(entryText(filter), ' ', true);
        }
        m.sites = sites;
        m.interval = std::max(60, interval.seconds());
        m.delay = delay.seconds();
        m.notify = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notify));
        m.download = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(download));
        m.getBlacklisted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(blacklisted));
        m.pathOverride = entryText(folder);
        m.filenameOverride = entryText(filename);
    }
    gtk_widget_destroy(d);
    return ok && (!standalone || !m.query.tags.empty());
}

bool editFavoriteDialog(GtkWindow* parent, Favorite& f, bool isNew) {
    GtkWidget* d = dialog(parent, isNew ? "Add favorite" : "Edit favorite", 560);
    GtkWidget* gr = grid();
    int r = 0;
    GtkWidget* name = entry(f.name);
    gridRow(gr, r++, "Search", name);
    GtkWidget* note = spin(0, 100, f.note);
    gridRow(gr, r++, "Note (%)", note);
    StringList sites = f.sites;
    gridRow(gr, r++, "Sites", sitesButton(GTK_WINDOW(d), &sites));
    GtkWidget* filter = entry(join(f.postFiltering, " "));
    gridRow(gr, r++, "Post-filter", filter);
    GtkWidget* image = entry(f.imagePath, "Thumbnail image (optional)");
    GtkWidget* imgRow = hbox(4);
    gtk_box_pack_start(GTK_BOX(imgRow), image, TRUE, TRUE, 0);
    GtkWidget* browse = iconButton("document-open-symbolic", "Choose an image");
    onClicked(browse, [image, d] {
        std::string fsel = chooseFile(GTK_WINDOW(d), "Favorite thumbnail", false, entryText(image), "Images", "*.jpg;*.jpeg;*.png;*.gif;*.webp");
        if(!fsel.empty()) gtk_entry_set_text(GTK_ENTRY(image), fsel.c_str());
    });
    gtk_box_pack_start(GTK_BOX(imgRow), browse, FALSE, FALSE, 0);
    gridRow(gr, r++, "Thumbnail", imgRow);
    GtkWidget* monitor = gtk_check_button_new_with_label("Monitor this favorite for new images");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(monitor), !f.monitors.empty());
    gtk_grid_attach(GTK_GRID(gr), monitor, 1, r++, 1, 1);
    Monitor mon = f.monitors.empty() ? Monitor() : f.monitors[0];
    if(f.monitors.empty()) {
        Settings& s = app().profile->settings();
        mon.interval = (int)s.getInt("Monitoring/defaultInterval", 86400);
        mon.download = s.getBool("Monitoring/defaultDownloadEnabled", false);
        mon.notify = s.getBool("Monitoring/defaultNotificationEnabled", true);
        mon.lastSuccess = DateTime::nowUtc();
    }
    GtkWidget* editMonitor = button("Monitor settings…");
    onClicked(editMonitor, [d, &mon] { editMonitorDialog(GTK_WINDOW(d), mon, false); });
    gtk_grid_attach(GTK_GRID(gr), editMonitor, 1, r++, 1, 1);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), gr, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    bool ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
    if(ok) {
        f.name = trim(entryText(name));
        f.note = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(note));
        f.sites = sites;
        f.postFiltering = split(entryText(filter), ' ', true);
        std::string img = entryText(image);
        if(img != f.imagePath && fileExists(img)) {
            // Store a 150px copy in the profile
            std::string dest = buildPath(paths::dataDir(), "thumbs/" + f.cleanName() + ".png");
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(img.c_str(), 150, 150, TRUE, nullptr);
            if(pb && ensureParent(dest) && gdk_pixbuf_save(pb, dest.c_str(), "png", nullptr, nullptr)) f.imagePath = dest;
            if(pb) g_object_unref(pb);
        }
        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(monitor))) {
            if(f.monitors.empty())
                f.monitors.push_back(mon);
            else
                f.monitors[0] = mon;
        } else {
            f.monitors.clear();
        }
    }
    gtk_widget_destroy(d);
    return ok && !f.name.empty();
}

void addFavorite(GtkWindow* parent, const std::string& tag) {
    Favorite f;
    f.name = tag;
    f.lastViewed = DateTime::nowUtc();
    if(editFavoriteDialog(parent, f, true)) app().profile->addFavorite(f);
}

// ---- about ---------------------------------------------------------------------------------

void showAbout(GtkWindow* parent) {
    GtkWidget* d = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(d), parent);
    addClass(d, "bg");
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(d), BG_APP_NAME);
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(d), BG_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(d),
                                  "Imageboard/booru downloader with powerful file naming.\nGTK 3 rewrite of Grabber by Bionus.");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(d), BG_HOMEPAGE);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(d), GTK_LICENSE_APACHE_2_0);
    const char* authors[] = {"Bionus and the Grabber contributors (original program and site sources)", "TheElevatedOne (GTK rewrite)", nullptr};
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(d), authors);
    GdkPixbuf* logo = appIcon(128);
    if(logo) {
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(d), logo);
        g_object_unref(logo);
    }
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

// ---- tools ---------------------------------------------------------------------------------

namespace {
// Runs `step` from idle callbacks until it returns false, so long jobs keep the UI responsive
void runIncrementally(std::function<bool()> step) {
    auto* fn = new std::function<bool()>(std::move(step));
    g_idle_add_full(G_PRIORITY_LOW, [](gpointer d) -> gboolean {
        return (*static_cast<std::function<bool()>*>(d))() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }, fn, [](gpointer d) { delete static_cast<std::function<bool()>*>(d); });
}

void listFilesRecursive(const std::string& dir, StringList& out, int depth = 0) {
    if(depth > 32) return;
    for(auto& name : listDir(dir, true, true)) {
        std::string p = buildPath(dir, name);
        if(g_file_test(p.c_str(), G_FILE_TEST_IS_SYMLINK)) continue;
        if(isDir(p))
            listFilesRecursive(p, out, depth + 1);
        else if(!endsWith(name, ".tmp") && !endsWith(name, ".txt") && !endsWith(name, ".xmp"))
            out.push_back(p);
    }
}
}  // namespace

void md5DatabaseDialog(GtkWindow* parent) {
    Md5Database& db = app().profile->md5Database();
    GtkWidget* d = gtk_dialog_new_with_buttons("MD5 database", parent, GTK_DIALOG_MODAL, "_Close", GTK_RESPONSE_CLOSE, nullptr);
    addClass(d, "bg");
    GtkWidget* box = dialogContent(d);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget* info = label("The MD5 database remembers downloaded files to detect duplicates.");
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    GtkWidget* count = label("");
    GtkWidget* progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
    auto updateCount = [count, &db] { gtk_label_set_text(GTK_LABEL(count), (std::to_string(db.count()) + " files in the database").c_str()); };
    updateCount();
    GtkWidget* addFolder = button("Add a folder…", "folder-new-symbolic", "Add every file of a folder (recursively) to the database");
    GtkWidget* clean = button("Remove missing files", "edit-clear-symbolic", "Forget files that no longer exist");
    GtkWidget* clear = button("Clear", "edit-delete-symbolic", "Empty the database");
    auto busy = std::make_shared<bool>(false);
    onClicked(addFolder, [d, progress, updateCount, busy] {
        if(*busy) return;
        std::string folder = chooseFolder(GTK_WINDOW(d), "Folder to add", app().profile->savePath());
        if(folder.empty()) return;
        auto files = std::make_shared<StringList>();
        listFilesRecursive(folder, *files);
        auto index = std::make_shared<size_t>(0);
        *busy = true;
        std::weak_ptr<bool> alive = busy;
        runIncrementally([files, index, progress, updateCount, alive]() {
            auto b = alive.lock();
            if(!b) return false;
            Md5Database& db2 = app().profile->md5Database();
            for(int i = 0; i < 25 && *index < files->size(); i++, (*index)++) db2.add(fileMd5((*files)[*index]), (*files)[*index]);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), files->empty() ? 1 : (double)*index / files->size());
            gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), (std::to_string(*index) + " / " + std::to_string(files->size())).c_str());
            if(*index >= files->size()) {
                *b = false;
                updateCount();
                return false;
            }
            return true;
        });
    });
    onClicked(clean, [&db, updateCount, progress] {
        int removed = 0;
        for(auto& [md5, path] : db.all())
            if(!fileExists(path)) {
                db.remove(md5, path);
                removed++;
            }
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), (std::to_string(removed) + " missing files removed").c_str());
        updateCount();
    });
    onClicked(clear, [d, &db, updateCount] {
        if(confirm(GTK_WINDOW(d), "Clear the MD5 database?", "Duplicates of already downloaded files won't be detected anymore.")) {
            db.clear();
            updateCount();
        }
    });
    GtkWidget* buttons = hbox(6);
    for(GtkWidget* w : {addFolder, clean, clear}) gtk_box_pack_start(GTK_BOX(buttons), w, FALSE, FALSE, 0);
    for(GtkWidget* w : {info, count, buttons, progress}) gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    busy.reset();
    gtk_widget_destroy(d);
}

static void findEmpty(const std::string& dir, StringList& out, int depth = 0) {
    if(depth > 64) return;
    for(auto& name : listDir(dir, true, false)) {
        std::string p = buildPath(dir, name);
        if(g_file_test(p.c_str(), G_FILE_TEST_IS_SYMLINK)) continue;
        findEmpty(p, out, depth + 1);
    }
    // Empty once its empty subfolders are gone
    StringList entries = listDir(dir, true, true);
    bool empty = true;
    for(auto& e : entries) {
        std::string p = buildPath(dir, e);
        if(!isDir(p) || !contains(out, p)) {
            empty = false;
            break;
        }
    }
    if(empty && depth > 0) out.push_back(dir);
}

void emptyFoldersDialog(GtkWindow* parent) {
    std::string folder = chooseFolder(parent, "Folder to clean", app().profile->savePath());
    if(folder.empty()) return;
    StringList empty;
    findEmpty(folder, empty);
    if(empty.empty()) {
        message(parent, "No empty folders", "No empty folder was found in " + folder + ".");
        return;
    }
    std::string list = join(StringList(empty.begin(), empty.begin() + (long)std::min<size_t>(empty.size(), 15)), "\n");
    if(empty.size() > 15) list += "\n…";
    if(!confirm(parent, "Remove " + std::to_string(empty.size()) + " empty folders?", list)) return;
    int removed = 0;
    for(auto& e : empty)
        if(g_rmdir(e.c_str()) == 0) removed++;
    message(parent, "Done", std::to_string(removed) + " folders removed.");
}

void tagLoaderDialog(GtkWindow* parent) {
    GtkWidget* d = dialog(parent, "Load tag database", 460);
    GtkWidget* info = label("Downloads the complete tag list of a site so tag types are known without loading each image's details. "
                            "This can take a while for large sites.");
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    StringList ids;
    for(auto& [url, site] : app().profile->sites())
        if(site->tagsApi()) ids.push_back(url);
    GtkWidget* site = combo(ids, ids, ids.empty() ? "" : ids[0]);
    GtkWidget* min = spin(0, 1000000, 100);
    GtkWidget* gr = grid();
    gridRow(gr, 0, "Site", site);
    gridRow(gr, 1, "Minimum image count", min);
    GtkWidget* progress = label("");
    for(GtkWidget* w : {info, gr, progress}) gtk_box_pack_start(GTK_BOX(dialogContent(d)), w, FALSE, FALSE, 0);
    gtk_widget_show_all(d);
    std::shared_ptr<TagListLoader> loader;
    while(gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        auto s = app().profile->site(comboId(site));
        if(!s) break;
        gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_OK), FALSE);
        loader = std::make_shared<TagListLoader>(s, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(min)));
        loader->progress.connect([progress](int pages, int tags) {
            gtk_label_set_text(GTK_LABEL(progress), (std::to_string(pages) + " pages, " + std::to_string(tags) + " tags").c_str());
        });
        GtkWidget* dlg = d;
        loader->start([progress, dlg](bool ok, int count) {
            gtk_label_set_text(GTK_LABEL(progress), ((ok ? "Done: " : "Stopped: ") + std::to_string(count) + " tags saved").c_str());
            gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK), TRUE);
        });
    }
    if(loader) loader->abort();
    gtk_widget_destroy(d);
}

void importGrabberDialog(GtkWindow* parent) {
    std::string def = buildPath(g_get_user_config_dir(), "Bionus/Grabber");
    std::string folder = chooseFolder(parent, "Profile folder of the original Grabber (contains settings.ini)", isDir(def) ? def : g_get_home_dir());
    if(folder.empty()) return;
    if(!fileExists(buildPath(folder, "settings.ini"))) {
        message(parent, "Not a Grabber profile", folder + " does not contain a settings.ini file.", GTK_MESSAGE_ERROR);
        return;
    }
    if(!confirm(parent, "Import this Grabber profile?",
                "Settings, favorites, lists, the MD5 database and per-site settings are copied from\n" + folder +
                    "\n\nFiles they replace are kept with a .bak suffix. The application restarts to load them."))
        return;
    app().pendingImport = folder;
    if(app().window) app().window->requestQuit();
}

}  // namespace bg::gui
