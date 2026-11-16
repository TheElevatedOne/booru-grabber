#include "gui/settingsdialog.hpp"

#include <tuple>

#include "core/filename.hpp"
#include "core/net.hpp"
#include "core/profile.hpp"
#include "gui/mainwindow.hpp"
#include "gui/style.hpp"
#include "gui/tray.hpp"

namespace bg::gui {

namespace {

struct Builder {
    explicit Builder(Settings& settings) : s(settings) {}
    Settings& s;
    GtkWidget* grid = nullptr;
    int row = 0;
    std::vector<std::function<void()>> savers;

    GtkWidget* page(GtkWidget* stack, const char* id, const char* title) {
        grid = bg::gui::grid();
        addClass(grid, "bg-settings-page");
        row = 0;
        GtkWidget* sw = scrolled(grid);
        gtk_stack_add_titled(GTK_STACK(stack), sw, id, title);
        return grid;
    }
    void header(const std::string& title) {
        GtkWidget* l = section(title);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
    }
    void note(const std::string& text) {
        GtkWidget* l = label(text);
        gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 70);
        addClass(l, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
    }
    GtkWidget* boolean(const std::string& key, const std::string& text, bool def, const std::string& tip = "") {
        GtkWidget* c = gtk_check_button_new_with_label(text.c_str());
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), s.getBool(key, def));
        if(!tip.empty()) gtk_widget_set_tooltip_text(c, tip.c_str());
        gtk_grid_attach(GTK_GRID(grid), c, 0, row++, 2, 1);
        savers.push_back([this, c, key] { s.setBool(key, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c))); });
        return c;
    }
    GtkWidget* integer(const std::string& key, const std::string& text, long long def, double min, double max, const std::string& tip = "") {
        GtkWidget* sp = spin(min, max, (double)s.getInt(key, def));
        gridRow(grid, row++, text, sp, tip);
        savers.push_back([this, sp, key] { s.setInt(key, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp))); });
        return sp;
    }
    GtkWidget* text(const std::string& key, const std::string& label, const std::string& def, const std::string& tip = "", bool password = false) {
        GtkWidget* e = entry(s.get(key, def));
        if(password) gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
        gridRow(grid, row++, label, e, tip);
        savers.push_back([this, e, key] { s.set(key, entryText(e)); });
        return e;
    }
    GtkWidget* choice(const std::string& key, const std::string& label, const std::string& def, const StringList& ids, const StringList& labels,
                      const std::string& tip = "") {
        GtkWidget* c = combo(ids, labels, s.get(key, def));
        gridRow(grid, row++, label, c, tip);
        savers.push_back([this, c, key] { s.set(key, comboId(c)); });
        return c;
    }
    GtkWidget* folder(const std::string& key, const std::string& label, const std::string& def, GtkWindow* parent) {
        GtkWidget* e = entry(s.get(key, def), def);
        GtkWidget* box = hbox(4);
        gtk_box_pack_start(GTK_BOX(box), e, TRUE, TRUE, 0);
        GtkWidget* b = iconButton("folder-open-symbolic", "Choose a folder");
        onClicked(b, [e, parent] {
            std::string f = chooseFolder(parent, "Choose a folder", entryText(e));
            if(!f.empty()) gtk_entry_set_text(GTK_ENTRY(e), f.c_str());
        });
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
        gridRow(grid, row++, label, box);
        savers.push_back([this, e, key] { s.set(key, entryText(e)); });
        return e;
    }
    GtkWidget* filename(const std::string& key, const std::string& label, const std::string& def) {
        GtkWidget* e = text(key, label, def, "Tokens: %md5% %id% %ext% %artist% %copyright% %character% %general% %rating% %website% %date:format=yyyy-MM-dd% …");
        GtkWidget* status = bg::gui::label("");
        gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
        auto update = [e, status, this] {
            std::string msg;
            Filename fn(entryText(e));
            bool ok = fn.empty() || fn.isValid(&s, &msg, app().profile->additionalTokens());
            gtk_label_set_text(GTK_LABEL(status), msg.c_str());
            for(const char* c : {"bg-valid", "bg-warning", "bg-invalid"}) removeClass(status, c);
            addClass(status, ok ? (msg == "Valid filename!" ? "bg-valid" : "bg-warning") : "bg-invalid");
        };
        onChanged(e, update);
        update();
        gtk_grid_attach(GTK_GRID(grid), status, 1, row++, 1, 1);
        return e;
    }
    GtkWidget* area(const std::string& initial, const std::string& label, int height, std::function<void(const std::string&)> save) {
        GtkWidget* tv = gtk_text_view_new();
        addClass(tv, "bg-mono");
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), initial.c_str(), -1);
        GtkWidget* sw = scrolled(tv, true);
        gtk_widget_set_size_request(sw, 420, height);
        gtk_widget_set_hexpand(sw, TRUE);
        if(!label.empty()) {
            GtkWidget* l = bg::gui::label(label);
            gtk_grid_attach(GTK_GRID(grid), l, 0, row++, 2, 1);
        }
        gtk_grid_attach(GTK_GRID(grid), sw, 0, row++, 2, 1);
        savers.push_back([tv, save] {
            GtkTextBuffer* b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
            GtkTextIter a, z;
            gtk_text_buffer_get_bounds(b, &a, &z);
            gchar* t = gtk_text_buffer_get_text(b, &a, &z, FALSE);
            save(t);
            g_free(t);
        });
        return tv;
    }
    void color(const std::string& key, const std::string& label, const std::string& def) {
        GtkWidget* box = hbox(6);
        GtkWidget* useDefault = gtk_check_button_new_with_label(def.empty() ? "Theme text colour" : "Default");
        GdkRGBA c;
        std::string current = s.get(key, def);
        bool isDefault = !s.containsOwn(key);
        if(current.empty() || !gdk_rgba_parse(&c, current.c_str())) gdk_rgba_parse(&c, def.empty() ? "#888888" : def.c_str());
        GtkWidget* btn = gtk_color_button_new_with_rgba(&c);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(useDefault), isDefault);
        gtk_widget_set_sensitive(btn, !isDefault);
        onToggled(useDefault, [useDefault, btn] { gtk_widget_set_sensitive(btn, !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(useDefault))); });
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), useDefault, FALSE, FALSE, 0);
        gridRow(grid, row++, label, box);
        savers.push_back([this, key, btn, useDefault] {
            if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(useDefault))) {
                s.remove(key);
                return;
            }
            GdkRGBA rgba;
            gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &rgba);
            char buf[8];
            snprintf(buf, sizeof buf, "#%02x%02x%02x", (int)(rgba.red * 255), (int)(rgba.green * 255), (int)(rgba.blue * 255));
            s.set(key, buf);
        });
    }
};

}  // namespace

void settingsDialog(GtkWindow* parent) {
    Profile* p = app().profile;
    Settings& s = p->settings();
    GtkWidget* d = dialog(parent, "Settings", 900, 640);
    GtkWidget* box = hbox(0);
    GtkWidget* stack = gtk_stack_new();
    GtkWidget* side = gtk_stack_sidebar_new();
    gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(side), GTK_STACK(stack));
    addClass(side, "bg-settings-sidebar");
    gtk_box_pack_start(GTK_BOX(box), side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dialogContent(d)), box, TRUE, TRUE, 0);
    GtkWindow* win = GTK_WINDOW(d);
    Builder b(s);

    // General
    b.page(stack, "general", "General");
    b.header("Startup");
    b.choice("start", "On startup", "restore", {"restore", "none"}, {"Restore the previous tabs", "Open an empty tab"});
    b.boolean("confirm_close", "Ask before quitting while downloading", true);
    b.header("Results");
    b.integer("limit", "Images per page", 20, 1, 1000);
    b.integer("Interface/thumbnailSize", "Thumbnail size (px)", 150, 64, 600);
    b.boolean("mergeresults", "Merge the results of all sites by default", false);
    b.boolean("hideblacklisted", "Hide blacklisted images instead of marking them", false);
    b.boolean("downloadblacklist", "Download blacklisted images in \"Get all\"", false);
    b.integer("tagsautoadd", "Add tags to auto-completion above this count", 10, 0, 100000000);
    b.header("Image viewer");
    b.boolean("imageNavigateScroll", "Mouse wheel changes image (Ctrl+wheel zooms)", true);
    b.boolean("imageCloseMiddleClick", "Middle click closes the viewer", true);
    b.boolean("Viewer/singleWindow", "Reuse a single viewer window", false);
    b.boolean("Viewer/viewSamples", "Show samples instead of full images", false);
    b.boolean("Viewer/scaleUp", "Enlarge small images to the window", false);
    b.boolean("Viewer/useVideoPlayer", "Play videos in the viewer", true);
    b.integer("Viewer/slideshow", "Slideshow interval (s)", 5, 1, 3600);
    b.choice("Viewer/tagOrder", "Tag order", "type", {"type", "name", "count", "original"}, {"Type", "Name", "Count", "As returned"});
    b.boolean("Viewer/showTagCount", "Show tag counts", false);

    // Sources
    b.page(stack, "sources", "Sources");
    b.header("API order");
    b.note("Sites try their APIs in this order until one returns results. A site can override it in its own settings.");
    StringList apiIds{"", "xml", "json", "regex", "rss"}, apiLabels{"—", "XML", "JSON", "HTML", "RSS"};
    StringList defaults{"xml", "json", "regex", "rss"};
    for(int i = 0; i < 4; i++) b.choice("source_" + std::to_string(i + 1), "API " + std::to_string(i + 1), defaults[(size_t)i], apiIds, apiLabels);
    b.header("Requests");
    b.text("userAgent", "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0",
           "Sent to sites that don't define their own");
    b.integer("Network/timeout", "Network timeout (s)", 60, 0, 3600);

    // Save
    b.page(stack, "save", "Saving");
    b.header("Location");
    b.folder("Save/path", "Folder", p->savePath(), win);
    b.filename("Save/filename", "Filename", "%md5%.%ext%");
    b.folder("Save/path_favorites", "Folder for favorites", "", win);
    b.filename("Save/filename_favorites", "Filename for favorites", "");
    b.header("Downloading");
    b.boolean("Save/downloadoriginals", "Download originals (not samples)", true);
    b.boolean("Save/samplefallback", "Use the sample when the original is missing", true);
    b.integer("Save/simultaneous", "Simultaneous downloads", 1, 1, 10);
    b.boolean("Save/keepDate", "Set the file date to the upload date", true);
    b.boolean("Save/headerDetection", "Fix wrong extensions from the file contents", true);
    b.choice("Save/multiple_files", "When a filename gives several paths", "copy", {"copy", "link"}, {"Copy the file", "Create symbolic links"});
    b.header("Filenames");
    b.boolean("Save/noJpeg", "Use .jpg instead of .jpeg", true);
    b.boolean("Save/replaceblanks", "Keep underscores in tags (don't replace them with spaces)", false);
    b.text("Save/separator", "Tag separator", " ");
    b.integer("Save/limit", "Maximum filename length (0 = system)", 0, 0, 255);
    b.header("Duplicates (MD5)");
    StringList dupIds{"save", "copy", "move", "link", "hardlink", "ignore"},
        dupLabels{"Save again", "Copy the existing file", "Move the existing file", "Create a symbolic link", "Create a hard link", "Skip"};
    b.choice("Save/md5Duplicates", "Already downloaded elsewhere", "save", dupIds, dupLabels);
    b.choice("Save/md5DuplicatesSameDir", "Already in the same folder", "save", dupIds, dupLabels);
    b.boolean("Save/keepDeletedMd5", "Remember deleted files (don't download them again)", false);
    b.header("Extension guessing");
    for(auto [key, text, def] : {std::tuple<const char*, const char*, const char*>{"extensionRotationStatic", "Images", "jpg, png, gif, jpeg, webm, swf, mp4"},
                                 {"extensionRotationAnimated", "Animations", "mp4, webm, gif, jpg, png, jpeg, swf"}}) {
        GtkWidget* e = b.text(key, text, def, "Extensions tried in order when a file is not found");
        std::string k = key;
        b.savers.push_back([&s, e, k] {
            StringList l;
            for(auto& x : split(entryText(e), ',', true))
                if(!trim(x).empty()) l.push_back(trim(x));
            s.setList(k, l);
        });
    }

    // Tag tokens
    b.page(stack, "tokens", "Tag tokens");
    b.note("How %artist%, %copyright%, %character%… are written when an image has none or several of them.");
    struct TypeDef {
        const char* key;
        const char* label;
        const char* empty;
        const char* multiple;
    } types[] = {{"artist", "Artists", "anonymous", "multiple artists"}, {"copyright", "Copyrights", "misc", "crossover"},
                 {"character", "Characters", "unknown", "group"},        {"model", "Models", "unknown", "multiple"},
                 {"species", "Species", "unknown", "multiple"},          {"meta", "Meta", "none", "multiple"},
                 {"general", "General", "", ""},                         {"contributor", "Contributors", "none", "multiple"},
                 {"lore", "Lore", "none", "multiple"},                   {"photo_set", "Photo sets", "unknown", "multiple"}};
    for(auto& t : types) {
        std::string k = std::string("Save/") + t.key;
        b.header(t.label);
        b.text(k + "_empty", "If empty", t.empty);
        b.choice(k + "_multiple", "If several", "keepAll", {"keepAll", "multiple", "keepN", "keepNThenAdd", "replaceAll"},
                 {"Keep all", "One file per value", "Keep the first N", "Keep N, then add text", "Replace by a text"});
        b.integer(k + "_multiple_limit", "\"Several\" means more than", 1, 1, 100);
        b.integer(k + "_multiple_keepN", "N (keep the first N)", 1, 1, 100);
        b.integer(k + "_multiple_keepNThenAdd_keep", "N (keep N, then add text)", 1, 1, 100);
        b.text(k + "_multiple_keepNThenAdd_add", "Text added after N", " (+ %count%)");
        b.text(k + "_value", "Replacement text", t.multiple);
        b.text(k + "_sep", "Separator", s.get("Save/separator", " "));
        b.choice(k + "_sort", "Order", "original", {"original", "name"}, {"As returned", "By name"});
    }
    b.boolean("Save/copyright_useshorter", "Shorten copyrights that start with another one", true);

    // Custom tokens & conditional filenames
    b.page(stack, "customs", "Custom tokens");
    b.note("Each line defines a token from a list of tags: \"name: tag1 tag2 tag3\". %name% then contains the image's tags from that list.");
    std::string customs;
    for(auto& [k, v] : getCustoms(&s)) customs += k + ": " + join(v, " ") + "\n";
    b.area(customs, "", 160, [&s](const std::string& t) {
        s.remove("Save/Customs");
        for(auto& line : split(t, '\n', true)) {
            size_t c = line.find(':');
            if(c == std::string::npos) continue;
            std::string name = trim(line.substr(0, c));
            if(!name.empty()) s.set("Save/Customs/" + name, trim(line.substr(c + 1)));
        }
    });
    b.header("Conditional filenames");
    b.note("One rule per line: \"condition | filename | folder\". When an image matches the condition (e.g. \"rating:explicit\" or "
           "\"%artist% & touhou\"), the filename and/or folder replace the default ones. Leave a part empty to keep the default.");
    std::string conds;
    for(auto& c : getConditionalFilenames(&s)) conds += c.condition + " | " + c.filename.format() + " | " + c.path + "\n";
    b.area(conds, "", 140, [&s](const std::string& t) {
        s.remove("Filenames");
        int i = 0;
        for(auto& line : split(t, '\n', true)) {
            // The last two "|" separate the filename and the folder, so conditions can still use "|" (or)
            StringList parts = split(line, '|');
            std::string fn, dir;
            if(parts.size() >= 3) {
                dir = trim(parts.back());
                parts.pop_back();
                fn = trim(parts.back());
                parts.pop_back();
            } else if(parts.size() == 2) {
                fn = trim(parts.back());
                parts.pop_back();
            }
            std::string cond = trim(join(parts, "|"));
            if(cond.empty()) continue;
            std::string k = "Filenames/" + std::to_string(i++);
            s.set(k + "_cond", cond);
            s.set(k + "_fn", fn);
            s.set(k + "_dir", dir);
        }
    });

    // Tag lists
    b.page(stack, "tags", "Tag lists");
    b.header("Blacklist");
    b.note("One rule per line; all filters of a line must match. Examples: \"guro\", \"rating:explicit comic\", \"-%artist%\", \"score:<0\".");
    b.area(p->blacklist().toString(), "", 150, [p](const std::string& t) { p->setBlacklist(Blacklist(split(t, '\n', true))); });
    b.header("Ignored tags");
    b.note("Ignored tags are treated as general tags in filenames.");
    b.area(join(p->ignored(), "\n"), "", 80, [p](const std::string& t) {
        StringList l;
        for(auto& x : split(t, '\n', true))
            if(!trim(x).empty()) l.push_back(trim(x));
        p->setIgnored(l);
    });
    b.header("Removed tags");
    b.note("Removed from the tag tokens of filenames. Wildcards (*) allowed.");
    b.area(s.get("ignoredtags"), "", 80, [p](const std::string& t) { p->setRemovedTags(t); });

    // Colours
    b.page(stack, "colors", "Colours");
    b.header("Tags");
    struct ColorDef {
        const char* key;
        const char* label;
        const char* def;
    } colors[] = {{"artists", "Artists", "#aa0000"},      {"copyrights", "Copyrights", "#aa00aa"}, {"characters", "Characters", "#00aa00"},
                  {"species", "Species", "#ee6600"},      {"metas", "Meta", "#ee6600"},             {"models", "Models", "#00aaff"},
                  {"circles", "Circles", "#55bbff"},      {"generals", "General", ""},              {"favorites", "Favorites", "#ffaaff"},
                  {"keptForLater", "View later", "#aaaa00"}, {"blacklisteds", "Blacklisted", "#444444"}, {"ignoreds", "Ignored", "#777777"}};
    for(auto& c : colors) b.color(std::string("Coloring/Colors/") + c.key, c.label, c.def);
    b.header("Thumbnail borders");
    b.color("Coloring/Borders/blacklisteds", "Blacklisted", "#444444");
    b.color("Coloring/Borders/favorites", "Favorited tag", "#ffaaff");
    b.color("Coloring/Borders/parent", "Has a parent", "#cccc00");
    b.color("Coloring/Borders/children", "Has children", "#00ff00");
    b.color("Coloring/Borders/pending", "Pending", "#0000ff");

    // Network
    b.page(stack, "network", "Proxy");
    b.boolean("Proxy/use", "Use a proxy", false);
    b.boolean("Proxy/useSystem", "Use the system proxy settings", false);
    b.choice("Proxy/type", "Type", "http", {"http", "socks5"}, {"HTTP", "SOCKS5"});
    b.text("Proxy/hostName", "Host", "");
    b.integer("Proxy/port", "Port", 8080, 1, 65535);
    b.text("Proxy/user", "User", "");
    b.text("Proxy/password", "Password", "", "", true);

    // Commands
    b.page(stack, "commands", "Commands");
    b.note("Commands run after each download, with filename tokens plus %path%. Tag commands also get %tag%, %type% and %number%.");
    b.text("Exec/tag_before", "Before each tag", "");
    b.text("Exec/image", "For each image", "");
    b.text("Exec/tag_after", "After each tag", "");
    b.integer("Exec/timeout", "Timeout (s)", 30, 1, 3600);
    b.boolean("Exec/dry_run", "Only log the commands (dry run)", false);
    b.header("SQLite database");
    b.text("Exec/SQL/database", "Database file", "");
    b.text("Exec/SQL/before", "Before downloads", "");
    b.text("Exec/SQL/tag_before", "Before each tag", "");
    b.text("Exec/SQL/image", "For each image", "");
    b.text("Exec/SQL/tag_after", "After each tag", "");
    b.text("Exec/SQL/after", "After downloads", "");
    b.boolean("Exec/SQL/dry_run", "Only log the queries (dry run)", false);
    s.set("Exec/SQL/driver", "QSQLITE");

    // Conversion & metadata
    b.page(stack, "conversion", "Conversion");
    b.header("Videos (ffmpeg)");
    b.boolean("Save/FFmpegRemuxWebmToMp4", "Remux VP9 WebM to MP4 (lossless)", false);
    b.boolean("Save/FFmpegConvertWebmToMp4", "Convert WebM to MP4", false);
    b.header("Pixiv ugoira (ffmpeg)");
    b.boolean("Save/ConvertUgoira", "Convert ugoira archives to animations", false);
    b.choice("Save/ConvertUgoiraFormat", "Format", "gif", {"gif", "webm", "mp4", "webp", "apng"}, {"GIF", "WebM", "MP4", "WebP", "APNG"});
    b.boolean("Save/ConvertUgoiraDeleteOriginal", "Delete the archive after converting", false);
    b.header("Resizing");
    b.boolean("ImageSize/maxWidthEnabled", "Limit the width", false);
    b.integer("ImageSize/maxWidth", "Maximum width", 1000, 1, 100000);
    b.boolean("ImageSize/maxHeightEnabled", "Limit the height", false);
    b.integer("ImageSize/maxHeight", "Maximum height", 1000, 1, 100000);
    b.header("Metadata (exiftool)");
    b.note("One \"Tag=value\" per line, values use filename tokens, e.g. \"XMP:Subject=%all:separator=;%\".");
    std::string meta;
    for(auto& item : s.readArray("Save/MetadataExiftool")) meta += item["key"] + "=" + item["value"] + "\n";
    b.area(meta, "", 100, [&s](const std::string& t) {
        std::vector<StringMap> items;
        for(auto& line : split(t, '\n', true)) {
            size_t eq = line.find('=');
            if(eq == std::string::npos || trim(line.substr(0, eq)).empty()) continue;
            items.push_back({{"key", trim(line.substr(0, eq))}, {"value", line.substr(eq + 1)}});
        }
        s.writeArray("Save/MetadataExiftool", items);
    });
    b.text("Save/MetadataExiftoolExtensions", "File types", "jpg jpeg png gif mp4");
    b.choice("Save/MetadataExiftoolSidecar", "XMP sidecar file", "on_error", {"no", "on_error", "both", "only"},
             {"Never", "When writing fails", "Always", "Only the sidecar"});
    b.boolean("Save/MetadataExiftoolClear", "Remove existing metadata first", false);

    // Monitoring
    b.page(stack, "monitoring", "Monitoring");
    b.integer("Monitoring/startupDelay", "Wait after startup (s)", 0, 0, 86400);
    b.integer("Monitoring/defaultInterval", "Default interval (s)", 86400, 60, 31536000);
    b.boolean("Monitoring/defaultDownloadEnabled", "New monitors download images", false);
    b.boolean("Monitoring/defaultNotificationEnabled", "New monitors notify", true);
    b.header("Tray icon");
#ifdef BG_HAVE_TRAY
    b.boolean("Monitoring/enableTray", "Show a tray icon", false);
#else
    b.boolean("Monitoring/enableTray", "Show a tray icon (not available in this build)", false);
#endif
    b.boolean("Monitoring/closeToTray", "Closing the window keeps running in the tray", false);

    gtk_widget_show_all(d);
    if(runDialog(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        for(auto& f : b.savers) f();
        s.sync();
        net::Session::instance().applySettings(s);
        for(auto& [_, site] : p->sites()) site->loadConfig();
        updateDynamicStyle();
        if(s.getBool("Monitoring/enableTray", false))
            setupTray();
        else
            removeTray();
        p->blacklistChanged.emit();
    }
    gtk_widget_destroy(d);
}

}  // namespace bg::gui
