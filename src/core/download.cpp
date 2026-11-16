#include "core/download.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cmath>

#include "core/logger.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"

namespace bg {

// ---- ImageDownloader --------------------------------------------------------------

ImageDownloader::ImageDownloader(Profile* profile, ImagePtr image, std::string filename, std::string folder, int count, bool addMd5,
                                 bool startCommands, bool loadTags, bool rotate, bool force, SizeType size, bool postSave)
    : m_profile(profile), m_image(std::move(image)), m_filename(std::move(filename)), m_folder(std::move(folder)), m_count(count),
      m_addMd5(addMd5), m_startCommands(startCommands), m_loadTags(loadTags), m_rotate(rotate), m_force(force), m_postSave(postSave) {
    setSize(size);
}

ImageDownloader::ImageDownloader(Profile* profile, ImagePtr image, StringList paths, int count, bool addMd5, bool startCommands, bool rotate,
                                 bool force, SizeType size, bool postSave)
    : m_profile(profile), m_image(std::move(image)), m_paths(std::move(paths)), m_count(count), m_addMd5(addMd5),
      m_startCommands(startCommands), m_loadTags(false), m_rotate(rotate), m_force(force), m_postSave(postSave) {
    setSize(size);
}

void ImageDownloader::setSize(SizeType size) {
    if(size == SizeType::Unknown) {
        bool originals = m_profile->settings().getBool("Save/downloadoriginals", true);
        bool hasSample = !m_image->url(SizeType::Sample).empty();
        m_size = originals || !hasSample ? SizeType::Full : SizeType::Sample;
    } else {
        m_size = size;
    }
}

int ImageDownloader::needExactTags() const {
    Settings& s = m_profile->settings();
    int need = 0;
    for(auto& group : s.childGroups("LogFiles")) {
        need = std::max(need, Filename(s.get("LogFiles/" + group + "/content")).needExactTags());
        if(need == 2) return need;
    }
    for(const char* key : {"Exec/tag_before", "Exec/image", "Exec/tag_after", "Exec/SQL/before", "Exec/SQL/tag_before", "Exec/SQL/image",
                           "Exec/SQL/tag_after", "Exec/SQL/after"}) {
        std::string v = s.get(key);
        if(v.empty()) continue;
        need = std::max(need, Filename(v).needExactTags());
        if(need == 2) return need;
    }
    for(auto& item : s.readArray("Save/MetadataExiftool")) {
        need = std::max(need, Filename(item["value"]).needExactTags());
        if(need == 2) return need;
    }
    return need;
}

void ImageDownloader::save(Done done) {
    m_done = std::move(done);
    m_running = true;
    Site* site = m_image->site();
    Settings& s = m_profile->settings();
    StringList forced = site && !site->apis().empty() ? site->apis().front()->forcedTokens() : StringList();
    StringList customs;
    for(auto& [k, _] : getCustoms(&s)) customs.push_back(k);

    // Paths are known early when the filename doesn't depend on details; use them for the "same dir" MD5 setting
    int fnLevel = m_filename.empty() ? 0 : m_filename.needExactTags(forced, customs);
    bool fnNeedsTags = fnLevel == 2 || (fnLevel == 1 && m_image->hasUnknownTag());
    std::string md5Path;
    if(!fnNeedsTags && !m_filename.empty()) {
        StringList p = m_image->paths(m_filename, m_folder, m_count);
        if(!p.empty()) md5Path = p[0];
    }
    if(m_profile->md5Action(m_image->md5(), md5Path).first == "ignore" && !m_force) {
        loadedSave(Image::LoadResult::Ok);
        return;
    }

    bool needFileUrl = contains(forced, "*") || contains(forced, "file_url");
    bool needUgoira = m_image->extension() == "zip" && s.getBool("Save/ConvertUgoira", false);
    int needTags = std::max(needExactTags(), fnLevel);
    bool filenameNeedTags = needTags == 2 || (needTags == 1 && m_image->hasUnknownTag());
    bool blacklistNeedTags = m_blacklist && !m_blacklist->empty() && m_image->tags().empty();
    if(!blacklistNeedTags && !needFileUrl && !needUgoira && (!m_loadTags || !m_paths.empty() || !filenameNeedTags)) {
        loadedSave(Image::LoadResult::Ok);
        return;
    }
    log("Loading the image details first (blacklist: " + std::to_string(blacklistNeedTags) + ", file url: " + std::to_string(needFileUrl) +
            ", filename tags: " + std::to_string(filenameNeedTags) + ")",
        LogLevel::Info);
    std::weak_ptr<ImageDownloader> weak = weak_from_this();
    m_image->loadDetails([weak](Image::LoadResult r) {
        if(auto self = weak.lock())
            if(self->m_running) self->loadedSave(r);
    });
}

void ImageDownloader::abort() {
    if(!m_running) return;
    m_running = false;
    m_done = nullptr;
    if(m_reply) m_reply->abort();
    m_reply.reset();
    if(!m_temporaryPath.empty() && m_temporaryPath != m_image->savePath(currentSize())) removeFile(m_temporaryPath);
}

void ImageDownloader::finish(const SaveResults& r) {
    m_running = false;
    m_reply.reset();
    auto done = std::move(m_done);
    m_done = nullptr;
    auto keep = shared_from_this();
    if(done) done(m_image, r);
}

SaveResults ImageDownloader::makeResult(const StringList& paths, SaveResult r) const {
    SaveResults out;
    for(auto& p : paths) out.push_back({p, currentSize(), r});
    if(out.empty()) out.push_back({"", currentSize(), r});
    return out;
}

void ImageDownloader::loadedSave(Image::LoadResult result) {
    if(result != Image::LoadResult::Ok) {
        finish(makeResult({""}, SaveResult::DetailsLoadError));
        return;
    }
    if(m_paths.empty()) {
        m_paths = m_image->paths(m_filename, m_folder, m_count);
        if(m_paths.empty()) {
            log("No path could be generated for filename: '" + m_filename.format() + "'", LogLevel::Error);
            finish(makeResult({""}, SaveResult::Error));
            return;
        }
        if(m_filename.needTemporaryFile(m_image->tokens())) {
            std::string dir = !m_folder.empty() && (isDir(m_folder) || mkpath(m_folder)) ? m_folder : m_profile->tempPath();
            m_temporaryPath = buildPath(dir, uuid() + ".tmp");
        }
    }
    if(m_temporaryPath.empty()) m_temporaryPath = m_paths[0] + ".tmp";

    if(m_blacklist) {
        StringList detected = m_blacklist->match(m_image->tokens());
        if(!detected.empty()) {
            log("Image contains blacklisted tags: '" + join(detected, "', '") + "'", LogLevel::Info);
            finish(makeResult(m_paths, SaveResult::Blacklisted));
            return;
        }
    }

    if(!m_force) {
        bool allExist = true;
        for(auto& p : m_paths)
            if(!fileExists(p)) allExist = false;
        if(allExist) {
            log("File already exists: `" + m_paths[0] + "`", LogLevel::Info);
            if(m_addMd5)
                for(auto& p : m_paths) m_profile->addMd5(fileMd5(p), p);
            finish(makeResult(m_paths, SaveResult::AlreadyExistsDisk));
            return;
        }
        // Already loaded (viewer) or duplicate found in the MD5 database
        SaveResult res = m_image->preSave(m_temporaryPath, m_size);
        if(res != SaveResult::NotLoaded && res != SaveResult::AlreadyExistsDisk) {
            SaveResults pre{{m_temporaryPath, m_size, res}};
            if(res == SaveResult::Saved || res == SaveResult::Copied || res == SaveResult::Moved || res == SaveResult::Linked) pre = afterTemporarySave(res);
            if(res == SaveResult::AlreadyExistsMd5 || res == SaveResult::AlreadyExistsDeletedMd5) pre = makeResult(m_paths, res);
            finish(pre);
            return;
        }
    }

    m_url = m_image->url(m_size);
    if(m_url.empty()) {
        log("Image without URL found for '" + m_paths[0] + "'", LogLevel::Warning);
        finish(makeResult(m_paths, SaveResult::NetworkError));
        return;
    }
    log("Loading and saving image from `" + m_url + "` in `" + m_paths[0] + "`", LogLevel::Info);
    loadImage();
}

void ImageDownloader::loadImage(bool rateLimit) {
    Site* site = m_image->site();
    if(!ensureParent(m_temporaryPath)) {
        log("Impossible to create the destination folder: " + dirName(m_temporaryPath), LogLevel::Error);
        finish(makeResult(m_paths, SaveResult::Error));
        return;
    }
    RequestOptions opts;
    opts.pageUrl = m_image->parentUrl();
    opts.ref = "image";
    opts.imagePageUrl = m_image->pageUrl();
    opts.saveTo = m_temporaryPath;
    std::weak_ptr<ImageDownloader> weak = weak_from_this();
    opts.progress = [weak](int64_t received, int64_t total) {
        auto self = weak.lock();
        if(!self) return;
        if(total > 0 && (self->m_image->fileSize() == 0 || self->m_image->fileSize() < total / 2))
            self->m_image->setFileSize(total, self->currentSize());
        self->progress.emit(received, total);
    };
    m_reply = site->get(site->fixUrl(m_url), rateLimit ? QueryType::Retry : QueryType::Img, [weak](const net::Response& r) {
        if(auto self = weak.lock())
            if(self->m_running) self->onResponse(r);
    }, opts);
}

void ImageDownloader::onResponse(const net::Response& r) {
    if(r.code == net::Error::Canceled) return;
    if(r.code == net::Error::NotFound) {
        removeFile(m_temporaryPath);
        Settings& s = m_profile->settings();
        bool sampleFallback = s.getBool("Save/samplefallback", true);
        bool shouldFallback = m_size == SizeType::Full && sampleFallback && !m_image->url(SizeType::Sample).empty() &&
                              m_image->url(SizeType::Sample) != m_image->url(SizeType::Full);
        std::string newExt = m_rotate ? m_image->nextExtension() : "";
        if(!newExt.empty()) {
            m_url = urlSetExtension(m_image->url(m_size), newExt);
            log("Image not found. New try with extension " + newExt + " (" + m_url + ")...", LogLevel::Info);
            m_image->setUrl(m_url);
            loadImage();
        } else if(shouldFallback && !m_tryingSample) {
            m_url = m_image->url(SizeType::Sample);
            m_tryingSample = true;
            log("Image not found. New try with its sample (" + m_url + ")...", LogLevel::Warning);
            loadImage();
        } else {
            log("Image not found: " + m_url, LogLevel::Warning);
            finish(makeResult(m_paths, SaveResult::NotFound));
        }
        return;
    }
    if(!r.ok()) {
        removeFile(m_temporaryPath);
        if(r.code == net::Error::HostNotFound) {
            log("Host not found for the image `" + m_url + "`: " + r.error, LogLevel::Error);
            finish(makeResult(m_paths, SaveResult::Error));
            return;
        }
        if((r.status == 429 || r.status == 503 || r.status == 509) && m_retries < 5) {
            m_retries++;
            log("Rate limit reached (" + std::to_string(r.status) + ") for `" + m_url + "`. New try.", LogLevel::Warning);
            loadImage(true);
            return;
        }
        log("Network error for the image `" + m_url + "`: " + r.error, LogLevel::Error);
        finish(makeResult(m_paths, SaveResult::NetworkError));
        return;
    }
    // Error pages served instead of the file (e.g. Cloudflare redirects to a HTML page)
    std::string ct = toLower(r.header("content-type"));
    if(startsWith(ct, "text/html") && getExtension(m_url) != "html") {
        log("Expected a file but got a HTML page for `" + m_url + "` (final URL `" + r.url + "`)", LogLevel::Warning);
        removeFile(m_temporaryPath);
        finish(makeResult(m_paths, SaveResult::NetworkError));
        return;
    }
    finish(afterTemporarySave(SaveResult::Saved));
}

SaveResults ImageDownloader::afterTemporarySave(SaveResult saveResult) {
    Settings& s = m_profile->settings();
    std::string multipleFiles = s.get("Save/multiple_files", "copy");
    SizeType size = currentSize();
    m_image->setSavePath(m_temporaryPath, size);

    if(!m_image->size(size).hasSize()) {
        int w = 0, h = 0;
        if(gdk_pixbuf_get_file_info(m_temporaryPath.c_str(), &w, &h) && w > 0 && h > 0) m_image->setSize(w, h, size);
    }

    // Optional resizing
    bool maxW = s.getBool("ImageSize/maxWidthEnabled", false), maxH = s.getBool("ImageSize/maxHeightEnabled", false);
    const ImageSize& is = m_image->size(size);
    if(is.hasSize() && (maxW || maxH) && !m_image->isVideo() && m_image->isAnimated().empty()) {
        int w = is.width, h = is.height;
        int limitW = maxW ? (int)s.getInt("ImageSize/maxWidth", 1000) : w;
        int limitH = maxH ? (int)s.getInt("ImageSize/maxHeight", 1000) : h;
        if(w > limitW || h > limitH) {
            double scale = std::min((double)limitW / w, (double)limitH / h);
            GError* err = nullptr;
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(m_temporaryPath.c_str(), std::max(1, (int)(w * scale)),
                                                              std::max(1, (int)(h * scale)), TRUE, &err);
            if(pb) {
                std::string ext = toLower(m_image->extension());
                const char* type = ext == "png" ? "png" : (ext == "webp" ? "webp" : "jpeg");
                if(gdk_pixbuf_save(pb, m_temporaryPath.c_str(), type, nullptr, nullptr)) {
                    m_image->setSize(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb), size);
                    m_image->setFileSize(fileSize(m_temporaryPath), size);
                }
                g_object_unref(pb);
            } else if(err) {
                log("Could not resize `" + m_temporaryPath + "`: " + err->message, LogLevel::Warning);
                g_error_free(err);
            }
        }
    }

    if(!m_filename.empty()) {
        StringList p = m_image->paths(m_filename, m_folder, m_count);
        if(!p.empty()) m_paths = p;
    }

    bool moved = false;
    std::string firstPath;
    SaveResults result;
    for(auto& path : m_paths) {
        if(fileExists(path)) {
            log("File already exists: `" + path + "`", LogLevel::Info);
            if(m_addMd5) m_profile->addMd5(fileMd5(path), path);
            result.push_back({path, size, SaveResult::AlreadyExistsDisk});
            continue;
        }
        if(!ensureParent(path)) {
            log("Impossible to create the destination folder: " + dirName(path), LogLevel::Error);
            result.push_back({path, size, SaveResult::Error});
            continue;
        }
        if(!moved) {
            if(!renameFile(m_temporaryPath, path)) {
                log("Error moving `" + m_temporaryPath + "` to `" + path + "`", LogLevel::Error);
                result.push_back({path, size, SaveResult::Error});
                continue;
            }
            moved = true;
            firstPath = path;
        } else if(multipleFiles == "link") {
            if(!createLink(firstPath, path, "link")) {
                log("Error creating link from `" + firstPath + "` to `" + path + "`", LogLevel::Error);
                result.push_back({path, size, SaveResult::Error});
                continue;
            }
        } else {
            if(!copyFile(firstPath, path)) {
                log("Error copying `" + firstPath + "` to `" + path + "`", LogLevel::Error);
                result.push_back({path, size, SaveResult::Error});
                continue;
            }
        }
        std::string finalPath = path;
        if(m_postSave) finalPath = m_image->postSave(path, size, saveResult, m_addMd5, m_startCommands, m_count);
        result.push_back({finalPath, size, saveResult});
    }
    if(!moved) removeFile(m_temporaryPath);
    if(result.empty()) result = makeResult(m_paths, SaveResult::Error);
    return result;
}

// ---- Queries -------------------------------------------------------------------------

static std::string escapeNewlines(const std::string& s) { return replaceAll(replaceAll(s, "\\n", "\\\\n"), "\n", "\\n"); }
static std::string unescapeNewlines(const std::string& s) {
    std::string out;
    for(size_t i = 0; i < s.size(); i++) {
        if(s[i] == '\\' && i + 1 < s.size()) {
            if(s[i + 1] == 'n') {
                out += '\n';
                i++;
                continue;
            }
            if(s[i + 1] == '\\' && i + 2 < s.size() && s[i + 2] == 'n') {
                out += "\\n";
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

nlohmann::json DownloadQueryGroup::toJson(bool saveProgress) const {
    nlohmann::json j;
    j["query"] = query.toJson();
    j["page"] = page;
    j["perpage"] = perpage;
    j["total"] = total;
    j["postFiltering"] = postFiltering;
    j["getBlacklisted"] = getBlacklisted;
    j["galleriesCountAsOne"] = galleriesCountAsOne;
    j["site"] = site ? site->url() : "";
    j["filename"] = escapeNewlines(filename);
    j["path"] = path;
    if(saveProgress) {
        j["progressVal"] = progressVal;
        j["progressFinished"] = progressFinished;
    }
    return j;
}

bool DownloadQueryGroup::fromJson(const nlohmann::json& j, Profile* profile) {
    if(!j.is_object()) return false;
    query.fromJson(j.contains("query") ? j["query"] : j, profile);
    page = j.value("page", 1);
    perpage = j.value("perpage", 20);
    total = j.value("total", 20);
    getBlacklisted = j.value("getBlacklisted", false);
    galleriesCountAsOne = j.value("galleriesCountAsOne", true);
    filename = unescapeNewlines(j.value("filename", ""));
    path = j.value("path", "");
    progressVal = j.value("progressVal", 0);
    progressFinished = j.value("progressFinished", false);
    postFiltering.clear();
    if(j.contains("postFiltering") && j["postFiltering"].is_array())
        for(auto& p : j["postFiltering"])
            if(p.is_string()) postFiltering.push_back(p.get<std::string>());
    std::string siteName = j.value("site", "");
    site = profile->site(siteName);
    if(!site) {
        log("Unknown site: " + siteName, LogLevel::Warning);
        return false;
    }
    return page >= 1 && perpage >= 1 && (total >= 1 || total == -1);
}

nlohmann::json DownloadQueryImage::toJson() const {
    nlohmann::json j;
    j["site"] = site ? site->url() : "";
    j["filename"] = escapeNewlines(filename);
    j["path"] = path;
    j["image"] = image->write();
    return j;
}

bool DownloadQueryImage::fromJson(const nlohmann::json& j, Profile* profile) {
    if(!j.is_object()) return false;
    site = profile->site(j.value("site", ""));
    if(!site) return false;
    image = std::make_shared<Image>(profile);
    if(!j.contains("image") || !image->read(j["image"])) return false;
    filename = unescapeNewlines(j.value("filename", ""));
    path = j.value("path", "");
    return true;
}

// ---- PackLoader ---------------------------------------------------------------------

PackLoader::PackLoader(Profile* profile, DownloadQueryGroup query, int packSize) : m_profile(profile), m_query(std::move(query)), m_packSize(packSize) {}

static std::string imageKey(const ImagePtr& img) {
    if(img->id()) return img->site()->url() + "#" + std::to_string(img->id());
    if(!img->md5().empty()) return img->md5();
    // Sources without IDs (booru.io) identify their images with an "identity" object or the page URL
    nlohmann::json identity = img->identity(false);
    if(identity.is_object() && !identity.empty()) return img->site()->url() + "#" + identity.dump();
    if(!img->pageUrl().empty()) return img->pageUrl();
    return img->url();
}

void PackLoader::start() {
    int page = m_query.page;
    if(m_query.progressVal > 0) {
        int skip = m_query.progressVal / std::max(1, m_query.perpage);
        page += skip;
        m_total = skip * m_query.perpage;
    }
    std::vector<Site*> sites{m_query.site.get()};
    m_pendingPages.push_back(std::make_shared<Page>(m_profile, m_query.site, sites, m_query.query, page, m_query.perpage, m_query.postFiltering));
}

void PackLoader::abort() {
    m_aborted = true;
    if(m_current) m_current->abort();
    m_done = nullptr;
}

bool PackLoader::hasNext() const {
    return (!m_overflow.empty() || !m_pendingPages.empty() || !m_pendingGalleries.empty()) && (m_total < m_query.total || m_query.total < 0);
}

int PackLoader::nextPackSize() const {
    if(m_query.total < 0) return m_packSize;
    return m_packSize < 0 ? m_query.total - m_total : std::min(m_packSize, m_query.total - m_total);
}

void PackLoader::next(std::function<void(std::vector<ImagePtr>)> done) {
    m_done = std::move(done);
    m_results.clear();
    m_pageCount = 0;
    m_maxPages = m_packSize < 0 ? INT32_MAX : std::max(1, (int)std::ceil((double)m_packSize / std::max(1, m_query.perpage)));
    m_aborted = false;
    if(!m_overflow.empty()) {
        int already = m_total;
        size_t taken = 0;
        while(taken < m_overflow.size() && ((int)m_results.size() < m_packSize || m_packSize < 0) &&
              (already + (int)m_results.size() != m_query.total || (m_overflowGallery && m_query.galleriesCountAsOne))) {
            m_results.push_back(m_overflow[taken++]);
        }
        m_overflow.erase(m_overflow.begin(), m_overflow.begin() + (long)taken);
        if(!m_overflowGallery || !m_query.galleriesCountAsOne) m_total += (int)taken;
        if(m_overflowGallery && m_overflow.empty() && m_query.galleriesCountAsOne && !m_overflowHasNext) m_total++;
    }
    loadNextPage();
}

void PackLoader::finishPack() {
    m_current.reset();
    auto done = std::move(m_done);
    m_done = nullptr;
    if(done) done(std::move(m_results));
}

void PackLoader::loadNextPage() {
    if(m_aborted || !hasNext() || m_pageCount >= m_maxPages || ((int)m_results.size() >= m_packSize && m_packSize >= 0) ||
       (m_pendingPages.empty() && m_pendingGalleries.empty())) {
        finishPack();
        return;
    }
    bool gallery = !m_pendingGalleries.empty();
    PagePtr page = gallery ? m_pendingGalleries.front() : m_pendingPages.front();
    if(gallery)
        m_pendingGalleries.pop_front();
    else
        m_pendingPages.pop_front();
    m_current = page;
    std::weak_ptr<PackLoader> weak = weak_from_this();
    page->load([weak, page, gallery](Page* p, bool ok) {
        auto self = weak.lock();
        if(!self || self->m_aborted) return;
        self->pageLoaded.emit(p);
        int newImages = 0;
        bool reachedTotal = false;
        for(auto& img : p->images()) {
            if(!self->m_seen.insert(imageKey(img)).second) continue;
            newImages++;
            if(img->isGallery()) {
                SearchQuery q;
                q.gallery = img;
                q.tags = p->search();
                std::vector<Site*> sites{self->m_query.site.get()};
                self->m_pendingGalleries.push_back(
                    std::make_shared<Page>(self->m_profile, self->m_query.site, sites, q, 1, self->m_query.perpage, self->m_query.postFiltering));
                continue;
            }
            if((int)self->m_results.size() >= self->m_packSize && self->m_packSize >= 0) {
                self->m_overflow.push_back(img);
                self->m_overflowGallery = gallery;
                self->m_overflowHasNext = p->hasNext();
            } else {
                self->m_results.push_back(img);
                if(!gallery || !self->m_query.galleriesCountAsOne) self->m_total++;
            }
            if(self->m_total == self->m_query.total) {
                reachedTotal = true;
                break;
            }
        }
        // Next page, unless the site keeps returning the same results (or nothing)
        if(ok && p->hasNext() && !reachedTotal && newImages > 0) {
            std::vector<Site*> sites{self->m_query.site.get()};
            auto next = std::make_shared<Page>(self->m_profile, self->m_query.site, sites, p->query(), p->page() + 1, self->m_query.perpage,
                                               self->m_query.postFiltering, false, p->pageInformation());
            if(gallery)
                self->m_pendingGalleries.push_back(next);
            else
                self->m_pendingPages.push_back(next);
        } else if(ok && p->hasNext() && newImages == 0 && !p->images().empty()) {
            log("[" + self->m_query.site->url() + "] Page " + std::to_string(p->page()) + " only returned already seen images, stopping",
                LogLevel::Warning);
        }
        if(gallery && !p->hasNext() && self->m_query.galleriesCountAsOne && self->m_overflow.empty()) self->m_total++;
        if(!gallery) self->m_pageCount++;
        self->loadNextPage();
    });
}

// ---- BatchDownloader ----------------------------------------------------------------

BatchDownloader::BatchDownloader(Profile* profile, std::shared_ptr<DownloadQueryGroup> group) : m_profile(profile), m_group(std::move(group)) {}
BatchDownloader::BatchDownloader(Profile* profile, std::vector<DownloadQueryImage> images) : m_profile(profile), m_images(std::move(images)) {}

void BatchDownloader::start() {
    if(m_step == Step::Aborted) {
        // Resume
        if(!m_pending.empty()) {
            startImages();
            return;
        }
        if(m_packLoader) {
            nextPack();
            return;
        }
    } else if(m_step != Step::NotStarted) {
        return;
    }
    for(auto& c : m_counters) c = 0;
    m_processed = 0;
    m_total = m_group ? m_group->total : (int)m_images.size();
    login();
}

void BatchDownloader::abort() {
    m_step = Step::Aborted;
    for(auto& [_, d] : m_downloaders) d->abort();
    // Unfinished images go back to the queue so start() can resume
    for(auto& [img, d] : m_downloaders) m_pending.push_front(d->image());
    m_downloaders.clear();
    m_active = 0;
    if(m_packLoader) m_packLoader->abort();
}

void BatchDownloader::login() {
    m_step = Step::Login;
    std::shared_ptr<Site> site = m_group ? m_group->site : (m_images.empty() ? nullptr : m_images[0].site);
    std::weak_ptr<BatchDownloader> weak = weak_from_this();
    auto after = [weak](LoginResult) {
        auto self = weak.lock();
        if(!self || self->m_step == Step::Aborted) return;
        if(self->m_group) {
            Settings& s = self->m_profile->settings();
            int packSize = s.getBool("packing_enable", true) ? (int)s.getInt("packing_size", 1000) : -1;
            self->m_packLoader = std::make_shared<PackLoader>(self->m_profile, *self->m_group, packSize);
            self->m_packLoader->start();
            self->nextPack();
        } else {
            for(auto& q : self->m_images) self->m_pending.push_back(q.image);
            self->startImages();
        }
    };
    if(!site) {
        allFinished();
        return;
    }
    site->login(false, after);
}

void BatchDownloader::nextPack() {
    if(!m_packLoader->hasNext()) {
        allFinished();
        return;
    }
    m_step = Step::PageDownload;
    int packSize = m_packLoader->nextPackSize();
    std::weak_ptr<BatchDownloader> weak = weak_from_this();
    m_packLoader->next([weak, packSize](std::vector<ImagePtr> images) {
        auto self = weak.lock();
        if(!self || self->m_step == Step::Aborted) return;
        if(packSize > (int)images.size() && !self->m_packLoader->hasNext()) self->m_counters[Missing] += packSize - (int)images.size();
        for(auto& i : images) self->m_pending.push_back(i);
        if(images.empty() && !self->m_packLoader->hasNext()) {
            self->allFinished();
            return;
        }
        self->startImages();
    });
}

void BatchDownloader::startImages() {
    m_step = Step::ImageDownload;
    int simultaneous = (int)std::clamp(m_profile->settings().getInt("Save/simultaneous", 1), 1LL, 10LL);
    while(m_active < simultaneous && !m_pending.empty()) nextImage();
    if(m_active == 0 && m_pending.empty()) {
        if(m_packLoader && m_packLoader->hasNext())
            nextPack();
        else
            allFinished();
    }
}

std::string BatchDownloader::filenameFor(const ImagePtr& img) const {
    if(m_group) return m_group->filename;
    for(auto& q : m_images)
        if(q.image == img) return q.filename;
    return m_profile->settings().get("Save/filename");
}

std::string BatchDownloader::pathFor(const ImagePtr& img) const {
    if(m_group) return m_group->path;
    for(auto& q : m_images)
        if(q.image == img) return q.path;
    return m_profile->savePath();
}

void BatchDownloader::nextImage() {
    if(m_step != Step::ImageDownload || m_pending.empty()) return;
    ImagePtr img = m_pending.front();
    m_pending.pop_front();
    m_active++;
    auto dl = std::make_shared<ImageDownloader>(m_profile, img, filenameFor(img), expandHome(pathFor(img)), m_processed + m_active, true, false);
    bool getBlacklisted = m_group ? m_group->getBlacklisted : m_getBlacklisted;
    if(!getBlacklisted) dl->setBlacklist(&m_profile->blacklist());
    m_downloaders[img.get()] = dl;
    std::weak_ptr<BatchDownloader> weak = weak_from_this();
    dl->progress.connect([weak, img](int64_t r, int64_t t) {
        if(auto self = weak.lock()) self->imageProgress.emit(img, r, t);
    });
    imageStarted.emit(img);
    dl->save([weak](const ImagePtr& i, const SaveResults& r) {
        if(auto self = weak.lock()) self->imageFinished(i, r);
    });
}

void BatchDownloader::imageFinished(const ImagePtr& img, const SaveResults& results) {
    auto it = m_downloaders.find(img.get());
    if(it == m_downloaders.end()) return;
    auto keep = it->second;
    m_downloaders.erase(it);
    m_active--;
    SaveResult res = results.empty() ? SaveResult::Error : results[0].result;
    switch(res) {
        case SaveResult::NetworkError:
        case SaveResult::Error:
        case SaveResult::DetailsLoadError:
            m_counters[Errors]++;
            m_failed.push_back(img);
            break;
        case SaveResult::NotFound: m_counters[NotFound]++; break;
        case SaveResult::AlreadyExistsDisk: m_counters[AlreadyExists]++; break;
        case SaveResult::Blacklisted:
        case SaveResult::AlreadyExistsMd5:
        case SaveResult::AlreadyExistsDeletedMd5: m_counters[Ignored]++; break;
        default: m_counters[Downloaded]++;
    }
    m_processed++;
    if(m_group) m_group->progressVal = m_group->progressVal + 1;
    imageSaved.emit(img, results);
    progressChanged.emit();
    if(m_step != Step::ImageDownload) return;
    std::weak_ptr<BatchDownloader> weak = weak_from_this();
    defer([weak] {
        if(auto self = weak.lock())
            if(self->m_step == Step::ImageDownload) self->startImages();
    });
}

void BatchDownloader::allFinished() {
    if(m_step == Step::Finished) return;
    if(m_packLoader && m_packLoader->hasNext() && m_step != Step::Aborted) {
        nextPack();
        return;
    }
    m_packLoader.reset();
    m_step = Step::Finished;
    if(m_group) m_group->progressFinished = true;
    progressChanged.emit();
    finished.emit();
}

// ---- DownloadQueue -----------------------------------------------------------------------

DownloadQueue::DownloadQueue(Profile* profile) : m_profile(profile) {}

DownloadQueue::~DownloadQueue() {
    if(m_batch) m_batch->abort();
    save();
}

std::string DownloadQueue::defaultFile() const { return buildPath(paths::dataDir(), "restore.igl"); }

void DownloadQueue::addGroup(const DownloadQueryGroup& g) {
    Group group;
    group.query = std::make_shared<DownloadQueryGroup>(g);
    group.total = g.total;
    m_groups.push_back(group);
    save();
    changed.emit();
}

void DownloadQueue::addUnique(const DownloadQueryImage& i) {
    for(auto& u : m_uniques)
        if(u.query.image == i.image || (u.query.image->url() == i.image->url() && u.status != Unique::Status::Done)) return;
    Unique u;
    u.query = i;
    m_uniques.push_back(u);
    save();
    changed.emit();
}

void DownloadQueue::removeGroup(size_t index) {
    if(index >= m_groups.size()) return;
    if(m_running && (int)index == m_currentGroup) return;
    m_groups.erase(m_groups.begin() + (long)index);
    if(m_currentGroup > (int)index) m_currentGroup--;
    save();
    changed.emit();
}

void DownloadQueue::removeUnique(size_t index) {
    if(index >= m_uniques.size() || m_uniques[index].status == Unique::Status::Running) return;
    m_uniques.erase(m_uniques.begin() + (long)index);
    save();
    changed.emit();
}

void DownloadQueue::clearFinished() {
    for(size_t i = m_groups.size(); i-- > 0;)
        if(m_groups[i].status == Group::Status::Done) m_groups.erase(m_groups.begin() + (long)i);
    m_uniques.erase(std::remove_if(m_uniques.begin(), m_uniques.end(), [](const Unique& u) { return u.status == Unique::Status::Done; }),
                    m_uniques.end());
    save();
    changed.emit();
}

void DownloadQueue::clear() {
    pause();
    m_groups.clear();
    m_uniques.clear();
    save();
    changed.emit();
}

void DownloadQueue::resetGroup(size_t index) {
    if(index >= m_groups.size()) return;
    Group& g = m_groups[index];
    g.status = Group::Status::Pending;
    g.query->progressVal = 0;
    g.query->progressFinished = false;
    g.downloaded = g.errors = g.ignored = g.alreadyExists = g.notFound = g.processed = 0;
    g.message.clear();
    save();
    changed.emit();
}

bool DownloadQueue::load(const std::string& p) {
    std::string path = p.empty() ? defaultFile() : p;
    auto data = readFile(path);
    if(!data) return false;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(*data);
    } catch(...) {
        // Old text format: one group per line, not supported anymore
        log("Could not read the download list " + path, LogLevel::Warning);
        return false;
    }
    if(j.contains("batchs") && j["batchs"].is_array()) {
        for(auto& g : j["batchs"]) {
            DownloadQueryGroup q;
            if(!q.fromJson(g, m_profile)) continue;
            Group group;
            group.query = std::make_shared<DownloadQueryGroup>(q);
            group.total = q.total;
            group.processed = q.progressVal;
            if(q.progressFinished) group.status = Group::Status::Done;
            m_groups.push_back(group);
        }
    }
    if(j.contains("uniques") && j["uniques"].is_array()) {
        for(auto& u : j["uniques"]) {
            DownloadQueryImage q;
            if(!q.fromJson(u, m_profile)) continue;
            Unique unique;
            unique.query = q;
            m_uniques.push_back(unique);
        }
    }
    changed.emit();
    return true;
}

bool DownloadQueue::save(const std::string& p) const {
    nlohmann::json batchs = nlohmann::json::array(), uniques = nlohmann::json::array();
    for(auto& g : m_groups) batchs.push_back(g.query->toJson());
    for(auto& u : m_uniques)
        if(u.status != Unique::Status::Done) uniques.push_back(u.query.toJson());
    return writeFile(p.empty() ? defaultFile() : p, nlohmann::json{{"version", 3}, {"batchs", batchs}, {"uniques", uniques}}.dump(4));
}

void DownloadQueue::start() {
    m_only.clear();
    if(m_running) return;
    m_running = true;
    for(auto& g : m_groups)
        if(g.status == Group::Status::Paused || g.status == Group::Status::Failed) g.status = Group::Status::Pending;
    changed.emit();
    runNext();
}

void DownloadQueue::startGroups(const std::vector<size_t>& indices) {
    if(m_running) return;
    m_only = indices;
    m_running = true;
    for(auto i : indices)
        if(i < m_groups.size() && m_groups[i].status != Group::Status::Done) m_groups[i].status = Group::Status::Pending;
    changed.emit();
    runNext();
}

void DownloadQueue::pause() {
    if(!m_running) return;
    m_running = false;
    if(m_batch) m_batch->abort();
    m_batch.reset();
    if(m_currentGroup >= 0 && m_currentGroup < (int)m_groups.size()) m_groups[(size_t)m_currentGroup].status = Group::Status::Paused;
    for(auto i : m_runningUniques)
        if(i < m_uniques.size()) m_uniques[i].status = Unique::Status::Pending;
    m_runningUniques.clear();
    m_currentGroup = -1;
    save();
    changed.emit();
}

void DownloadQueue::runNext() {
    if(!m_running) return;
    m_batch.reset();
    // Groups one after the other
    for(size_t i = 0; i < m_groups.size(); i++) {
        Group& g = m_groups[i];
        if(g.status != Group::Status::Pending) continue;
        if(!m_only.empty() && std::find(m_only.begin(), m_only.end(), i) == m_only.end()) continue;
        m_currentGroup = (int)i;
        g.status = Group::Status::Running;
        auto batch = std::make_shared<BatchDownloader>(m_profile, g.query);
        m_batch = batch;
        std::shared_ptr<DownloadQueryGroup> query = g.query;
        auto findGroup = [this, query]() -> Group* {
            for(auto& gg : m_groups)
                if(gg.query == query) return &gg;
            return nullptr;
        };
        std::weak_ptr<BatchDownloader> weak = batch;
        batch->progressChanged.connect([this, weak, findGroup] {
            auto b = weak.lock();
            Group* gg = findGroup();
            if(!b || !gg) return;
            gg->downloaded = b->counter(BatchDownloader::Downloaded);
            gg->errors = b->counter(BatchDownloader::Errors);
            gg->ignored = b->counter(BatchDownloader::Ignored);
            gg->alreadyExists = b->counter(BatchDownloader::AlreadyExists);
            gg->notFound = b->counter(BatchDownloader::NotFound);
            gg->processed = gg->query->progressVal;
            progressChanged.emit();
        });
        batch->imageSaved.connect([this](const ImagePtr& img, const SaveResults& r) { imageSaved.emit(img, r); });
        batch->imageProgress.connect([this](const ImagePtr& img, int64_t a, int64_t b) { imageProgress.emit(img, a, b); });
        batch->finished.connect([this, findGroup] {
            Group* gg = findGroup();
            if(gg) {
                gg->status = gg->errors > 0 && gg->downloaded == 0 && gg->alreadyExists == 0 ? Group::Status::Failed : Group::Status::Done;
                gg->message = std::to_string(gg->downloaded) + " downloaded, " + std::to_string(gg->alreadyExists) + " already existed, " +
                              std::to_string(gg->ignored) + " ignored, " + std::to_string(gg->notFound) + " not found, " +
                              std::to_string(gg->errors) + " errors";
                log("Download of \"" + gg->query->query.toString() + "\" finished: " + gg->message, LogLevel::Info);
            }
            m_currentGroup = -1;
            save();
            changed.emit();
            defer([this] { runNext(); });
        });
        changed.emit();
        batch->start();
        return;
    }
    m_currentGroup = -1;
    // Then single images, all in one batch (only when no specific groups were requested)
    if(m_only.empty()) {
        std::vector<DownloadQueryImage> images;
        m_runningUniques.clear();
        for(size_t i = 0; i < m_uniques.size(); i++) {
            if(m_uniques[i].status != Unique::Status::Pending) continue;
            m_uniques[i].status = Unique::Status::Running;
            m_runningUniques.push_back(i);
            images.push_back(m_uniques[i].query);
        }
        if(!images.empty()) {
            // Group by site: a batch logs into one site
            auto batch = std::make_shared<BatchDownloader>(m_profile, images);
            m_batch = batch;
            batch->imageSaved.connect([this](const ImagePtr& img, const SaveResults& r) {
                for(auto& u : m_uniques) {
                    if(u.query.image != img) continue;
                    SaveResult res = r.empty() ? SaveResult::Error : r[0].result;
                    bool failed = res == SaveResult::Error || res == SaveResult::NetworkError || res == SaveResult::DetailsLoadError;
                    u.status = failed ? Unique::Status::Failed : Unique::Status::Done;
                    u.message = saveResultName(res);
                }
                imageSaved.emit(img, r);
                changed.emit();
            });
            batch->imageProgress.connect([this](const ImagePtr& img, int64_t a, int64_t b) { imageProgress.emit(img, a, b); });
            batch->finished.connect([this] {
                m_runningUniques.clear();
                save();
                defer([this] { runNext(); });
            });
            changed.emit();
            batch->start();
            return;
        }
    }
    m_running = false;
    m_only.clear();
    save();
    changed.emit();
    finished.emit();
}

// ---- TagListLoader ---------------------------------------------------------------------

TagListLoader::TagListLoader(std::shared_ptr<Site> site, int minCount) : m_site(std::move(site)), m_minCount(minCount) {}

void TagListLoader::start(std::function<void(bool, int)> done) {
    m_done = std::move(done);
    if(!m_site->tagsApi()) {
        log("[" + m_site->url() + "] This site cannot load tags", LogLevel::Error);
        if(m_done) m_done(false, 0);
        return;
    }
    loadPage();
}

void TagListLoader::abort() {
    if(m_reply) m_reply->abort();
    m_done = nullptr;
}

void TagListLoader::loadPage() {
    Api* api = m_site->tagsApi();
    PageUrl url = api->tagsUrl(m_page, 1000, "count", m_site->info());
    if(!url.error.empty() || url.url.empty()) {
        log("[" + m_site->url() + "] " + url.error, LogLevel::Error);
        if(m_done) m_done(false, m_count);
        return;
    }
    std::weak_ptr<TagListLoader> weak = weak_from_this();
    m_reply = m_site->get(url.url, QueryType::List, [weak, api](const net::Response& r) {
        auto self = weak.lock();
        if(!self) return;
        ParsedTags tags = api->parseTags(r.body, r.status, self->m_site->info());
        bool stop = !r.ok() || !tags.error.empty() || tags.tags.empty();
        int added = 0;
        for(auto& t : tags.tags) {
            if(t.count() < self->m_minCount) {
                stop = true;
                continue;
            }
            self->m_tags.push_back(t);
            added++;
        }
        self->m_count += added;
        self->progress.emit(self->m_page, self->m_count);
        if(stop || added == 0) {
            self->m_site->tagDatabase().setTags(self->m_tags, true);
            log("[" + self->m_site->url() + "] Loaded " + std::to_string(self->m_count) + " tags", LogLevel::Info);
            auto done = std::move(self->m_done);
            if(done) done(r.ok() || self->m_count > 0, self->m_count);
            return;
        }
        self->m_page++;
        self->loadPage();
    });
}

// ---- Source guessing ------------------------------------------------------------------

void guessSource(Profile* profile, const std::string& url, std::function<void(Source*)> done) {
    struct State {
        std::vector<std::pair<Source*, Api*>> candidates;
        size_t index = 0;
        std::string base;
        std::function<void(Source*)> done;
        std::shared_ptr<net::Reply> reply;
    };
    auto st = std::make_shared<State>();
    st->done = std::move(done);
    std::string u = url;
    if(!startsWith(u, "http")) u = "https://" + u;
    while(endsWith(u, "/")) u.pop_back();
    st->base = u;
    for(auto& [_, src] : profile->sources())
        for(auto& api : src->apis())
            if(api->canLoadCheck()) {
                st->candidates.emplace_back(src.get(), api.get());
                break;
            }
    auto next = std::make_shared<std::function<void()>>();
    *next = [st, next] {
        if(st->index >= st->candidates.size()) {
            st->done(nullptr);
            return;
        }
        auto [src, api] = st->candidates[st->index++];
        PageUrl check = api->checkUrl();
        if(!check.error.empty() || check.url.empty()) {
            (*next)();
            return;
        }
        net::Request req;
        req.url = startsWith(check.url, "http") ? check.url : st->base + (startsWith(check.url, "/") ? "" : "/") + check.url;
        Source* source = src;
        Api* a = api;
        st->reply = net::Session::instance().send(req, [st, next, source, a](const net::Response& r) {
            if(a->parseCheck(r.body, r.status)) {
                st->done(source);
                return;
            }
            (*next)();
        });
    };
    (*next)();
}

}  // namespace bg
