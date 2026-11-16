#pragma once

#include <memory>
#include <string>

#include "core/download.hpp"
#include "core/profile.hpp"

namespace bg {

// Periodically checks monitors (standalone and attached to favorites) for new images
class MonitoringCenter {
public:
    MonitoringCenter(Profile* profile, DownloadQueue* queue);
    ~MonitoringCenter();
    void start();
    void stop();
    bool running() const { return m_timer != 0; }
    bool checking() const { return m_checking; }
    // Checks one monitor now: standalone monitors by index, favorite monitors by favorite name + index
    void checkStandalone(size_t index);
    void checkFavorite(const std::string& favorite, size_t index);
    void checkAll();

    Signal<const std::string&, const std::string&> notify;  // title, body
    Signal<> statusChanged;

private:
    struct Job {
        bool favorite;
        std::string favoriteName;
        size_t index;
    };
    void tick();
    void runNextJob();
    Monitor* resolve(const Job& j, SearchQuery* query, StringList* postFilters);
    void check(const Job& j);

    Profile* m_profile;
    DownloadQueue* m_queue;
    guint m_timer = 0;
    bool m_checking = false;
    std::deque<Job> m_jobs;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

// Imports a profile of the original Qt Grabber (settings, favorites, lists, MD5 database, per-site
// settings and tag databases) into the booru-grabber directories. Only runs when asked for
// (--import-grabber / the import menu entry); the old profile is never modified.
// Returns the number of imported files, or -1 when `from` holds no Grabber profile.
int importGrabberProfile(const std::string& from);
// Imports cookies.txt files left by the migration (Qt raw cookie format) into the cookie jar
void importLegacyCookies();

}  // namespace bg
