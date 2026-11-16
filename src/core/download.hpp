// Downloading: single images, result packs of a search, and the persistent download queue.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/image.hpp"
#include "core/page.hpp"

namespace bg {

class Profile;
class Site;
class Blacklist;

struct ImageSaveResult {
    std::string path;
    SizeType size = SizeType::Full;
    SaveResult result = SaveResult::Error;
};
using SaveResults = std::vector<ImageSaveResult>;

class ImageDownloader : public std::enable_shared_from_this<ImageDownloader> {
public:
    using Done = std::function<void(const ImagePtr&, const SaveResults&)>;

    ImageDownloader(Profile* profile, ImagePtr image, std::string filename, std::string folder, int count, bool addMd5 = true,
                    bool startCommands = false, bool loadTags = true, bool rotate = true, bool force = false,
                    SizeType size = SizeType::Unknown, bool postSave = true);
    ImageDownloader(Profile* profile, ImagePtr image, StringList paths, int count, bool addMd5 = true, bool startCommands = false,
                    bool rotate = true, bool force = false, SizeType size = SizeType::Unknown, bool postSave = true);
    void setBlacklist(const Blacklist* b) { m_blacklist = b; }
    void save(Done done);
    void abort();
    bool running() const { return m_running; }
    const ImagePtr& image() const { return m_image; }

    Signal<int64_t, int64_t> progress;

private:
    void setSize(SizeType size);
    int needExactTags() const;
    void loadedSave(Image::LoadResult result);
    void loadImage(bool rateLimit = false);
    void onResponse(const net::Response& r);
    SaveResults afterTemporarySave(SaveResult result);
    SaveResults makeResult(const StringList& paths, SaveResult r) const;
    void finish(const SaveResults& r);
    SizeType currentSize() const { return m_tryingSample ? SizeType::Sample : m_size; }

    Profile* m_profile;
    ImagePtr m_image;
    Filename m_filename;
    std::string m_folder;
    StringList m_paths;
    int m_count;
    bool m_addMd5, m_startCommands, m_loadTags, m_rotate, m_force, m_postSave;
    SizeType m_size = SizeType::Full;
    const Blacklist* m_blacklist = nullptr;
    std::string m_url, m_temporaryPath;
    bool m_tryingSample = false, m_running = false;
    int m_retries = 0;
    std::shared_ptr<net::Reply> m_reply;
    Done m_done;
};

struct DownloadQueryGroup {
    SearchQuery query;
    int page = 1, perpage = 20, total = 20;
    StringList postFiltering;
    bool getBlacklisted = false, galleriesCountAsOne = true;
    std::shared_ptr<Site> site;
    std::string filename, path;
    int progressVal = 0;
    bool progressFinished = false;

    nlohmann::json toJson(bool saveProgress = true) const;
    bool fromJson(const nlohmann::json& j, Profile* profile);
};

struct DownloadQueryImage {
    ImagePtr image;
    std::shared_ptr<Site> site;
    std::string filename, path;

    nlohmann::json toJson() const;
    bool fromJson(const nlohmann::json& j, Profile* profile);
};

// Loads the results of a group search in packs of images, following pages and galleries
class PackLoader : public std::enable_shared_from_this<PackLoader> {
public:
    PackLoader(Profile* profile, DownloadQueryGroup query, int packSize);
    void start();
    bool hasNext() const;
    int nextPackSize() const;
    void next(std::function<void(std::vector<ImagePtr>)> done);
    void abort();
    Signal<Page*> pageLoaded;

private:
    void loadNextPage();
    void finishPack();

    Profile* m_profile;
    DownloadQueryGroup m_query;
    int m_packSize;
    int m_total = 0;
    std::deque<PagePtr> m_pendingPages, m_pendingGalleries;
    std::vector<ImagePtr> m_overflow;
    bool m_overflowGallery = false, m_overflowHasNext = false, m_aborted = false;
    std::set<std::string> m_seen;
    // current pack
    std::vector<ImagePtr> m_results;
    int m_pageCount = 0, m_maxPages = 1;
    PagePtr m_current;
    std::function<void(std::vector<ImagePtr>)> m_done;
};

class BatchDownloader : public std::enable_shared_from_this<BatchDownloader> {
public:
    enum class Step { NotStarted, Login, PageDownload, ImageDownload, Finished, Aborted };
    enum Counter { Downloaded, AlreadyExists, Ignored, NotFound, Errors, Missing, CounterCount };

    BatchDownloader(Profile* profile, std::shared_ptr<DownloadQueryGroup> group);
    BatchDownloader(Profile* profile, std::vector<DownloadQueryImage> images);
    void start();
    void abort();
    Step step() const { return m_step; }
    int total() const { return m_total; }
    int processed() const { return m_processed; }
    int counter(Counter c) const { return m_counters[c]; }
    const std::vector<ImagePtr>& failed() const { return m_failed; }
    void setGetBlacklisted(bool b) { m_getBlacklisted = b; }  // single images only; groups have their own setting

    Signal<const ImagePtr&, const SaveResults&> imageSaved;
    Signal<const ImagePtr&, int64_t, int64_t> imageProgress;
    Signal<const ImagePtr&> imageStarted;
    Signal<> progressChanged;
    Signal<> finished;

private:
    void login();
    void nextPack();
    void startImages();
    void nextImage();
    void imageFinished(const ImagePtr& img, const SaveResults& r);
    void allFinished();
    std::string filenameFor(const ImagePtr& img) const;
    std::string pathFor(const ImagePtr& img) const;

    Profile* m_profile;
    std::shared_ptr<DownloadQueryGroup> m_group;
    std::vector<DownloadQueryImage> m_images;
    std::shared_ptr<PackLoader> m_packLoader;
    std::deque<ImagePtr> m_pending;
    std::map<Image*, std::shared_ptr<ImageDownloader>> m_downloaders;
    std::vector<ImagePtr> m_failed;
    Step m_step = Step::NotStarted;
    bool m_getBlacklisted = true;
    int m_total = 0, m_processed = 0, m_active = 0;
    int m_counters[CounterCount] = {0};
};

// The downloads list: search groups and single images, saved to restore.igl
class DownloadQueue {
public:
    explicit DownloadQueue(Profile* profile);
    ~DownloadQueue();

    struct Group {
        std::shared_ptr<DownloadQueryGroup> query;
        enum class Status { Pending, Running, Paused, Done, Failed } status = Status::Pending;
        int downloaded = 0, errors = 0, ignored = 0, alreadyExists = 0, notFound = 0;
        int processed = 0, total = 0;
        std::string message;
    };
    struct Unique {
        DownloadQueryImage query;
        enum class Status { Pending, Running, Done, Failed } status = Status::Pending;
        std::string message;
    };

    std::vector<Group>& groups() { return m_groups; }
    std::vector<Unique>& uniques() { return m_uniques; }
    void addGroup(const DownloadQueryGroup& g);
    void addUnique(const DownloadQueryImage& i);
    void removeGroup(size_t index);
    void removeUnique(size_t index);
    void clearFinished();
    void clear();
    void resetGroup(size_t index);

    bool load(const std::string& path = "");
    bool save(const std::string& path = "") const;

    void start();
    void startGroups(const std::vector<size_t>& indices);
    void pause();
    bool running() const { return m_running; }
    int currentGroup() const { return m_currentGroup; }

    Signal<> changed;                        // list contents changed
    Signal<> progressChanged;                // counters of the running item changed
    Signal<const ImagePtr&, int64_t, int64_t> imageProgress;
    Signal<const ImagePtr&, const SaveResults&> imageSaved;
    Signal<> finished;

private:
    void runNext();
    std::string defaultFile() const;

    Profile* m_profile;
    std::vector<Group> m_groups;
    std::vector<Unique> m_uniques;
    std::shared_ptr<BatchDownloader> m_batch;
    std::vector<size_t> m_runningUniques;
    bool m_running = false;
    int m_currentGroup = -1;
    std::vector<size_t> m_only;
};

// Loads the complete tag list of a site into its tag database
class TagListLoader : public std::enable_shared_from_this<TagListLoader> {
public:
    TagListLoader(std::shared_ptr<Site> site, int minCount = 0);
    void start(std::function<void(bool ok, int count)> done);
    void abort();
    Signal<int, int> progress;  // pages, tags

private:
    void loadPage();
    std::shared_ptr<Site> m_site;
    int m_minCount, m_page = 1, m_count = 0;
    TagList m_tags;
    std::shared_ptr<net::Reply> m_reply;
    std::function<void(bool, int)> m_done;
};

// Finds which source handles a URL by trying each source's "check" API
void guessSource(Profile* profile, const std::string& url, std::function<void(Source*)> done);

}  // namespace bg
