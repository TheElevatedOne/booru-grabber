#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/filename.hpp"
#include "core/net.hpp"
#include "core/source.hpp"
#include "core/token.hpp"

namespace bg {

class Profile;
class Site;

enum class SaveResult {
    AlreadyExistsDisk,
    AlreadyExistsMd5,
    AlreadyExistsDeletedMd5,
    Blacklisted,
    Moved,
    Copied,
    Shortcut,
    Linked,
    Saved,
    Error,
    NotLoaded,
    NotFound,
    NetworkError,
    DetailsLoadError
};
const char* saveResultName(SaveResult r);

struct ImageSize {
    SizeType type = SizeType::Unknown;
    std::string url;
    int width = 0, height = 0;
    long long fileSize = 0, bitRate = 0;
    Rect rect;

    ~ImageSize();
    bool hasSize() const { return width > 0 && height > 0; }
    // Moves the temporary file (or copies the saved one) to path; returns the source path or "" when not loaded
    std::string save(const std::string& path);
    bool setTemporaryPath(const std::string& path);
    bool setSavePath(const std::string& path);
    const std::string& savePath() const { return m_savePath; }
    const std::string& temporaryPath() const { return m_temporaryPath; }
    std::string md5() const;
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);

private:
    std::string m_temporaryPath, m_savePath;
    mutable std::string m_md5;
};

class Image : public std::enable_shared_from_this<Image> {
public:
    enum class LoadResult { Ok, Error, CloudflareError, NetworkError };

    Image(std::shared_ptr<Site> site, const RawImage& raw, Profile* profile, StringList search = {}, std::string parentUrl = "");
    explicit Image(Profile* profile);
    bool read(const nlohmann::json& j);
    nlohmann::json write() const;

    Site* site() const { return m_site.get(); }
    std::shared_ptr<Site> sitePtr() const { return m_site; }
    Profile* profile() const { return m_profile; }
    unsigned long long id() const { return m_id; }
    std::string md5() const;
    const std::string& name() const { return m_name; }
    const TagList& tags() const { return m_tags; }
    StringList tagNames(bool namespaces = false) const;
    const std::vector<Pool>& pools() const { return m_pools; }
    const StringList& search() const { return m_search; }
    const StringList& sources() const { return m_sources; }
    nlohmann::json identity(bool withId = false) const;
    const std::string& pageUrl() const { return m_pageUrl; }
    const std::string& parentUrl() const { return m_parentUrl; }
    std::string url(SizeType size = SizeType::Full) const;
    std::string fileUrl() const { return size(SizeType::Full).url; }
    const ImageSize& size(SizeType s) const;
    ImageSize& size(SizeType s);
    int width() const { return size(SizeType::Full).width; }
    int height() const { return size(SizeType::Full).height; }
    long long fileSize() const { return size(SizeType::Full).fileSize; }
    DateTime createdAt() const;
    std::string dateRaw() const;
    std::string extension() const;
    bool isGallery() const { return m_isGallery; }
    int galleryCount() const { return m_galleryCount; }
    bool isVideo() const;
    std::string isAnimated() const;  // "gif", "apng" or ""
    bool hasTag(const std::string& tag) const;
    bool hasUnknownTag() const;
    int value() const;  // approximate pixel count, for sorting
    SizeType preferredDisplaySize() const;
    const ImageSize& mediaForSize(int width, int height, bool thumbnail = false) const;
    std::vector<std::pair<std::string, int>> ugoiraFrames() const;
    std::shared_ptr<Image> parentGallery() const { return m_parentGallery; }
    const std::map<std::string, Var>& data() const { return m_data; }
    StringList extensionRotation() const { return m_extensions; }
    std::string nextExtension();

    void setUrl(const std::string& url);
    void setSize(int w, int h, SizeType s);
    void setFileSize(long long size, SizeType s);
    void setFileExtension(const std::string& ext);
    void setTags(const TagList& tags);
    void setParentGallery(std::shared_ptr<Image> gallery);
    void setTemporaryPath(const std::string& path, SizeType s = SizeType::Full);
    void setSavePath(const std::string& path, SizeType s = SizeType::Full);
    std::string savePath(SizeType s = SizeType::Full) const { return size(s).savePath(); }
    void setDetailsErrorsAsWarnings(bool w) { m_detailsWarnings = w; }

    // Tokens for filenames and filters (cached until something changes)
    const TokenMap& tokens() const;
    void refreshTokens() { m_tokens.reset(); }

    // Details page (full tags, real file URL, pools...)
    void loadDetails(std::function<void(LoadResult)> done, bool rateLimit = false);
    void abortDetails();
    bool detailsLoaded() const { return m_detailsLoaded; }

    // Saving
    StringList paths(const Filename& filename, const std::string& folder, int count) const;
    SaveResult preSave(const std::string& path, SizeType size);
    std::string postSave(const std::string& path, SizeType size, SaveResult result, bool addMd5, bool startCommands, int count,
                         bool basic = false);
    void remove(const StringList& paths);

    // Display helpers
    std::string tooltip() const;  // Pango markup
    std::vector<std::pair<std::string, std::string>> details() const;
    std::string state() const;  // "blacklisted", "favorited", "parent", "children", "pending" or ""

    Signal<const std::string&, const std::string&> urlChanged;

private:
    void init();
    TokenMap generateTokens() const;
    std::string postSaving(std::string path, SizeType size, bool addMd5, bool startCommands, int count, bool basic);
    void parseDetails(const net::Response& r);
    void finishDetails(LoadResult r);

    Profile* m_profile = nullptr;
    std::shared_ptr<Site> m_site;
    unsigned long long m_id = 0;
    std::string m_md5, m_name, m_pageUrl, m_parentUrl, m_extension;
    StringList m_search, m_sources, m_extensions;
    nlohmann::json m_identity = nlohmann::json::object();
    std::map<std::string, Var> m_data;
    TagList m_tags;
    std::vector<Pool> m_pools;
    bool m_isGallery = false;
    int m_galleryCount = -1, m_position = 0;
    std::map<SizeType, std::shared_ptr<ImageSize>> m_sizes;
    std::vector<std::shared_ptr<ImageSize>> m_allSizes;
    std::shared_ptr<Image> m_parentGallery;
    mutable std::unique_ptr<TokenMap> m_tokens;
    // details loading
    bool m_detailsLoading = false, m_detailsLoaded = false, m_detailsWarnings = false;
    std::shared_ptr<net::Reply> m_detailsReply;
    std::vector<std::function<void(LoadResult)>> m_detailsWaiters;
    int m_detailsRetries = 0;
    size_t m_extensionIndex = 0;
};

using ImagePtr = std::shared_ptr<Image>;

}  // namespace bg
