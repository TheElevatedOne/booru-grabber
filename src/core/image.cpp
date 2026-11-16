#include "core/image.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "core/external.hpp"
#include "core/logger.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"

namespace bg {

const char* saveResultName(SaveResult r) {
    switch(r) {
        case SaveResult::AlreadyExistsDisk: return "already exists";
        case SaveResult::AlreadyExistsMd5: return "MD5 already exists";
        case SaveResult::AlreadyExistsDeletedMd5: return "MD5 of a deleted file";
        case SaveResult::Blacklisted: return "blacklisted";
        case SaveResult::Moved: return "moved";
        case SaveResult::Copied: return "copied";
        case SaveResult::Shortcut: return "shortcut";
        case SaveResult::Linked: return "linked";
        case SaveResult::Saved: return "saved";
        case SaveResult::Error: return "error";
        case SaveResult::NotLoaded: return "not loaded";
        case SaveResult::NotFound: return "not found";
        case SaveResult::NetworkError: return "network error";
        case SaveResult::DetailsLoadError: return "details error";
    }
    return "";
}

// ---- ImageSize -----------------------------------------------------------------

ImageSize::~ImageSize() {
    if(!m_temporaryPath.empty() && fileExists(m_temporaryPath)) {
        log("Deleting temporary file `" + m_temporaryPath + "`", LogLevel::Debug);
        removeFile(m_temporaryPath);
    }
}

std::string ImageSize::save(const std::string& path) {
    if(!m_temporaryPath.empty() && fileExists(m_temporaryPath)) {
        std::string temp = m_temporaryPath;
        if(renameFile(m_temporaryPath, path)) {
            m_temporaryPath.clear();
        } else if(!copyFile(m_temporaryPath, path)) {
            log("Error copying `" + m_temporaryPath + "` to `" + path + "`", LogLevel::Error);
            return "";
        }
        m_savePath = path;
        return temp;
    }
    if(!m_savePath.empty() && fileExists(m_savePath)) {
        if(!copyFile(m_savePath, path)) return "";
        return m_savePath;
    }
    return "";
}

bool ImageSize::setTemporaryPath(const std::string& path) {
    bool changed = setSavePath(path);
    if(m_temporaryPath == path) return changed;
    if(!m_temporaryPath.empty()) removeFile(m_temporaryPath);
    m_temporaryPath = path;
    if(fileSize <= 0) {
        fileSize = std::max<int64_t>(0, bg::fileSize(path));
        return true;
    }
    return changed;
}

bool ImageSize::setSavePath(const std::string& path) {
    if(path == m_savePath) return false;
    m_savePath = path;
    m_md5.clear();
    if(fileSize <= 0) fileSize = std::max<int64_t>(0, bg::fileSize(path));
    return true;
}

std::string ImageSize::md5() const {
    if(m_md5.empty()) {
        std::string p = !m_savePath.empty() && fileExists(m_savePath) ? m_savePath : m_temporaryPath;
        if(!p.empty()) m_md5 = fileMd5(p);
    }
    return m_md5;
}

nlohmann::json ImageSize::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    if(!url.empty()) j["url"] = url;
    if(fileSize > 0) j["fileSize"] = fileSize;
    if(hasSize()) j["size"] = {{"width", width}, {"height", height}};
    if(!rect.empty()) j["rect"] = {{"left", rect.x}, {"top", rect.y}, {"width", rect.w}, {"height", rect.h}};
    return j;
}

void ImageSize::fromJson(const nlohmann::json& j) {
    if(!j.is_object()) return;
    url = j.value("url", url);
    fileSize = j.value("fileSize", fileSize);
    if(j.contains("size") && j["size"].is_object()) {
        width = j["size"].value("width", 0);
        height = j["size"].value("height", 0);
    }
    if(j.contains("rect") && j["rect"].is_object()) {
        auto& r = j["rect"];
        rect = {r.value("left", 0), r.value("top", 0), r.value("width", 0), r.value("height", 0)};
    }
}

// ---- construction --------------------------------------------------------------------

static std::string ratingName(const std::string& val) {
    static const std::map<std::string, std::string> assoc{{"g", "general"}, {"s", "safe"}, {"q", "questionable"}, {"e", "explicit"}};
    auto it = assoc.find(val);
    return it != assoc.end() ? it->second : toLower(val);
}

// ImageFactory transforms of the original: turn well-known string fields into typed tokens
static void applyTransforms(StringMap& d, std::map<std::string, Var>& data, TagList& tags) {
    auto take = [&](const std::string& key, auto&& fn) {
        auto it = d.find(key);
        if(it != d.end() && !data.count(key) && !it->second.empty()) {
            std::string v = it->second;
            d.erase(it);
            fn(v);
        }
    };
    take("parent_id", [&](const std::string& v) { data["parentid"] = Var(toInt(v)); });
    take("creator_id", [&](const std::string& v) { data["authorid"] = Var(toInt(v)); });
    for(const char* k : {"author", "status", "score"}) take(k, [&, k](const std::string& v) { data[k] = Var(v); });
    take("rating", [&](const std::string& v) { data["rating"] = Var(ratingName(v)); });
    for(const char* k : {"has_children", "has_note", "has_comments"}) take(k, [&, k](const std::string& v) { data[k] = Var(v == "true"); });
    for(const char* type : {"general", "artist", "character", "copyright", "model", "species", "meta"}) {
        take(std::string("tags_") + type, [&, type](const std::string& v) {
            for(auto& t : split(v, ' ', true)) tags.emplace_back(replaceAll(t, "&amp;", "&"), TagType(type));
        });
    }
    take("tags", [&](const std::string& v) {
        if(!tags.empty()) return;
        static const Regex rxSpaces("[\r\n\t]+");
        std::string raw = rxSpaces.replace(v, " ");
        size_t commas = 0, spaces = 0;
        for(size_t i = 0; i < raw.size(); i++) {
            if(raw[i] == ' ') spaces++;
            if(raw.compare(i, 2, ", ") == 0) commas++;
        }
        bool byComma = commas >= 10 || (commas > 0 && (spaces - commas) / commas < 2);
        StringList list = byComma ? split(raw, ", ", true) : split(raw, ' ', true);
        for(auto tg : list) {
            tg = replaceAll(tg, "&amp;", "&");
            size_t colon = tg.find(':');
            if(colon != std::string::npos) {
                std::string tp = toLower(tg.substr(0, colon));
                if(tp == "user") {
                    data["author"] = Var(tg.substr(colon + 1));
                    continue;
                }
                if(tp == "score") {
                    data["score"] = Var(tg.substr(colon + 1));
                    continue;
                }
                if(tp == "size") continue;
                if(tp == "rating") {
                    data["rating"] = Var(ratingName(tg.substr(colon + 1)));
                    continue;
                }
            }
            tags.emplace_back(tg);
        }
    });
    take("created_at", [&](const std::string& v) {
        data["date"] = Var(DateTime::parse(v));
        data["date_raw"] = Var(v);
    });
    take("date", [&](const std::string& v) {
        data["date"] = Var(DateTime::fromIso(v, false));
        data["date_raw"] = Var(v);
    });
}

static bool inRange(int w, int h, int lo, int hi) { return std::max(w, h) >= lo && std::max(w, h) <= hi; }
// Same rules as Grabber's isBigger(): sources often give only a width (or only a height) for their medias
static bool bigger(int w1, int h1, int w2, int h2) {
    if(w1 > 0 && h1 > 0 && w2 <= 0 && h2 <= 0) return true;  // unknown sizes are the smallest
    if(w1 > 0 && h1 > 0 && w2 > 0 && h2 > 0) return w1 > w2 && h1 > h2;
    if(w1 > 0 && w2 > 0) return w1 > w2;
    if(h1 > 0 && h2 > 0) return h1 > h2;
    return false;
}

Image::Image(Profile* profile) : m_profile(profile) {
    for(SizeType t : {SizeType::Full, SizeType::Sample, SizeType::Thumbnail}) {
        auto s = std::make_shared<ImageSize>();
        s->type = t;
        m_sizes[t] = s;
        m_allSizes.push_back(s);
    }
}

Image::Image(std::shared_ptr<Site> site, const RawImage& raw, Profile* profile, StringList search, std::string parentUrl)
    : Image(profile) {
    m_site = std::move(site);
    m_parentUrl = std::move(parentUrl);
    StringMap d = raw.d;
    m_data = raw.data;
    m_identity = raw.identity;
    TagList tags = raw.tags;
    applyTransforms(d, m_data, tags);
    auto get = [&](const std::string& k) {
        auto it = d.find(k);
        return it == d.end() ? std::string() : it->second;
    };

    m_isGallery = get("type") == "gallery";
    m_md5 = get("md5");
    m_name = get("name");
    m_search = !search.empty() ? search : split(get("search"), ' ', true);
    m_id = toUInt(get("id"), 0);
    if(d.count("sources"))
        m_sources = split(d["sources"], '\n', true);
    else if(d.count("source") && !d["source"].empty())
        m_sources = {d["source"]};
    m_galleryCount = d.count("gallery_count") ? (int)toInt(d["gallery_count"], -1) : -1;
    m_position = (int)toInt(get("position"));

    static const std::pair<SizeType, const char*> prefixes[] = {{SizeType::Full, ""}, {SizeType::Sample, "sample_"}, {SizeType::Thumbnail, "preview_"}};
    for(auto& [type, prefix] : prefixes) {
        ImageSize& is = *m_sizes[type];
        std::string pre = prefix;
        std::string urlKey = (pre.empty() ? "file_" : pre) + "url";
        if(d.count(urlKey) && !d[urlKey].empty()) is.url = removeCacheBuster(m_site->fixUrl(d[urlKey]));
        int w = (int)toInt(get(pre + "width")), h = (int)toInt(get(pre + "height"));
        if(w > 0 && h > 0) {
            is.width = w;
            is.height = h;
        }
        is.fileSize = toInt(get(pre + "file_size"));
        std::string rect = get(pre + "rect");
        if(!rect.empty()) {
            StringList r = split(rect, ';');
            if(r.size() == 4)
                is.rect = {(int)toInt(r[0]), (int)toInt(r[1]), (int)toInt(r[2]), (int)toInt(r[3])};
            else
                log("Invalid number of values for image rectangle", LogLevel::Error);
        }
    }

    // Medias: typed ones are trusted, untyped ones are sorted into thumbnail/sample/full by size
    std::vector<std::shared_ptr<ImageSize>> medias;
    for(auto& m : raw.medias) {
        auto media = std::make_shared<ImageSize>();
        media->type = m.type;
        media->url = m.url;
        media->width = m.width;
        media->height = m.height;
        media->fileSize = m.fileSize;
        media->bitRate = m.bitRate;
        media->rect = m.rect;
        m_allSizes.push_back(media);
        medias.push_back(media);
        if(m.type != SizeType::Unknown) {
            m_sizes[m.type] = media;
            continue;
        }
        ImageSize& thumb = *m_sizes[SizeType::Thumbnail];
        if(!thumb.hasSize() || (inRange(m.width, m.height, 150, 300) &&
                                (bigger(m.width, m.height, thumb.width, thumb.height) || !inRange(thumb.width, thumb.height, 150, 300))))
            m_sizes[SizeType::Thumbnail] = media;
        ImageSize& sample = *m_sizes[SizeType::Sample];
        if(inRange(m.width, m.height, 500, 1500) && bigger(m.width, m.height, sample.width, sample.height)) m_sizes[SizeType::Sample] = media;
        ImageSize& full = *m_sizes[SizeType::Full];
        if(bigger(m.width, m.height, full.width, full.height)) m_sizes[SizeType::Full] = media;
    }
    // When no media is bigger than the announced size, the biggest one is still the best file there is
    // (only medias: the thumbnail must not become the file, galleries load the file from the details page)
    if(m_sizes[SizeType::Full]->url.empty() && !medias.empty()) {
        std::shared_ptr<ImageSize> best;
        for(auto& m : medias)
            if(!m->url.empty() && (!best || bigger(m->width, m->height, best->width, best->height))) best = m;
        if(best) m_sizes[SizeType::Full] = best;
    }

    if(d.count("page_url")) m_pageUrl = m_site->fixUrl(m_site->fixLoginUrl(d["page_url"]));
    m_tags = tags;

    // Complete unknown tag types from the site's tag database
    StringList unknown;
    for(auto& t : m_tags)
        if(t.type().isUnknown()) unknown.push_back(t.text());
    if(!unknown.empty()) {
        auto types = m_site->tagDatabase().getTagTypes(unknown);
        for(auto& t : m_tags) {
            auto it = types.find(t.text());
            if(it != types.end()) t.setType(it->second);
        }
    }

    // File URL and extension guessing
    std::string url = m_sizes[SizeType::Full]->url;
    std::string ext = getExtension(url);
    if(d.count("ext") && !d["ext"].empty()) {
        if(ext != d["ext"]) {
            m_sizes[SizeType::Full]->url = urlSetExtension(url, d["ext"]);
            m_extension = d["ext"];
        }
    } else if(ext == "jpg" && !this->url(SizeType::Thumbnail).empty()) {
        bool fixed = false;
        std::string previewExt = getExtension(this->url(SizeType::Thumbnail));
        if(!this->url(SizeType::Sample).empty()) {
            std::string sampleExt = getExtension(this->url(SizeType::Sample));
            if(sampleExt != "jpg" && sampleExt != "png" && sampleExt != ext && previewExt == ext) {
                m_sizes[SizeType::Full]->url = urlSetExtension(url, sampleExt);
                fixed = true;
            }
        }
        if(!fixed) {
            if((hasTag("swf") || hasTag("flash")) && ext != "swf")
                setFileExtension("swf");
            else if((hasTag("gif") || hasTag("animated_gif")) && ext != "webm" && ext != "mp4")
                setFileExtension("gif");
            else if(hasTag("mp4") && ext != "gif" && ext != "webm")
                setFileExtension("mp4");
            else if(hasTag("animated_png") && ext != "webm" && ext != "mp4")
                setFileExtension("png");
            else if((hasTag("webm") || hasTag("animated")) && ext != "gif" && ext != "mp4")
                setFileExtension("webm");
        }
    } else if(contains(get("image"), "MB // gif\" height=\"") && ext != "gif") {
        m_sizes[SizeType::Full]->url = urlSetExtension(url, "gif");
    } else if(ext == "webm" && hasTag("mp4")) {
        m_sizes[SizeType::Full]->url = urlSetExtension(url, "mp4");
    }
    m_sizes[SizeType::Full]->url = removeCacheBuster(m_sizes[SizeType::Full]->url);
    init();
}

void Image::init() {
    if(m_site) {
        if(m_pageUrl.empty()) {
            if(Api* api = m_site->detailsApi()) m_pageUrl = api->detailsUrl(m_id, m_md5, m_identity, m_site->info()).url;
        }
        m_pageUrl = m_site->fixUrl(m_pageUrl);
    }
    static const StringList defStatic{"jpg", "png", "gif", "jpeg", "webm", "swf", "mp4"};
    static const StringList defAnimated{"mp4", "webm", "gif", "jpg", "png", "jpeg", "swf"};
    bool animated = hasTag("gif") || hasTag("animated_gif") || hasTag("mp4") || hasTag("animated_png") || hasTag("webm") ||
                    hasTag("animated") || hasTag("video");
    if(m_profile) {
        m_extensions = animated ? m_profile->settings().getList("extensionRotationAnimated", defAnimated)
                                : m_profile->settings().getList("extensionRotationStatic", defStatic);
    }
    m_extensionIndex = 0;
}

std::string Image::nextExtension() {
    // Rotate through the configured extensions, skipping the current one
    std::string current = getExtension(url());
    while(m_extensionIndex < m_extensions.size()) {
        std::string next = trim(m_extensions[m_extensionIndex++]);
        if(!next.empty() && next != current) return next;
    }
    return "";
}

// ---- serialization ---------------------------------------------------------------------

static const std::pair<SizeType, const char*> kSizeNames[] = {{SizeType::Full, "full"}, {SizeType::Sample, "sample"}, {SizeType::Thumbnail, "thumbnail"}};

nlohmann::json Image::write() const {
    nlohmann::json j;
    if(m_site) j["website"] = m_site->url();
    if(m_parentGallery) j["gallery"] = m_parentGallery->write();
    nlohmann::json sizes = nlohmann::json::object();
    for(auto& [t, name] : kSizeNames) {
        auto js = size(t).toJson();
        if(!js.empty()) sizes[name] = js;
    }
    if(!sizes.empty()) j["sizes"] = sizes;
    nlohmann::json tags = nlohmann::json::array();
    for(auto& t : m_tags) tags.push_back(t.toJson());
    j["name"] = m_name;
    j["id"] = std::to_string(m_id);
    j["md5"] = m_md5;
    j["tags"] = tags;
    j["url"] = url();
    j["search"] = m_search;
    if(!m_pageUrl.empty()) j["page_url"] = m_pageUrl;
    if(!m_sources.empty()) j["sources"] = m_sources;
    nlohmann::json data = nlohmann::json::object();
    for(auto& [k, v] : m_data) {
        nlohmann::json jv = v.toJson();
        if(!jv.is_null()) data[k] = jv;
    }
    if(!data.empty()) j["data"] = data;
    if(!m_identity.empty()) j["identity"] = m_identity;
    if(m_isGallery) j["type"] = "gallery";
    return j;
}

bool Image::read(const nlohmann::json& j) {
    if(!j.is_object() || !m_profile) return false;
    std::string website = j.value("website", "");
    m_site = m_profile->site(website);
    if(!m_site) {
        log("Unknown site: " + website, LogLevel::Warning);
        return false;
    }
    if(j.contains("gallery") && j["gallery"].is_object()) {
        auto gallery = std::make_shared<Image>(m_profile);
        if(!gallery->read(j["gallery"])) return false;
        m_parentGallery = gallery;
    }
    if(j.contains("sizes") && j["sizes"].is_object())
        for(auto& [t, name] : kSizeNames)
            if(j["sizes"].contains(name)) m_sizes[t]->fromJson(j["sizes"][name]);
    if(j.contains("tags") && j["tags"].is_array())
        for(auto& t : j["tags"]) m_tags.push_back(Tag::fromJson(t));
    if(j.contains("search") && j["search"].is_array())
        for(auto& s : j["search"])
            if(s.is_string()) m_search.push_back(s.get<std::string>());
    if(j.contains("sources") && j["sources"].is_array())
        for(auto& s : j["sources"])
            if(s.is_string()) m_sources.push_back(s.get<std::string>());
    m_name = j.value("name", "");
    m_id = toUInt(j.value("id", "0"), 0);
    m_md5 = j.value("md5", "");
    m_pageUrl = j.value("page_url", "");
    m_isGallery = j.value("type", "") == "gallery";
    if(j.contains("data") && j["data"].is_object())
        for(auto it = j["data"].begin(); it != j["data"].end(); ++it) m_data[it.key()] = Var::fromJson(it.value());
    if(j.contains("identity") && j["identity"].is_object()) m_identity = j["identity"];
    std::string u = j.contains("file_url") ? j.value("file_url", "") : j.value("url", "");
    if(!u.empty()) {
        if(m_sizes[SizeType::Full]->url.empty()) m_sizes[SizeType::Full]->url = u;
        if(u != m_sizes[SizeType::Full]->url) m_sizes[SizeType::Full]->url = u;
    }
    init();
    return true;
}

// ---- accessors -----------------------------------------------------------------------------

std::string Image::md5() const { return m_md5.empty() ? m_sizes.at(SizeType::Full)->md5() : m_md5; }

StringList Image::tagNames(bool namespaces) const {
    StringList out;
    for(auto& t : m_tags) out.push_back((namespaces && !t.type().isUnknown() ? t.type().name() + ":" : "") + t.text());
    return out;
}

nlohmann::json Image::identity(bool withId) const {
    if(m_identity.empty() && withId) return {{"id", std::to_string(m_id)}};
    return m_identity;
}

std::string Image::url(SizeType s) const { return size(s).url; }

const ImageSize& Image::size(SizeType s) const {
    auto it = m_sizes.find(s == SizeType::Unknown ? SizeType::Full : s);
    return *it->second;
}
ImageSize& Image::size(SizeType s) { return *m_sizes[s == SizeType::Unknown ? SizeType::Full : s]; }

DateTime Image::createdAt() const {
    auto it = m_data.find("date");
    return it != m_data.end() && it->second.type() == Var::Date ? it->second.toDate() : DateTime();
}

std::string Image::dateRaw() const {
    auto it = m_data.find("date_raw");
    return it != m_data.end() ? it->second.toString() : "";
}

std::string Image::extension() const {
    std::string e = toLower(getExtension(url()));
    return e.empty() ? m_extension : e;
}

bool Image::isVideo() const {
    std::string e = toLower(getExtension(url()));
    return e == "mp4" || e == "webm" || e == "mkv" || e == "mov";
}

std::string Image::isAnimated() const {
    std::string e = toLower(getExtension(url()));
    if(e == "gif" || e == "apng") return e;
    if(e == "png" && (hasTag("animated") || hasTag("animated_png"))) return "apng";
    return "";
}

bool Image::hasTag(const std::string& tag) const {
    std::string t = trim(tag);
    for(auto& x : m_tags)
        if(iequals(x.text(), t)) return true;
    return false;
}

bool Image::hasUnknownTag() const {
    if(m_tags.empty()) return true;
    for(auto& t : m_tags)
        if(t.type().isUnknown()) return true;
    return false;
}

int Image::value() const {
    const ImageSize& full = size(SizeType::Full);
    if(full.hasSize()) return full.width * full.height;
    if(hasTag("incredibly_absurdres")) return 10000 * 10000;
    if(hasTag("absurdres")) return 3200 * 2400;
    if(hasTag("highres")) return 1600 * 1200;
    if(hasTag("lowres")) return 500 * 500;
    return 1200 * 900;
}

SizeType Image::preferredDisplaySize() const {
    bool originals = m_profile->settings().getBool("Save/downloadoriginals", true);
    bool viewSample = m_profile->settings().getBool("Viewer/viewSamples", false);
    bool isZip = getExtension(url(SizeType::Full)) == "zip";
    return !url(SizeType::Sample).empty() && (!originals || viewSample || isZip) ? SizeType::Sample : SizeType::Full;
}

const ImageSize& Image::mediaForSize(int w, int h, bool thumbnail) const {
    const ImageSize* best = nullptr;
    std::string thumbExt = getExtension(url(SizeType::Thumbnail));
    for(auto& m : m_allSizes) {
        if(!m->hasSize() || m->width > w || m->height > h || m->url.empty()) continue;
        if(best && !bigger(m->width, m->height, best->width, best->height)) continue;
        if(thumbnail && getExtension(m->url) != thumbExt) continue;
        best = m.get();
    }
    return best ? *best : size(SizeType::Thumbnail);
}

std::vector<std::pair<std::string, int>> Image::ugoiraFrames() const {
    std::vector<std::pair<std::string, int>> out;
    auto it = m_data.find("ugoira_metadata");
    if(it == m_data.end()) return out;
    const nlohmann::json& meta = it->second.json();
    if(!meta.is_object() || !meta.contains("frames") || !meta["frames"].is_array()) return out;
    for(auto& f : meta["frames"]) {
        if(!f.is_object()) continue;
        std::string file = f.contains("file") && f["file"].is_string() ? f["file"].get<std::string>() : "";
        int delay = f.contains("delay") && f["delay"].is_number() ? f["delay"].get<int>() : 100;
        out.emplace_back(file, delay);
    }
    return out;
}

// ---- mutators ------------------------------------------------------------------------------

void Image::setUrl(const std::string& u) {
    std::string before = url();
    m_sizes[SizeType::Full]->fileSize = 0;
    m_sizes[SizeType::Full]->url = u;
    refreshTokens();
    urlChanged.emit(before, u);
}

void Image::setSize(int w, int h, SizeType s) {
    size(s).width = w;
    size(s).height = h;
    refreshTokens();
}

void Image::setFileSize(long long fs, SizeType s) {
    size(s).fileSize = fs;
    refreshTokens();
}

void Image::setFileExtension(const std::string& ext) {
    m_sizes[SizeType::Full]->url = urlSetExtension(m_sizes[SizeType::Full]->url, ext);
    refreshTokens();
}

void Image::setTags(const TagList& tags) {
    m_tags = tags;
    refreshTokens();
}

void Image::setParentGallery(std::shared_ptr<Image> gallery) {
    m_parentGallery = std::move(gallery);
    if(m_search.empty() && m_parentGallery) m_search = m_parentGallery->search();
    refreshTokens();
}

void Image::setTemporaryPath(const std::string& path, SizeType s) {
    if(size(s).setTemporaryPath(path)) refreshTokens();
}

void Image::setSavePath(const std::string& path, SizeType s) {
    if(size(s).setSavePath(path)) refreshTokens();
}

// ---- tokens ------------------------------------------------------------------------------

const TokenMap& Image::tokens() const {
    if(!m_tokens) m_tokens = std::make_unique<TokenMap>(generateTokens());
    return *m_tokens;
}

TokenMap Image::generateTokens() const {
    const Settings& settings = m_profile->settings();
    TokenMap t;
    std::map<std::string, StringList> details;

    static const Regex poolRx("pool:(\\d+)");
    auto pool = poolRx.match(join(m_search, " "));
    t["pool"] = Token(Var(pool ? pool->groups[1] : std::string()));

    std::string fileName = urlFileName(url());
    size_t dot = fileName.rfind('.');
    t["filename"] = Token(Var(urlDecode(dot == std::string::npos ? std::string() : fileName.substr(0, dot))));
    t["website"] = Token(Var(m_site ? m_site->url() : std::string()));
    t["websitename"] = Token(Var(m_site ? m_site->name() : std::string()));
    t["md5"] = Token(Var(md5()));
    const Image* self = this;
    t["md5_forced"] = Token([self]() { return Var(self->size(SizeType::Full).md5()); });
    t["id"] = Token(Var((long long)m_id));
    t["height"] = Token(Var(height()));
    t["width"] = Token(Var(width()));
    t["mpixels"] = Token(Var((long long)width() * height()));
    t["ratio"] = Token(Var(width() == height() ? 1.0 : (height() > 0 ? (double)width() / height() : 0.0)));
    t["url_file"] = Token(Var(url()));
    t["url_original"] = Token(Var(url(SizeType::Full)));
    t["url_sample"] = Token(Var(url(SizeType::Sample)));
    t["url_thumbnail"] = Token(Var(url(SizeType::Thumbnail)));
    t["url_page"] = Token(Var(m_pageUrl));
    t["source"] = Token(Var(m_sources.empty() ? std::string() : m_sources[0]));
    t["sources"] = Token(Var(m_sources));
    t["filesize"] = Token(Var(fileSize()));
    t["name"] = Token(Var(m_name));
    t["position"] = m_position > 0 ? Token(Var(m_position)) : Token(Var(std::string()));

    for(size_t i = 0; i < 10 || i < m_search.size(); i++)
        t["search_" + std::to_string(i + 1)] = Token(Var(i < m_search.size() ? m_search[i] : std::string()));
    t["search"] = Token(Var(join(m_search, " ")));

    for(auto& tag : m_tags) details["allos"].push_back(replaceAll(tag.text(), " ", "_"));
    TagList tags = m_profile->removedTags().filterTags(m_tags);
    const StringList& ignored = m_profile->ignored();
    for(auto& tag : tags) {
        const std::string& txt = tag.text();
        details[containsCi(ignored, txt) ? "general" : tag.type().name()].push_back(txt);
        details["alls"].push_back(txt);
        details["alls_namespaces"].push_back(tag.type().name());
    }

    if(settings.getBool("Save/copyright_useshorter", true)) {
        StringList copyrights;
        for(auto& cop : details["copyright"]) {
            bool found = false;
            for(auto& c : copyrights) {
                if(c.compare(0, cop.size(), cop.substr(0, c.size())) == 0 || cop.compare(0, c.size(), c.substr(0, cop.size())) == 0) {
                    if(cop.size() < c.size()) c = cop;
                    found = true;
                }
            }
            if(!found) copyrights.push_back(cop);
        }
        details["copyright"] = copyrights;
    }

    t["general"] = Token(Var(details["general"]), "keepAll", "", "");
    t["artist"] = Token(Var(details["artist"]), "keepAll", "anonymous", "multiple artists");
    t["contributor"] = Token(Var(details["contributor"]), "keepAll", "none", "multiple");
    t["copyright"] = Token(Var(details["copyright"]), "keepAll", "misc", "crossover");
    t["character"] = Token(Var(details["character"]), "keepAll", "unknown", "group");
    StringList models = details["model"];
    models.insert(models.end(), details["idol"].begin(), details["idol"].end());
    t["model"] = Token(Var(models), "keepAll", "unknown", "multiple");
    t["photo_set"] = Token(Var(details["photo_set"]), "keepAll", "unknown", "multiple");
    t["species"] = Token(Var(details["species"]), "keepAll", "unknown", "multiple");
    t["meta"] = Token(Var(details["meta"]), "keepAll", "none", "multiple");
    t["lore"] = Token(Var(details["lore"]), "keepAll", "none", "multiple");
    t["allos"] = Token(Var(details["allos"]));
    t["allo"] = Token(Var(join(details["allos"], " ")));
    t["tags"] = Token(Var(tags));
    t["all"] = Token(Var(details["alls"]));
    t["all_namespaces"] = Token(Var(details["alls_namespaces"]));

    std::string ext = extension();
    if(settings.getBool("Save/noJpeg", true) && ext == "jpeg") ext = "jpg";
    t["ext"] = Token(Var(ext), Var("jpg"));
    t["filetype"] = Token(Var(ext), Var("jpg"));

    if(m_parentGallery) {
        std::shared_ptr<Image> gallery = m_parentGallery;
        t["gallery"] = Token([gallery]() { return Var(gallery->tokens()); });
    }

    for(auto& [k, v] : m_data) t[k] = k == "rating" ? Token(v, Var("unknown")) : Token(v);
    if(!t.count("rating")) t["rating"] = Token(Var("unknown"));

    // Custom tokens ("Save/Customs/<name>" lists of tags)
    for(auto& [key, list] : getCustoms(&settings)) {
        StringList matched;
        for(auto& tag : tags)
            if(containsCi(list, tag.text())) matched.push_back(tag.text());
        t[key] = Token(Var(matched));
    }

    // Grabber meta-tags, expensive so lazy
    Profile* profile = m_profile;
    TokenMap snapshot = t;
    t["grabber"] = Token([profile, snapshot]() {
        Settings& s = profile->settings();
        Filename filename(s.get("Save/filename"));
        StringList paths = filename.path(snapshot, &s, profile->savePath(), 0, Filename::Full);
        bool alreadyExists = false;
        for(auto& p : paths)
            if(fileExists(p)) alreadyExists = true;
        bool inMd5 = !profile->md5Exists(tokenValue(snapshot, "md5").toString()).empty();
        StringList metas;
        if(alreadyExists) metas.push_back("alreadyExists");
        if(inMd5) metas.push_back("inMd5List");
        if(alreadyExists || inMd5) metas.push_back("downloaded");
        std::set<std::string> names;
        auto tagsIt = snapshot.find("tags");
        if(tagsIt != snapshot.end())
            for(auto& tag : tagsIt->second.value().tags()) names.insert(tag.text());
        for(auto& fav : profile->favorites())
            if(names.count(fav.name)) {
                metas.push_back("favorited");
                break;
            }
        for(auto& mon : profile->monitors())
            if(names.count(mon.query.toString())) {
                metas.push_back("monitored");
                break;
            }
        return Var(metas);
    });
    t["grabber_branch"] = Token(Var("release"));
    t["grabber_version"] = Token(Var(BG_VERSION));
    return t;
}

// ---- details -----------------------------------------------------------------------------

void Image::loadDetails(std::function<void(LoadResult)> done, bool rateLimit) {
    if(done) m_detailsWaiters.push_back(std::move(done));
    if(m_detailsLoading && !rateLimit) return;
    if(m_detailsLoaded || m_pageUrl.empty() || !m_site) {
        finishDetails(LoadResult::Ok);
        return;
    }
    log("Loading image details from `" + m_pageUrl + "`", LogLevel::Info);
    m_detailsLoading = true;
    std::weak_ptr<Image> weak = weak_from_this();
    if(m_detailsReply) m_detailsReply->abort();
    m_detailsReply = m_site->get(m_pageUrl, rateLimit ? QueryType::Retry : QueryType::Details, [weak](const net::Response& r) {
        if(auto self = weak.lock()) self->parseDetails(r);
    });
}

void Image::abortDetails() {
    if(m_detailsReply) m_detailsReply->abort();
    m_detailsReply.reset();
    m_detailsLoading = false;
    m_detailsWaiters.clear();
}

void Image::finishDetails(LoadResult r) {
    m_detailsLoading = false;
    m_detailsReply.reset();
    auto waiters = std::move(m_detailsWaiters);
    m_detailsWaiters.clear();
    for(auto& w : waiters) w(r);
}

void Image::parseDetails(const net::Response& r) {
    if((r.status == 429 || r.status == 503 || r.status == 509) && !r.isCloudflare() && m_detailsRetries < 3) {
        m_detailsRetries++;
        log("Details rate limit reached (HTTP " + std::to_string(r.status) + "), retrying", LogLevel::Warning);
        loadDetails(nullptr, true);
        return;
    }
    if(r.isCloudflare()) {
        log("Cloudflare wall for '" + m_pageUrl + "'", LogLevel::Error);
        finishDetails(LoadResult::CloudflareError);
        return;
    }
    Api* api = m_site->detailsApi();
    if(!api) {
        finishDetails(LoadResult::Error);
        return;
    }
    if(!r.ok() && !(api->parseDetailsErrors() && !r.body.empty())) {
        if(r.code != net::Error::Canceled) log("Loading details error for '" + m_pageUrl + "': " + r.error, LogLevel::Error);
        finishDetails(LoadResult::NetworkError);
        return;
    }
    SiteInfo info = m_site->info();
    ParsedDetails ret = api->parseDetails(r.body, r.status, info);
    if(!ret.error.empty()) {
        log("[" + m_site->url() + "][" + api->name() + "] " + ret.error, m_detailsWarnings ? LogLevel::Warning : LogLevel::Error);
        finishDetails(LoadResult::Error);
        return;
    }
    if(ret.image) {
        Image full(m_site, *ret.image, m_profile);
        ret.tags = full.tags();
        ret.imageUrl = full.url();
        ret.createdAt = full.createdAt();
        if(!full.sources().empty()) ret.sources = full.sources();
    }
    if(!ret.pools.empty()) m_pools = ret.pools;
    if(!ret.tags.empty()) {
        m_tags = ret.tags;
        m_profile->addAutoComplete(ret.tags);
    }
    if(ret.createdAt.valid()) m_data["date"] = Var(ret.createdAt);
    if(!ret.sources.empty()) m_sources = ret.sources;
    if(!ret.imageUrl.empty()) {
        std::string before = url();
        std::string newUrl = m_site->fixUrl(ret.imageUrl, before);
        m_sizes[SizeType::Full]->url = newUrl;
        if(before != newUrl) {
            if(getExtension(before) != getExtension(newUrl)) m_sizes[SizeType::Full]->fileSize = 0;
            urlChanged.emit(before, newUrl);
        }
    }
    m_detailsLoaded = true;
    refreshTokens();

    // Ugoira metadata, needed to convert the zip later
    if(extension() == "zip" && m_profile->settings().getBool("Save/ConvertUgoira", false)) {
        if(const ApiEndpoint* ep = m_site->endpoint("ugoira_details")) {
            std::string u = ep->url(m_identity, 1, 1, {}, info).url;
            log("Loading ugoira details from `" + u + "`", LogLevel::Info);
            std::weak_ptr<Image> weak = weak_from_this();
            m_detailsReply = m_site->get(u, QueryType::Details, [weak, ep](const net::Response& rr) {
                auto self = weak.lock();
                if(!self) return;
                if(!rr.ok()) {
                    log("Loading ugoira details error: " + rr.error, LogLevel::Error);
                    self->finishDetails(LoadResult::NetworkError);
                    return;
                }
                self->m_data["ugoira_metadata"] = Var::fromJson(ep->parseAny(rr.body, rr.status));
                self->finishDetails(LoadResult::Ok);
            });
            return;
        }
    }
    finishDetails(LoadResult::Ok);
}

// ---- saving --------------------------------------------------------------------------------

StringList Image::paths(const Filename& filename, const std::string& folder, int count) const {
    return filename.path(tokens(), &m_profile->settings(), folder, count, Filename::Complex | Filename::Path);
}

SaveResult Image::preSave(const std::string& path, SizeType sz) {
    if(fileExists(path)) return SaveResult::AlreadyExistsDisk;
    auto action = sz != SizeType::Thumbnail ? m_profile->md5Action(md5(), path) : std::make_pair(std::string("save"), std::string());
    const std::string& what = action.first;
    const std::string& dup = action.second;
    if(what == "ignore") {
        if(!fileExists(dup)) {
            log("MD5 \"" + md5() + "\" of `" + url() + "` already found in non-existing file `" + dup + "`", LogLevel::Info);
            return SaveResult::AlreadyExistsDeletedMd5;
        }
        log("MD5 \"" + md5() + "\" of `" + url() + "` already found in `" + dup + "`", LogLevel::Info);
        return SaveResult::AlreadyExistsMd5;
    }
    if(!ensureParent(path)) {
        log("Impossible to create the destination folder: " + dirName(path), LogLevel::Error);
        return SaveResult::Error;
    }
    if(what == "save") {
        std::string from = size(sz).save(path);
        if(from.empty()) return SaveResult::NotLoaded;
        log("Saving image in `" + path + "` (from `" + from + "`)", LogLevel::Info);
        return SaveResult::Saved;
    }
    if(what == "copy") {
        log("Copy from `" + dup + "` to `" + path + "`", LogLevel::Info);
        return copyFile(dup, path) ? SaveResult::Copied : SaveResult::Error;
    }
    if(what == "move") {
        log("Moving from `" + dup + "` to `" + path + "`", LogLevel::Info);
        if(!moveFile(dup, path)) return SaveResult::Error;
        m_profile->removeMd5(md5(), dup);
        return SaveResult::Moved;
    }
    if(what == "link" || what == "hardlink") {
        log("Creating " + what + " for `" + dup + "` in `" + path + "`", LogLevel::Info);
        return createLink(dup, path, what) ? SaveResult::Linked : SaveResult::Error;
    }
    return SaveResult::Error;
}

static std::string pathTokens(std::string s, const std::string& path) {
    std::string dir = dirName(path);
    s = replaceAll(s, "%path:nobackslash%", path);
    s = replaceAll(s, "%path%", path);
    s = replaceAll(s, "%dir:nobackslash%", dir);
    s = replaceAll(s, "%dir%", dir);
    return s;
}

std::string Image::postSave(const std::string& path, SizeType sz, SaveResult res, bool addMd5, bool startCommands, int count, bool basic) {
    static const SaveResult md5Results[] = {SaveResult::Moved, SaveResult::Copied, SaveResult::Shortcut, SaveResult::Linked, SaveResult::Saved};
    bool add = addMd5 && std::find(std::begin(md5Results), std::end(md5Results), res) != std::end(md5Results);
    return postSaving(path, sz, add, startCommands, count, basic);
}

std::string Image::postSaving(std::string path, SizeType sz, bool addMd5, bool startCommands, int count, bool basic) {
    Settings& s = m_profile->settings();

    // External log files ("text files" in the settings)
    if(!basic) {
        for(auto& group : s.childGroups("LogFiles")) {
            std::string g = "LogFiles/" + group + "/";
            Filename content(s.get(g + "content"));
            StringList cont = content.path(tokens(), &s, "", count, Filename::Complex);
            if(cont.empty()) continue;
            int locationType = (int)s.getInt(g + "locationType");
            std::string file;
            if(locationType == 0) {
                StringList p = paths(Filename(s.get(g + "filename")), s.get(g + "path"), 0);
                if(!p.empty()) file = p[0];
            } else if(locationType == 1) {
                file = s.get(g + "uniquePath");
            } else if(locationType == 2) {
                file = path + s.get(g + "suffix");
            } else if(locationType == 3) {
                file = setExtension(path, "") + s.get(g + "suffixWithoutExtension");
            }
            if(file.empty()) continue;
            file = pathTokens(file, path);
            std::string text = pathTokens(cont[0], path);
            bool append = fileExists(file);
            appendFile(file, (append ? "\n" : "") + text);
        }
    }

    std::string ext = extension();
    if(s.getBool("Save/keepDate", true)) {
        DateTime d = createdAt();
        if(d.valid()) setFileMTime(path, d.toUnix());
    }

    // Fix the extension from the file header
    if(s.getBool("Save/headerDetection", true) && getExtension(path) == ext) {
        std::string headerExt = headerExtension(path);
        if(!headerExt.empty() && headerExt != ext && !(headerExt == "jpg" && ext == "jpeg")) {
            log("Invalid file extension (" + ext + " to " + headerExt + ") for `" + path + "`", LogLevel::Info);
            std::string newPath = buildPath(dirName(path), completeBaseName(path) + "." + headerExt);
            if(!fileExists(newPath) && renameFile(path, newPath)) {
                path = newPath;
                ext = headerExt;
            } else {
                log("Error renaming `" + path + "` to `" + newPath + "`", LogLevel::Error);
            }
        }
    }

    // Commands
    Commands& commands = m_profile->commands();
    if(startCommands) commands.before();
    for(auto& tag : m_tags) commands.tag(*this, tag, false);
    commands.image(*this, path);
    for(auto& tag : m_tags) commands.tag(*this, tag, true);
    if(startCommands) commands.after();

    // FFmpeg
    if(ext == "webm") {
        bool remux = s.getBool("Save/FFmpegRemuxWebmToMp4", false), convert = s.getBool("Save/FFmpegConvertWebmToMp4", false);
        bool overwrite = s.getBool("Save/FFmpegConvertOverwrite", true);
        int timeout = (int)s.getInt("Save/FFmpegConvertTimeout", 30000);
        if(remux && external::ffmpegVideoCodec(path) == "vp9") {
            path = external::ffmpegRemux(path, "mp4", overwrite, true, timeout);
            ext = getExtension(path);
        } else if(convert) {
            path = external::ffmpegConvert(path, "mp4", overwrite, true, timeout);
            ext = getExtension(path);
        }
    }

    // Image conversion
    std::string target = toLower(s.get("Save/ImageConversion/" + toUpper(ext) + "/to"));
    if(!target.empty() && target != ext) {
        std::string backend = s.get("Save/ImageConversion/" + toUpper(ext) + "/backend", s.get("Save/ImageConversionBackend", "ImageMagick"));
        bool overwrite = s.getBool("Save/ImageConversionOverwrite", true);
        int timeout = (int)s.getInt("Save/ImageConversionTimeout", 30000);
        path = backend == "FFmpeg" ? external::ffmpegConvert(path, target, overwrite, true, timeout)
                                   : external::imageMagickConvert(path, target, overwrite, true, timeout);
        ext = getExtension(path);
    }

    // Ugoira
    if(ext == "zip" && s.getBool("Save/ConvertUgoira", false)) {
        std::string fmt = s.get("Save/ConvertUgoiraFormat", "gif");
        path = external::ffmpegConvertUgoira(path, ugoiraFrames(), fmt, s.getBool("Save/ConvertUgoiraOverwrite", true),
                                             s.getBool("Save/ConvertUgoiraDeleteOriginal", false), (int)s.getInt("Save/ConvertUgoiraTimeout", 30000));
        ext = getExtension(path);
    }

    // Metadata through exiftool
    StringList exts = split(s.get("Save/MetadataExiftoolExtensions", "jpg jpeg png gif mp4"), ' ', true);
    if(exts.empty() || contains(exts, ext)) {
        std::vector<std::pair<std::string, std::string>> metadata;
        for(auto& item : s.readArray("Save/MetadataExiftool")) {
            StringList values = Filename(item["value"]).path(tokens(), &s, "", 0, Filename::Complex);
            if(!values.empty() && !item["key"].empty()) metadata.emplace_back(item["key"], values[0]);
        }
        if(!metadata.empty()) {
            static const std::map<std::string, external::Sidecar> sidecars{
                {"no", external::Sidecar::No}, {"on_error", external::Sidecar::OnError}, {"both", external::Sidecar::Both}, {"only", external::Sidecar::Only}};
            auto it = sidecars.find(s.get("Save/MetadataExiftoolSidecar", "on_error"));
            external::exiftoolSetMetadata(path, metadata, s.getBool("Save/MetadataExiftoolClear", false),
                                          s.getBool("Save/MetadataExiftoolKeepColorProfile", true),
                                          it != sidecars.end() ? it->second : external::Sidecar::OnError,
                                          s.getBool("Save/MetadataExiftoolSidecarNoExtension", false));
        }
    }

    if(addMd5) m_profile->addMd5(md5(), path);
    setSavePath(path, sz);
    return path;
}

void Image::remove(const StringList& files) {
    Settings& s = m_profile->settings();
    for(auto& path : files) {
        removeFile(path);
        for(auto& group : s.childGroups("LogFiles")) {
            std::string g = "LogFiles/" + group + "/";
            int locationType = (int)s.getInt(g + "locationType");
            if(locationType == 2) removeFile(path + s.get(g + "suffix"));
            if(locationType == 3) removeFile(setExtension(path, "") + s.get(g + "suffixWithoutExtension"));
        }
        m_profile->removeMd5(md5(), path);
    }
}

// ---- display ---------------------------------------------------------------------------------

std::string Image::state() const {
    if(!m_profile->blacklist().match(tokens()).empty()) return "blacklisted";
    for(auto& tag : m_tags) {
        if(contains(m_search, tag.text())) continue;
        for(auto& fav : m_profile->favorites())
            if(fav.name == tag.text()) return "favorited";
    }
    auto data = [&](const std::string& k) {
        auto it = m_data.find(k);
        return it == m_data.end() ? Var() : it->second;
    };
    if(data("parentid").toInt() != 0) return "parent";
    if(data("has_children").toBool()) return "children";
    if(data("status").toString() == "pending") return "pending";
    return "";
}

std::string Image::tooltip() const {
    auto data = [&](const std::string& k) {
        auto it = m_data.find(k);
        return it == m_data.end() ? std::string() : it->second.toString();
    };
    std::string out;
    if(!m_tags.empty()) {
        TagList sorted = m_tags;
        sortTags(sorted, m_profile->settings().get("Viewer/tagOrder", "type"));
        StringList names;
        for(size_t i = 0; i < sorted.size() && i < 60; i++) names.push_back(htmlEscape(sorted[i].text()));
        out += "<b>Tags:</b> " + join(names, " ") + (sorted.size() > 60 ? " …" : "") + "\n\n";
    }
    if(m_id) out += "<b>ID:</b> " + std::to_string(m_id) + "\n";
    if(!m_name.empty()) out += "<b>Name:</b> " + htmlEscape(m_name) + "\n";
    if(!data("rating").empty()) out += "<b>Rating:</b> " + htmlEscape(data("rating")) + "\n";
    if(!data("score").empty()) out += "<b>Score:</b> " + htmlEscape(data("score")) + "\n";
    if(!data("author").empty()) out += "<b>User:</b> " + htmlEscape(data("author")) + "\n";
    if(width() > 0 && height() > 0) out += "<b>Size:</b> " + std::to_string(width()) + " × " + std::to_string(height()) + "\n";
    if(fileSize() > 0) out += "<b>File size:</b> " + formatFilesize((double)fileSize()) + "\n";
    DateTime d = createdAt();
    if(d.valid()) out += "<b>Date:</b> " + d.toDisplayString() + "\n";
    while(!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

std::vector<std::pair<std::string, std::string>> Image::details() const {
    auto data = [&](const std::string& k) {
        auto it = m_data.find(k);
        return it == m_data.end() ? Var() : it->second;
    };
    const std::string unknown = "Unknown";
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("ID", m_id ? std::to_string(m_id) : unknown);
    out.emplace_back("MD5", !md5().empty() ? md5() : unknown);
    out.emplace_back("Rating", data("rating").toString().empty() ? unknown : data("rating").toString());
    out.emplace_back("Score", data("score").toString());
    out.emplace_back("Author", data("author").toString().empty() ? unknown : data("author").toString());
    out.emplace_back("Date", createdAt().valid() ? createdAt().toDisplayString() : unknown);
    out.emplace_back("Size", width() > 0 ? std::to_string(width()) + "×" + std::to_string(height()) : unknown);
    out.emplace_back("File size", fileSize() > 0 ? formatFilesize((double)fileSize()) : unknown);
    out.emplace_back("Page", m_pageUrl.empty() ? unknown : m_pageUrl);
    out.emplace_back("URL", url().empty() ? unknown : url());
    out.emplace_back(m_sources.size() > 1 ? "Sources" : "Source", m_sources.empty() ? unknown : join(m_sources, "\n"));
    out.emplace_back("Sample", url(SizeType::Sample).empty() ? unknown : url(SizeType::Sample));
    out.emplace_back("Thumbnail", url(SizeType::Thumbnail).empty() ? unknown : url(SizeType::Thumbnail));
    long long parent = data("parentid").toInt();
    out.emplace_back("Parent", parent ? "yes (#" + std::to_string(parent) + ")" : "no");
    out.emplace_back("Children", data("has_children").toBool() ? "yes" : "no");
    out.emplace_back("Comments", data("has_comments").toBool() ? "yes" : "no");
    out.emplace_back("Notes", data("has_note").toBool() ? "yes" : "no");
    return out;
}

}  // namespace bg
