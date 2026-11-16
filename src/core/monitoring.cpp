#include "core/monitoring.hpp"

#include <glib/gstdio.h>

#include "core/logger.hpp"
#include "core/net.hpp"
#include "core/paths.hpp"

namespace bg {

MonitoringCenter::MonitoringCenter(Profile* profile, DownloadQueue* queue) : m_profile(profile), m_queue(queue) {}

MonitoringCenter::~MonitoringCenter() {
    *m_alive = false;
    stop();
}

void MonitoringCenter::start() {
    if(m_timer) return;
    int startupDelay = (int)m_profile->settings().getInt("Monitoring/startupDelay", 0);
    std::weak_ptr<bool> alive = m_alive;
    after((unsigned)std::max(1, startupDelay) * 1000, [this, alive] {
        if(alive.lock()) tick();
    });
    m_timer = g_timeout_add_seconds(60, [](gpointer p) -> gboolean {
        static_cast<MonitoringCenter*>(p)->tick();
        return G_SOURCE_CONTINUE;
    }, this);
}

void MonitoringCenter::stop() {
    if(m_timer) g_source_remove(m_timer);
    m_timer = 0;
    m_jobs.clear();
}

void MonitoringCenter::tick() {
    if(m_checking) return;
    DateTime now = DateTime::nowUtc();
    auto& monitors = m_profile->monitors();
    for(size_t i = 0; i < monitors.size(); i++)
        if(monitors[i].nextCheck() <= now) m_jobs.push_back({false, "", i});
    for(auto& fav : m_profile->favorites())
        for(size_t i = 0; i < fav.monitors.size(); i++)
            if(fav.monitors[i].nextCheck() <= now) m_jobs.push_back({true, fav.name, i});
    runNextJob();
}

void MonitoringCenter::checkStandalone(size_t index) {
    m_jobs.push_back({false, "", index});
    if(!m_checking) runNextJob();
}

void MonitoringCenter::checkFavorite(const std::string& favorite, size_t index) {
    m_jobs.push_back({true, favorite, index});
    if(!m_checking) runNextJob();
}

void MonitoringCenter::checkAll() {
    auto& monitors = m_profile->monitors();
    for(size_t i = 0; i < monitors.size(); i++) m_jobs.push_back({false, "", i});
    for(auto& fav : m_profile->favorites())
        for(size_t i = 0; i < fav.monitors.size(); i++) m_jobs.push_back({true, fav.name, i});
    if(!m_checking) runNextJob();
}

Monitor* MonitoringCenter::resolve(const Job& j, SearchQuery* query, StringList* postFilters) {
    if(j.favorite) {
        Favorite* f = m_profile->favorite(j.favoriteName);
        if(!f || j.index >= f->monitors.size()) return nullptr;
        if(query) {
            query->tags = split(f->name, ' ', true);
            query->gallery.reset();
        }
        if(postFilters) *postFilters = f->postFiltering;
        return &f->monitors[j.index];
    }
    auto& monitors = m_profile->monitors();
    if(j.index >= monitors.size()) return nullptr;
    if(query) *query = monitors[j.index].query;
    if(postFilters) *postFilters = monitors[j.index].postFilters;
    return &monitors[j.index];
}

void MonitoringCenter::runNextJob() {
    if(m_jobs.empty()) {
        m_checking = false;
        statusChanged.emit();
        return;
    }
    Job j = m_jobs.front();
    m_jobs.pop_front();
    m_checking = true;
    statusChanged.emit();
    check(j);
}

void MonitoringCenter::check(const Job& job) {
    SearchQuery query;
    StringList postFilters;
    Monitor* monitor = resolve(job, &query, &postFilters);
    if(!monitor) {
        runNextJob();
        return;
    }
    StringList siteUrls = monitor->sites;
    if(siteUrls.empty()) siteUrls = m_profile->settings().getList("sites");
    auto sites = m_profile->filteredSites(siteUrls);
    if(sites.empty()) {
        log("Monitor '" + query.toString() + "' has no valid site", LogLevel::Warning);
        runNextJob();
        return;
    }
    int delay = monitor->delay;
    DateTime limit = DateTime::nowUtc().addSecs(-delay);
    DateTime lastSuccess = monitor->lastSuccess;
    bool download = monitor->download;
    log("Monitoring new images for '" + query.toString() + "' on '" + join(siteUrls, ", ") + "'", LogLevel::Info);

    struct State {
        size_t siteIndex = 0;
        int count = 0, newImages = 0;
        std::string state;
        bool success = true;
        std::vector<ImagePtr> found;
        std::shared_ptr<PackLoader> loader;
        int countRun = 0, newRun = 0;
        bool firstRun = true;
    };
    auto st = std::make_shared<State>();
    int perPage = (int)m_profile->settings().getInt("limit", 20);
    std::weak_ptr<bool> alive = m_alive;

    auto finishMonitor = [this, job, st, limit, siteUrls, query, alive]() {
        if(!alive.lock()) return;
        Monitor* m = resolve(job, nullptr, nullptr);
        if(m) {
            if(m->notify && st->newImages > 0) {
                std::string msg = st->newImages < st->count ? std::to_string(st->newImages) + " new image(s)"
                                                            : "More than " + std::to_string(st->newImages) + " new image(s)";
                notify.emit("New images for " + query.toString(), msg + " found for '" + query.toString() + "' on " + join(siteUrls, ", "));
            }
            if(m->download && st->newImages > 0) {
                Settings& s = m_profile->settings();
                std::string path = m->pathOverride.empty() ? m_profile->savePath() : expandHome(m->pathOverride);
                std::string filename = m->filenameOverride.empty() ? s.get("Save/filename") : m->filenameOverride;
                for(auto& img : st->found) {
                    DownloadQueryImage q;
                    q.image = img;
                    q.site = img->sitePtr();
                    q.filename = filename;
                    q.path = path;
                    m_queue->addUnique(q);
                }
                log("Added " + std::to_string(st->newImages) + " images to the download queue for monitor '" + query.toString() + "'", LogLevel::Info);
                if(!m_queue->running()) m_queue->start();
            }
            m->lastCheck = limit;
            if(st->success) m->lastSuccess = limit;
            if(m->lastState != st->state) {
                m->lastState = st->state;
                m->lastStateSince = DateTime::nowUtc();
                m->lastStateCount = 0;
            }
            m->lastStateCount++;
            m->cumulated += st->newImages;
            m->preciseCumulated = m->preciseCumulated && (st->count == 1 || st->newImages < st->count);
            if(job.favorite) {
                m_profile->syncFavorites();
                if(st->newImages > 0) m_profile->favoritesChanged.emit();
            } else {
                m_profile->syncMonitors();
                m_profile->monitorsChanged.emit();
            }
        }
        runNextJob();
    };

    auto nextSite = std::make_shared<std::function<void()>>();
    auto loadMore = std::make_shared<std::function<void()>>();
    *loadMore = [st, loadMore, nextSite, lastSuccess, delay, limit, download, alive]() {
        if(!alive.lock()) return;
        if(!(st->firstRun || download) || !st->loader->hasNext() || st->newRun != st->countRun) {
            (*nextSite)();
            return;
        }
        st->firstRun = false;
        st->loader->next([st, loadMore, lastSuccess, delay, limit](std::vector<ImagePtr> images) {
            st->countRun += (int)images.size();
            for(auto& img : images) {
                DateTime created = img->createdAt();
                if(created.valid() && created > lastSuccess && (delay <= 0 || created <= limit)) {
                    st->found.push_back(img);
                    st->newRun++;
                }
            }
            (*loadMore)();
        });
    };
    *nextSite = [st, sites, query, postFilters, perPage, loadMore, finishMonitor, nextSite, this, alive]() {
        if(!alive.lock()) return;
        if(st->loader) {
            // Close the previous site
            st->count += st->countRun;
            st->newImages += st->newRun;
            Site* prev = sites[st->siteIndex - 1].get();
            if(st->countRun == 0) log("No results for monitor '" + query.toString() + "' on site '" + prev->name() + "'", LogLevel::Warning);
            std::string newState = st->countRun == 0 ? "empty" : (st->newRun == 0 ? "finished" : "ok");
            st->state = st->state.empty() || st->state == newState ? newState : "mixed";
            st->success = st->success && st->countRun != 0;
        }
        if(st->siteIndex >= sites.size()) {
            finishMonitor();
            return;
        }
        auto site = sites[st->siteIndex++];
        DownloadQueryGroup q;
        q.query = query;
        q.page = 1;
        q.perpage = perPage;
        q.total = 1000;
        q.postFiltering = postFilters;
        q.site = site;
        st->loader = std::make_shared<PackLoader>(m_profile, q, perPage);
        st->loader->start();
        st->countRun = st->newRun = 0;
        st->firstRun = true;
        (*loadMore)();
    };
    (*nextSite)();
}

// ---- legacy profile migration ----------------------------------------------------------

// Copies from -> to, keeping a .bak copy of a file it replaces
static void copyOver(const std::string& from, const std::string& to, int& count) {
    if(!fileExists(from)) return;
    if(isDir(from)) {
        mkpath(to);
        for(auto& f : listDir(from, true, true)) copyOver(buildPath(from, f), buildPath(to, f), count);
        return;
    }
    ensureParent(to);
    if(fileExists(to)) renameFile(to, to + ".bak");
    if(copyFile(from, to, true)) count++;
}

int importGrabberProfile(const std::string& fromDir) {
    std::string from = expandHome(fromDir);
    if(!fileExists(buildPath(from, "settings.ini"))) return -1;
    std::string configDir = paths::configDir(), dataDir = paths::dataDir();
    log("Importing the Grabber profile from " + from, LogLevel::Info);
    int count = 0;

    // Settings, without Qt-only binary state (window geometry, column layouts); merged over the current ones
    IniFile ini, current;
    current.load(paths::settingsFile());
    if(ini.load(buildPath(from, "settings.ini"))) {
        for(auto it = ini.values.begin(); it != ini.values.end();) {
            std::string v = it->second.str();
            bool binary = startsWith(v, "@ByteArray(") || startsWith(v, "@Variant(") || startsWith(v, "@DateTime(");
            bool obsolete = startsWith(it->first, "GAnalytics/") || startsWith(it->first, "QtGoogleAnalytics/") || it->first == "baseStyle" ||
                            it->first == "send_usage_data" || it->first == "crashed" || it->first == "useQtUserAgent";
            if(binary || obsolete)
                it = ini.values.erase(it);
            else
                ++it;
        }
        for(auto& [k, v] : ini.values) current.values[k] = v;
        if(current.save(paths::settingsFile())) count++;
    }

    for(const char* f : {"favorites.json", "favorites.txt", "viewitlater.txt", "ignore.txt", "blacklist.txt", "wordsc.txt", "history.json",
                         "monitors.json", "restore.igl", "md5s.sqlite", "md5s.txt", "tabs.json", "filenamehistory.txt"})
        copyOver(buildPath(from, f), buildPath(dataDir, f), count);
    copyOver(buildPath(from, "thumbs"), buildPath(dataDir, "thumbs"), count);

    // Per-site data: settings to the config dir, cookies and tag databases to the data dir
    std::string sites = buildPath(from, "sites");
    std::string shared = paths::sitesDir();
    for(auto& source : listDir(sites, true, false)) {
        std::string srcDir = buildPath(sites, source);
        bool custom = !isDir(buildPath(shared, source)) && fileExists(buildPath(srcDir, "model.js"));
        if(custom) {
            for(auto& f : listDir(srcDir, false, true)) copyOver(buildPath(srcDir, f), buildPath(dataDir, "sites/" + source + "/" + f), count);
        }
        copyOver(buildPath(srcDir, "sites.txt"), buildPath(configDir, "sites/" + source + "/sites.txt"), count);
        for(auto& site : listDir(srcDir, true, false)) {
            std::string siteDir = buildPath(srcDir, site);
            copyOver(buildPath(siteDir, "settings.ini"), buildPath(configDir, "sites/" + source + "/" + site + "/settings.ini"), count);
            for(const char* f : {"tags.db", "tags.txt", "tag-types.txt"})
                copyOver(buildPath(siteDir, f), buildPath(dataDir, "sites/" + source + "/" + site + "/" + f), count);
            if(fileExists(buildPath(siteDir, "cookies.txt")))
                copyOver(buildPath(siteDir, "cookies.txt"), buildPath(dataDir, "legacy-cookies/" + site + ".txt"), count);
        }
    }
    log("Imported " + std::to_string(count) + " files from the Grabber profile", LogLevel::Info);
    return count;
}

void importLegacyCookies() {
    std::string dir = buildPath(paths::dataDir(), "legacy-cookies");
    if(!isDir(dir)) return;
    for(auto& f : listDir(dir, false, true)) {
        std::string domain = replaceAll(completeBaseName(f), "_", ":");
        size_t slash = domain.find('/');
        if(slash != std::string::npos) domain = domain.substr(0, slash);
        size_t colon = domain.find(':');
        if(colon != std::string::npos) domain = domain.substr(0, colon);
        for(auto& line : readLines(buildPath(dir, f))) net::Session::instance().addCookie(domain, line);
        removeFile(buildPath(dir, f));
    }
    g_rmdir(dir.c_str());
}

}  // namespace bg
