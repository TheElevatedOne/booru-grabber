#include "core/profile.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <set>

#include "core/external.hpp"
#include "core/logger.hpp"
#include "core/net.hpp"
#include "core/paths.hpp"

namespace bg {

static nlohmann::json readJsonFile(const std::string& path) {
    auto data = readFile(path);
    if(!data) return nullptr;
    try {
        return nlohmann::json::parse(*data);
    } catch(const std::exception& e) {
        log("Error parsing " + path + ": " + e.what(), LogLevel::Error);
        return nullptr;
    }
}

static bool writeJsonFile(const std::string& path, const nlohmann::json& j) { return writeFile(path, j.dump(4) + "\n"); }

// ---- Monitor ------------------------------------------------------------------

DateTime Monitor::nextCheck() const {
    if(!lastCheck.valid()) return DateTime::nowUtc();
    return lastCheck.addSecs(interval);
}

nlohmann::json Monitor::toJson() const {
    nlohmann::json j;
    j["sites"] = sites;
    j["interval"] = interval;
    j["delay"] = delay;
    j["lastSuccess"] = lastSuccess.toIso();
    j["lastCheck"] = lastCheck.toIso();
    j["cumulated"] = cumulated;
    j["preciseCumulated"] = preciseCumulated;
    j["download"] = download;
    j["pathOverride"] = pathOverride;
    j["filenameOverride"] = filenameOverride;
    j["postFilters"] = postFilters;
    j["notify"] = notify;
    j["getBlacklisted"] = getBlacklisted;
    if(!lastState.empty()) j["lastState"] = {{"state", lastState}, {"since", lastStateSince.toIso()}, {"count", lastStateCount}};
    j["query"] = query.toJson();
    return j;
}

Monitor Monitor::fromJson(const nlohmann::json& j, Profile* profile) {
    Monitor m;
    if(!j.is_object()) return m;
    if(j.contains("sites") && j["sites"].is_array())
        for(auto& s : j["sites"])
            if(s.is_string()) m.sites.push_back(s.get<std::string>());
    if(j.contains("site") && j["site"].is_string()) m.sites.push_back(j["site"].get<std::string>());
    m.interval = j.value("interval", 86400);
    m.delay = j.value("delay", 0);
    m.lastCheck = DateTime::fromIso(j.value("lastCheck", ""));
    m.lastSuccess = j.contains("lastSuccess") ? DateTime::fromIso(j.value("lastSuccess", "")) : m.lastCheck;
    m.cumulated = j.value("cumulated", 0);
    m.preciseCumulated = j.value("preciseCumulated", true);
    m.download = j.value("download", false);
    m.pathOverride = j.value("pathOverride", "");
    m.filenameOverride = j.value("filenameOverride", "");
    m.notify = j.value("notify", true);
    m.getBlacklisted = j.value("getBlacklisted", false);
    if(j.contains("postFilters") && j["postFilters"].is_array())
        for(auto& s : j["postFilters"])
            if(s.is_string()) m.postFilters.push_back(s.get<std::string>());
    if(j.contains("lastState") && j["lastState"].is_object()) {
        m.lastState = j["lastState"].value("state", "");
        m.lastStateSince = DateTime::fromIso(j["lastState"].value("since", ""));
        m.lastStateCount = j["lastState"].value("count", 0);
    }
    if(j.contains("query")) m.query.fromJson(j["query"], profile);
    return m;
}

// ---- Favorite ------------------------------------------------------------------

std::string Favorite::cleanName() const {
    std::string out;
    for(char c : name)
        if(!strchr("\\/:*?\"<>|", c)) out += c;
    return out;
}

nlohmann::json Favorite::toJson() const {
    nlohmann::json j;
    j["tag"] = name;
    j["note"] = note;
    j["lastViewed"] = lastViewed.toIso();
    if(!postFiltering.empty() && !(postFiltering.size() == 1 && postFiltering[0].empty())) j["postFiltering"] = postFiltering;
    if(!monitors.empty()) {
        nlohmann::json a = nlohmann::json::array();
        for(auto& m : monitors) a.push_back(m.toJson());
        j["monitors"] = a;
    }
    if(!sites.empty()) j["sites"] = sites;
    if(!lastImages.empty()) j["lastImages"] = lastImages;
    return j;
}

Favorite Favorite::fromJson(const nlohmann::json& j, Profile* profile) {
    Favorite f;
    f.name = j.value("tag", "");
    f.note = j.value("note", 50);
    f.lastViewed = DateTime::fromIso(j.value("lastViewed", ""), false);
    if(j.contains("postFiltering") && j["postFiltering"].is_array())
        for(auto& s : j["postFiltering"])
            if(s.is_string()) f.postFiltering.push_back(s.get<std::string>());
    if(j.contains("sites") && j["sites"].is_array())
        for(auto& s : j["sites"])
            if(s.is_string()) f.sites.push_back(s.get<std::string>());
    if(j.contains("monitors") && j["monitors"].is_array())
        for(auto& m : j["monitors"]) f.monitors.push_back(Monitor::fromJson(m, profile));
    if(j.contains("lastImages") && j["lastImages"].is_object()) f.lastImages = j["lastImages"];
    std::string thumb = buildPath(paths::dataDir(), "thumbs/" + f.cleanName() + ".png");
    if(fileExists(thumb)) f.imagePath = thumb;
    return f;
}

// ---- Md5Database -----------------------------------------------------------------

Md5Database::Md5Database(const std::string& path, Settings* settings) : m_settings(settings) {
    ensureParent(path);
    if(sqlite3_open(path.c_str(), &m_db) != SQLITE_OK) {
        log("Could not open the MD5 database " + path, LogLevel::Error);
        sqlite3_close(m_db);
        m_db = nullptr;
        return;
    }
    sqlite3_exec(m_db,
                 "CREATE TABLE IF NOT EXISTS md5s (md5 CHAR(32), path TEXT);"
                 "CREATE INDEX IF NOT EXISTS md5s_md5 ON md5s(md5);",
                 nullptr, nullptr, nullptr);
}

Md5Database::~Md5Database() {
    if(m_db) sqlite3_close(m_db);
}

StringList Md5Database::paths(const std::string& md5) {
    StringList out;
    if(!m_db || md5.empty()) return out;
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(m_db, "SELECT path FROM md5s WHERE md5 = ?", -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(st, 1, md5.c_str(), -1, SQLITE_TRANSIENT);
    while(sqlite3_step(st) == SQLITE_ROW) out.emplace_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

void Md5Database::add(const std::string& md5, const std::string& path) {
    if(!m_db || md5.empty() || path.empty()) return;
    if(contains(paths(md5), path)) return;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(m_db, "INSERT INTO md5s (md5, path) VALUES (?, ?)", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, md5.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

void Md5Database::remove(const std::string& md5, const std::string& path) {
    if(!m_db) return;
    sqlite3_stmt* st = nullptr;
    if(path.empty()) {
        sqlite3_prepare_v2(m_db, "DELETE FROM md5s WHERE md5 = ?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, md5.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_prepare_v2(m_db, "DELETE FROM md5s WHERE md5 = ? AND path = ?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, md5.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_step(st);
    sqlite3_finalize(st);
}

int Md5Database::count() {
    if(!m_db) return 0;
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if(sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM md5s", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

std::vector<std::pair<std::string, std::string>> Md5Database::all() {
    std::vector<std::pair<std::string, std::string>> out;
    if(!m_db) return out;
    sqlite3_stmt* st = nullptr;
    if(sqlite3_prepare_v2(m_db, "SELECT md5, path FROM md5s", -1, &st, nullptr) != SQLITE_OK) return out;
    while(sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back((const char*)sqlite3_column_text(st, 0), (const char*)sqlite3_column_text(st, 1));
    sqlite3_finalize(st);
    return out;
}

void Md5Database::clear() {
    if(m_db) sqlite3_exec(m_db, "DELETE FROM md5s", nullptr, nullptr, nullptr);
}

// md5s.txt: "md5path" lines (32 hex chars followed by the path)
void Md5Database::importText(const std::string& path) {
    if(!m_db) return;
    sqlite3_exec(m_db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
    int n = 0;
    for(auto& line : readLines(path)) {
        if(line.size() <= 32) continue;
        add(line.substr(0, 32), line.substr(32));
        n++;
    }
    sqlite3_exec(m_db, "COMMIT", nullptr, nullptr, nullptr);
    log("Imported " + std::to_string(n) + " MD5s from " + path, LogLevel::Info);
}

std::pair<std::string, std::string> Md5Database::action(const std::string& md5, const std::string& target) {
    StringList pths = paths(md5);
    if(md5.empty() || pths.empty()) return {"save", ""};
    StringList sameDir, otherDir;
    std::string targetDir = target.empty() ? "" : dirName(target);
    for(auto& p : pths) {
        if(!target.empty() && dirName(p) == targetDir)
            sameDir.push_back(p);
        else
            otherDir.push_back(p);
    }
    if(!sameDir.empty()) {
        auto r = action(md5, sameDir, m_settings->get("Save/md5DuplicatesSameDir", "save"));
        if(!r.first.empty()) return r;
    }
    if(!otherDir.empty()) {
        auto r = action(md5, otherDir, m_settings->get("Save/md5Duplicates", "save"));
        if(!r.first.empty()) return r;
    }
    return {"save", ""};
}

std::pair<std::string, std::string> Md5Database::action(const std::string& md5, const StringList& pths, std::string act) {
    bool keepDeleted = m_settings->getBool("Save/keepDeletedMd5", false);
    for(auto& p : pths) {
        if(fileExists(p)) return {act, p};
        if(!keepDeleted) remove(md5, p);
    }
    if(keepDeleted) {
        if(act != "ignore") act = "save";
        return {act, pths.front()};
    }
    return {"", ""};
}

StringList Md5Database::exists(const std::string& md5) {
    StringList out;
    if(md5.empty()) return out;
    bool keepDeleted = m_settings->getBool("Save/keepDeletedMd5", false);
    for(auto& p : paths(md5)) {
        if(fileExists(p) || keepDeleted)
            out.push_back(p);
        else
            remove(md5, p);
    }
    return out;
}

// ---- Commands --------------------------------------------------------------------

Commands::Commands(Profile* profile) : m_profile(profile) {}

Commands::~Commands() {
    if(m_sql) sqlite3_close(m_sql);
}

static std::string sqlEscape(const std::string& s) { return "'" + replaceAll(s, "'", "''") + "'"; }

bool Commands::execute(const std::string& command) const {
    Settings& s = m_profile->settings();
    log("Execution of \"" + command + "\"", LogLevel::Info);
    if(s.getBool("Exec/dry_run", false)) return true;
    StringList args = splitCommand(command);
    if(args.empty()) return false;
    auto r = external::run(args, (int)s.getInt("Exec/timeout", 30) * 1000);
    if(!r.out.empty()) log("[Command stdout] " + trim(r.out), LogLevel::Debug);
    if(!r.err.empty()) log("[Command stderr] " + trim(r.err), LogLevel::Error);
    if(!r.started) {
        log("Error executing command: " + command, LogLevel::Error);
        return false;
    }
    if(r.timedOut) {
        log("Command execution timeout", LogLevel::Error);
        return false;
    }
    if(r.exitCode != 0) {
        log("Error executing command (return code: " + std::to_string(r.exitCode) + ")", LogLevel::Error);
        return false;
    }
    return true;
}

bool Commands::sqlExec(const std::string& sql) {
    Settings& s = m_profile->settings();
    log("SQL execution of \"" + sql + "\"", LogLevel::Info);
    if(s.getBool("Exec/SQL/dry_run", false)) return true;
    if(!m_sql && !m_sqlTried) {
        m_sqlTried = true;
        std::string driver = s.get("Exec/SQL/driver", "QSQLITE");
        if(driver != "QSQLITE" && driver != "sqlite") {
            log("SQL driver '" + driver + "' is not supported, only SQLite databases can be used", LogLevel::Error);
            return false;
        }
        std::string db = expandHome(s.get("Exec/SQL/database"));
        if(db.empty() || sqlite3_open(db.c_str(), &m_sql) != SQLITE_OK) {
            log("Could not open the SQLite database '" + db + "'", LogLevel::Error);
            if(m_sql) sqlite3_close(m_sql);
            m_sql = nullptr;
            return false;
        }
    }
    if(!m_sql) return false;
    char* err = nullptr;
    if(sqlite3_exec(m_sql, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        log("SQL error: " + std::string(err ? err : "?"), LogLevel::Error);
        sqlite3_free(err);
        return false;
    }
    return true;
}

void Commands::before() {
    std::string sql = m_profile->settings().get("Exec/SQL/before");
    if(!sql.empty()) sqlExec(sql);
}

void Commands::after() {
    std::string sql = m_profile->settings().get("Exec/SQL/after");
    if(!sql.empty()) sqlExec(sql);
}

void Commands::image(const Image& img, const std::string& path) {
    Settings& s = m_profile->settings();
    std::string cmd = s.get("Exec/image");
    if(!cmd.empty()) {
        for(auto exec : Filename(cmd).path(img.tokens(), &s, "", 0, Filename::None)) {
            exec = replaceAll(replaceAll(exec, "%path:nobackslash%", path), "%path%", path);
            if(!execute(exec)) break;
        }
    }
    std::string sql = s.get("Exec/SQL/image");
    if(!sql.empty()) {
        Filename fn(sql);
        fn.setEscapeMethod(sqlEscape);
        for(auto exec : fn.path(img.tokens(), &s, "", 0, Filename::None)) {
            exec = replaceAll(replaceAll(exec, "%path:nobackslash%", sqlEscape(path)), "%path%", sqlEscape(path));
            if(!sqlExec(exec)) break;
        }
    }
}

void Commands::tag(const Image& img, const Tag& tag, bool after) {
    Settings& s = m_profile->settings();
    std::string original = replaceAll(tag.text(), " ", "_");
    std::string number = std::to_string(img.site() ? img.site()->tagDatabase().tagTypeNumber(tag.type()) : tag.type().number());
    std::string cmd = after ? s.get("Exec/tag_after", s.get("Exec/tag")) : s.get("Exec/tag_before");
    if(!cmd.empty()) {
        for(auto exec : Filename(cmd).path(img.tokens(), &s, "", 0, Filename::KeepInvalidTokens)) {
            exec = replaceAll(exec, "%tag%", original);
            exec = replaceAll(exec, "%original%", tag.text());
            exec = replaceAll(exec, "%type%", tag.type().name());
            exec = replaceAll(exec, "%number%", number);
            if(!execute(exec)) break;
        }
    }
    std::string sql = after ? s.get("Exec/SQL/tag_after", s.get("Exec/SQL/tag")) : s.get("Exec/SQL/tag_before");
    if(!sql.empty()) {
        Filename fn(sql);
        fn.setEscapeMethod(sqlEscape);
        for(auto exec : fn.path(img.tokens(), &s, "", 0, Filename::KeepInvalidTokens)) {
            exec = replaceAll(exec, "%tag%", sqlEscape(original));
            exec = replaceAll(exec, "%original%", sqlEscape(tag.text()));
            exec = replaceAll(exec, "%type%", sqlEscape(tag.type().name()));
            exec = replaceAll(exec, "%number%", number);
            if(!sqlExec(exec)) break;
        }
    }
}

// ---- Profile ----------------------------------------------------------------------

Profile::Profile() {
    m_settings = std::make_unique<Settings>(paths::settingsFile(), paths::systemSettingsFiles());
    m_settings->renameGroup("Zoom", "Viewer");

    std::string sites = paths::sitesDir();
    js::setHelperFile(buildPath(sites, "helper.js"));
    net::Session::instance().init(buildPath(dataDir(), "cookies.txt"), buildPath(paths::cacheDir(), "http"));
    net::Session::instance().applySettings(*m_settings);

    loadSources();
    loadLists();

    std::string md5Sqlite = buildPath(dataDir(), "md5s.sqlite");
    bool importTxt = !fileExists(md5Sqlite) && fileExists(buildPath(dataDir(), "md5s.txt"));
    m_md5 = std::make_unique<Md5Database>(md5Sqlite, m_settings.get());
    if(importTxt) m_md5->importText(buildPath(dataDir(), "md5s.txt"));
    m_commands = std::make_unique<Commands>(this);
}

Profile::~Profile() {
    sync();
    m_sites.clear();
    net::Session::instance().shutdown();
}

std::string Profile::configDir() const { return paths::configDir(); }
std::string Profile::dataDir() const { return paths::dataDir(); }

std::string Profile::tempPath() const {
    std::string override = expandHome(m_settings->get("tempPathOverride"));
    if(!override.empty() && isDir(override)) return override;
    // On disk rather than in $XDG_RUNTIME_DIR (RAM): viewed videos can be large
    std::string dir = buildPath(paths::cacheDir(), "tmp");
    mkpath(dir);
    return dir;
}

std::string Profile::savePath() const {
    std::string p = expandHome(m_settings->get("Save/path"));
    if(!p.empty()) return p;
    const char* pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    return buildPath(pictures && *pictures ? pictures : g_get_home_dir(), "Booru Grabber");
}

void Profile::purgeTemp(int maxAge) {
    std::string dir = tempPath();
    int64_t limit = time(nullptr) - maxAge;
    int purged = 0, total = 0;
    for(auto& f : listDir(dir, false, true)) {
        total++;
        std::string p = buildPath(dir, f);
        if(fileMTime(p) < limit && removeFile(p)) purged++;
    }
    if(total) log("Temp directory purged of " + std::to_string(purged) + "/" + std::to_string(total) + " files", LogLevel::Info);
}

void Profile::loadSources() {
    std::string shared = paths::sitesDir();
    std::string userData = paths::userSitesDir();
    std::string userConfig = buildPath(configDir(), "sites");
    StringList names = listDir(shared, true, false);
    for(auto& n : listDir(userData, true, false))
        if(fileExists(buildPath(userData, n + "/model.js")) && !contains(names, n)) names.push_back(n);
    for(auto& name : names) {
        std::string readDir = fileExists(buildPath(userData, name + "/model.js")) ? buildPath(userData, name) : buildPath(shared, name);
        if(!fileExists(buildPath(readDir, "model.js"))) continue;
        auto source = std::make_unique<Source>(readDir, buildPath(userConfig, name));
        if(!source->valid()) continue;
        for(auto& t : source->additionalTokens())
            if(!contains(m_additionalTokens, t)) m_additionalTokens.push_back(t);
        Source* src = source.get();
        m_sources[source->name()] = std::move(source);
        for(auto& url : src->sites()) m_sites[url] = std::make_shared<Site>(url, src, this);
    }
    log("Loaded " + std::to_string(m_sources.size()) + " sources and " + std::to_string(m_sites.size()) + " sites", LogLevel::Info);
}

void Profile::loadLists() {
    std::string dir = dataDir();
    // Favorites
    m_favorites.clear();
    std::set<std::string> unique;
    auto favs = readJsonFile(buildPath(dir, "favorites.json"));
    if(favs.is_object() && favs.contains("favorites") && favs["favorites"].is_array()) {
        for(auto& f : favs["favorites"]) {
            Favorite fav = Favorite::fromJson(f, this);
            if(!fav.name.empty() && unique.insert(toLower(fav.name)).second) m_favorites.push_back(fav);
        }
    } else {
        for(auto& line : readLines(buildPath(dir, "favorites.txt"))) {
            StringList xp = split(line, '|');
            Favorite fav;
            fav.name = xp[0];
            fav.note = xp.size() > 1 ? (int)toInt(xp[1], 50) : 50;
            fav.lastViewed = xp.size() > 2 ? DateTime::fromIso(xp[2], false) : DateTime::fromUnix(946684800);
            if(!fav.name.empty() && unique.insert(toLower(fav.name)).second) m_favorites.push_back(fav);
        }
    }

    m_keptForLater = readLines(buildPath(dir, "viewitlater.txt"));
    m_ignored = readLines(buildPath(dir, "ignore.txt"));
    m_customAutoComplete.clear();
    for(auto& line : readLines(buildPath(dir, "wordsc.txt")))
        for(auto& w : split(line, ' ', true)) m_customAutoComplete.push_back(w);

    // Auto-complete
    std::set<std::string> words(m_customAutoComplete.begin(), m_customAutoComplete.end());
    std::string wordsFile = paths::findData("words.txt");
    if(!wordsFile.empty())
        for(auto& line : readLines(wordsFile))
            for(auto& w : split(line, ' ', true)) words.insert(w);
    for(auto& f : m_favorites) words.insert(f.name);
    for(const char* s : {"grabber:alreadyExists", "grabber:inMd5List", "grabber:downloaded", "grabber:favorited", "grabber:monitored"})
        words.insert(s);
    m_autoComplete.assign(words.begin(), words.end());

    // Blacklist
    m_blacklist.clear();
    for(auto& t : split(m_settings->get("blacklistedtags"), ' ', true)) m_blacklist.add(t);
    for(auto& line : readLines(buildPath(dir, "blacklist.txt"))) m_blacklist.add(line);

    m_removedTags.clear();
    m_removedTags.add(splitAny(m_settings->get("ignoredtags"), " \n", true));

    // History
    m_history.clear();
    auto history = readJsonFile(buildPath(dir, "history.json"));
    if(history.is_object() && history.contains("entries") && history["entries"].is_array()) {
        for(auto& e : history["entries"]) {
            HistoryEntry h;
            if(e.contains("query")) h.query.fromJson(e["query"], this);
            if(e.contains("sites") && e["sites"].is_array())
                for(auto& s : e["sites"])
                    if(s.is_string()) h.sites.push_back(s.get<std::string>());
            h.date = DateTime::fromIso(e.value("date", ""));
            if(!h.query.tags.empty() || h.query.gallery) m_history.push_back(h);
        }
    }

    // Monitors
    m_monitors.clear();
    auto monitors = readJsonFile(buildPath(dir, "monitors.json"));
    if(monitors.is_object() && monitors.contains("monitors") && monitors["monitors"].is_array())
        for(auto& m : monitors["monitors"]) m_monitors.push_back(Monitor::fromJson(m, this));
}

void Profile::syncList(const std::string& file, const StringList& list) const { writeFile(buildPath(dataDir(), file), join(list, "\n") + "\n"); }

void Profile::syncFavorites() {
    nlohmann::json a = nlohmann::json::array();
    for(auto& f : m_favorites) a.push_back(f.toJson());
    writeJsonFile(buildPath(dataDir(), "favorites.json"), {{"version", 1}, {"favorites", a}});
}

void Profile::syncMonitors() {
    nlohmann::json a = nlohmann::json::array();
    for(auto& m : m_monitors) a.push_back(m.toJson());
    writeJsonFile(buildPath(dataDir(), "monitors.json"), {{"version", 1}, {"monitors", a}});
}

void Profile::sync() {
    syncFavorites();
    syncList("viewitlater.txt", m_keptForLater);
    syncList("ignore.txt", m_ignored);
    syncList("blacklist.txt", m_blacklist.lines());
    syncList("wordsc.txt", m_customAutoComplete);
    nlohmann::json entries = nlohmann::json::array();
    for(auto& h : m_history) entries.push_back({{"query", h.query.toJson()}, {"sites", h.sites}, {"date", h.date.toIso()}});
    writeJsonFile(buildPath(dataDir(), "history.json"), {{"version", 1}, {"entries", entries}});
    syncMonitors();
    for(auto& [_, site] : m_sites) {
        site->settings().sync();
        site->tagDatabase().save();
    }
    m_settings->sync();
}

Source* Profile::source(const std::string& name) const {
    auto it = m_sources.find(name);
    return it == m_sources.end() ? nullptr : it->second.get();
}

std::shared_ptr<Site> Profile::site(const std::string& url) const {
    auto it = m_sites.find(url);
    return it == m_sites.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Site>> Profile::filteredSites(const StringList& urls) const {
    std::vector<std::shared_ptr<Site>> out;
    for(auto& u : urls) {
        auto s = site(u);
        if(s)
            out.push_back(s);
        else
            log("Unknown site: " + u, LogLevel::Error);
    }
    return out;
}

std::shared_ptr<Site> Profile::addSite(Source* source, const std::string& url) {
    std::string u = trim(url);
    if(startsWith(u, "https://")) u = u.substr(8);
    if(startsWith(u, "http://")) u = u.substr(7);
    while(endsWith(u, "/")) u.pop_back();
    if(u.empty()) return nullptr;
    source->addSite(u);
    auto s = std::make_shared<Site>(u, source, this);
    m_sites[u] = s;
    sitesChanged.emit();
    return s;
}

void Profile::removeSite(const std::string& url) {
    auto s = site(url);
    if(!s) return;
    s->source()->removeSite(url);
    m_sites.erase(url);
    sitesChanged.emit();
}

Favorite* Profile::favorite(const std::string& name) {
    for(auto& f : m_favorites)
        if(iequals(f.name, name)) return &f;
    return nullptr;
}

void Profile::addFavorite(const Favorite& f) {
    m_favorites.erase(std::remove(m_favorites.begin(), m_favorites.end(), f), m_favorites.end());
    m_favorites.push_back(f);
    auto pos = std::lower_bound(m_autoComplete.begin(), m_autoComplete.end(), f.name);
    if(pos == m_autoComplete.end() || *pos != f.name) m_autoComplete.insert(pos, f.name);
    syncFavorites();
    favoritesChanged.emit();
}

void Profile::removeFavorite(const std::string& name) {
    Favorite* f = favorite(name);
    if(!f) return;
    if(!f->imagePath.empty()) removeFile(f->imagePath);
    std::string n = f->name;
    m_favorites.erase(std::remove_if(m_favorites.begin(), m_favorites.end(), [&](const Favorite& x) { return iequals(x.name, n); }), m_favorites.end());
    syncFavorites();
    favoritesChanged.emit();
}

void Profile::addKeptForLater(const std::string& tag) {
    removeAll(m_keptForLater, tag);
    m_keptForLater.push_back(tag);
    syncList("viewitlater.txt", m_keptForLater);
    keptForLaterChanged.emit();
}

void Profile::removeKeptForLater(const std::string& tag) {
    removeAll(m_keptForLater, tag);
    syncList("viewitlater.txt", m_keptForLater);
    keptForLaterChanged.emit();
}

void Profile::setIgnored(const StringList& tags) {
    m_ignored = tags;
    syncList("ignore.txt", m_ignored);
    ignoredChanged.emit();
}

void Profile::addIgnored(const std::string& tag) {
    removeAll(m_ignored, tag);
    m_ignored.push_back(tag);
    syncList("ignore.txt", m_ignored);
    ignoredChanged.emit();
}

void Profile::removeIgnored(const std::string& tag) {
    removeAll(m_ignored, tag);
    syncList("ignore.txt", m_ignored);
    ignoredChanged.emit();
}

void Profile::setRemovedTags(const std::string& raw) {
    m_removedTags.clear();
    m_removedTags.add(splitAny(raw, " \n", true));
    m_settings->set("ignoredtags", raw);
}

void Profile::setBlacklist(const Blacklist& b) {
    m_blacklist = b;
    m_settings->remove("blacklistedtags");
    syncList("blacklist.txt", m_blacklist.lines());
    blacklistChanged.emit();
}

void Profile::addBlacklistedTag(const std::string& tag) {
    m_blacklist.add(StringList{replaceAll(tag, ":", "::")});
    setBlacklist(m_blacklist);
}

void Profile::removeBlacklistedTag(const std::string& tag) {
    if(!m_blacklist.remove(tag)) m_blacklist.remove(replaceAll(tag, ":", "::"));
    setBlacklist(m_blacklist);
}

void Profile::addAutoComplete(const TagList& tags) {
    int threshold = (int)m_settings->getInt("tagsautoadd", 10);
    for(auto& t : tags) {
        if(t.count() >= threshold && !std::binary_search(m_autoComplete.begin(), m_autoComplete.end(), t.text())) {
            m_customAutoComplete.push_back(t.text());
            m_autoComplete.insert(std::upper_bound(m_autoComplete.begin(), m_autoComplete.end(), t.text()), t.text());
        }
    }
}

void Profile::addHistory(const SearchQuery& query, const StringList& sites) {
    m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                   [&](const HistoryEntry& h) { return h.query.tags == query.tags && h.sites == sites; }),
                    m_history.end());
    m_history.insert(m_history.begin(), HistoryEntry{query, sites, DateTime::nowUtc()});
    if(m_history.size() > 500) m_history.resize(500);
    historyChanged.emit();
}

void Profile::clearHistory() {
    m_history.clear();
    historyChanged.emit();
}

}  // namespace bg
