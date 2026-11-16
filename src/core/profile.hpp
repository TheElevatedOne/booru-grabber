// The user profile: settings, sources and sites, favorites, lists, MD5 database, history, monitors.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/filters.hpp"
#include "core/page.hpp"
#include "core/settings.hpp"
#include "core/site.hpp"

struct sqlite3;

namespace bg {

class Profile;

struct Monitor {
    StringList sites;
    SearchQuery query;
    int interval = 86400;  // seconds
    int delay = 0;
    DateTime lastCheck, lastSuccess;
    int cumulated = 0;
    bool preciseCumulated = true;
    bool download = false;
    std::string pathOverride, filenameOverride;
    StringList postFilters;
    bool notify = true;
    bool getBlacklisted = false;
    std::string lastState;
    DateTime lastStateSince;
    int lastStateCount = 0;

    DateTime nextCheck() const;
    nlohmann::json toJson() const;
    static Monitor fromJson(const nlohmann::json& j, Profile* profile);
};

struct Favorite {
    std::string name;
    int note = 50;
    DateTime lastViewed;
    std::string imagePath;
    StringList postFiltering;
    StringList sites;
    std::vector<Monitor> monitors;
    nlohmann::json lastImages = nlohmann::json::object();

    std::string cleanName() const;
    nlohmann::json toJson() const;
    static Favorite fromJson(const nlohmann::json& j, Profile* profile);
    bool operator==(const Favorite& o) const { return iequals(name, o.name); }
};

struct HistoryEntry {
    SearchQuery query;
    StringList sites;
    DateTime date;
};

class Md5Database {
public:
    Md5Database(const std::string& path, Settings* settings);
    ~Md5Database();
    std::pair<std::string, std::string> action(const std::string& md5, const std::string& target);
    StringList exists(const std::string& md5);
    void add(const std::string& md5, const std::string& path);
    void remove(const std::string& md5, const std::string& path = "");
    StringList paths(const std::string& md5);
    int count();
    void importText(const std::string& path);
    std::vector<std::pair<std::string, std::string>> all();
    void clear();

private:
    std::pair<std::string, std::string> action(const std::string& md5, const StringList& paths, std::string action);
    sqlite3* m_db = nullptr;
    Settings* m_settings;
};

class Commands {
public:
    explicit Commands(Profile* profile);
    ~Commands();
    void before();
    void after();
    void image(const Image& img, const std::string& path);
    void tag(const Image& img, const Tag& tag, bool after);

private:
    bool execute(const std::string& command) const;
    bool sqlExec(const std::string& sql);
    Profile* m_profile;
    sqlite3* m_sql = nullptr;
    bool m_sqlTried = false;
};

class Profile {
public:
    // Uses the paths module (XDG directories, portable or --profile directory)
    Profile();
    ~Profile();
    Profile(const Profile&) = delete;
    Profile& operator=(const Profile&) = delete;

    void sync();
    Settings& settings() { return *m_settings; }
    const Settings& settings() const { return *m_settings; }
    std::string configDir() const;
    std::string dataDir() const;
    std::string tempPath() const;
    std::string savePath() const;  // Save/path, or $XDG_PICTURES_DIR/Booru Grabber when unset
    void purgeTemp(int maxAgeSeconds);

    // Sources & sites
    const std::map<std::string, std::unique_ptr<Source>>& sources() const { return m_sources; }
    Source* source(const std::string& name) const;
    const std::map<std::string, std::shared_ptr<Site>>& sites() const { return m_sites; }
    std::shared_ptr<Site> site(const std::string& url) const;
    std::vector<std::shared_ptr<Site>> filteredSites(const StringList& urls) const;
    std::shared_ptr<Site> addSite(Source* source, const std::string& url);
    void removeSite(const std::string& url);
    const StringList& additionalTokens() const { return m_additionalTokens; }

    // Favorites
    std::vector<Favorite>& favorites() { return m_favorites; }
    Favorite* favorite(const std::string& name);
    void addFavorite(const Favorite& f);
    void removeFavorite(const std::string& name);
    void syncFavorites();

    // Lists
    const StringList& keptForLater() const { return m_keptForLater; }
    void addKeptForLater(const std::string& tag);
    void removeKeptForLater(const std::string& tag);
    const StringList& ignored() const { return m_ignored; }
    void setIgnored(const StringList& tags);
    void addIgnored(const std::string& tag);
    void removeIgnored(const std::string& tag);
    TagFilterList& removedTags() { return m_removedTags; }
    void setRemovedTags(const std::string& raw);
    Blacklist& blacklist() { return m_blacklist; }
    void setBlacklist(const Blacklist& b);
    void addBlacklistedTag(const std::string& tag);
    void removeBlacklistedTag(const std::string& tag);

    // Auto-completion
    const StringList& autoComplete() const { return m_autoComplete; }
    void addAutoComplete(const TagList& tags);

    // MD5
    Md5Database& md5Database() { return *m_md5; }
    std::pair<std::string, std::string> md5Action(const std::string& md5, const std::string& target) { return m_md5->action(md5, target); }
    StringList md5Exists(const std::string& md5) { return m_md5->exists(md5); }
    void addMd5(const std::string& md5, const std::string& path) { m_md5->add(md5, path); }
    void removeMd5(const std::string& md5, const std::string& path = "") { m_md5->remove(md5, path); }

    // History
    const std::vector<HistoryEntry>& history() const { return m_history; }
    void addHistory(const SearchQuery& query, const StringList& sites);
    void clearHistory();

    // Monitors (standalone, not attached to a favorite)
    std::vector<Monitor>& monitors() { return m_monitors; }
    void syncMonitors();

    Commands& commands() { return *m_commands; }

    Signal<> favoritesChanged, keptForLaterChanged, ignoredChanged, blacklistChanged, sitesChanged, monitorsChanged, historyChanged;

private:
    void loadSources();
    void loadLists();
    void syncList(const std::string& file, const StringList& list) const;

    std::unique_ptr<Settings> m_settings;
    std::map<std::string, std::unique_ptr<Source>> m_sources;
    std::map<std::string, std::shared_ptr<Site>> m_sites;
    StringList m_additionalTokens;
    std::vector<Favorite> m_favorites;
    StringList m_keptForLater, m_ignored, m_autoComplete, m_customAutoComplete;
    TagFilterList m_removedTags;
    Blacklist m_blacklist;
    std::unique_ptr<Md5Database> m_md5;
    std::vector<HistoryEntry> m_history;
    std::vector<Monitor> m_monitors;
    std::unique_ptr<Commands> m_commands;
};

}  // namespace bg
