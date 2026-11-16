// Site sources: sites/<Source>/model.js and the APIs, endpoints and auth methods it declares.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/js.hpp"
#include "core/token.hpp"

namespace bg {

enum class SizeType { Unknown, Thumbnail, Sample, Full };

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

struct Pool {
    int id = 0;
    std::string name;
    int current = 0, next = 0, previous = 0;
};

struct PageInformation {
    int page = 0;
    unsigned long long minId = 0, maxId = 0;
    std::string minDate, maxDate;
    std::string previousPage, nextPage;
};

struct PageUrl {
    std::string error;
    std::string url;
    StringMap headers;
    bool isPost = false;
    std::string payload;
};

struct RawMedia {
    SizeType type = SizeType::Unknown;
    std::string url;
    int width = 0, height = 0;
    long long fileSize = 0, bitRate = 0;
    Rect rect;
};

// An image as returned by a source, before it becomes an Image
struct RawImage {
    StringMap d;  // flat string values (id, md5, file_url, preview_url, ...)
    nlohmann::json identity = nlohmann::json::object();
    std::map<std::string, Var> data;  // extra tokens
    TagList tags;
    std::vector<RawMedia> medias;
};

struct ParsedPage {
    std::string error;
    int pageCount = -1, imageCount = -1;
    TagList tags;
    std::vector<RawImage> images;
    std::string urlNextPage, urlPrevPage, wiki;
};

struct ParsedDetails {
    std::string error;
    std::vector<Pool> pools;
    TagList tags;
    std::string imageUrl;
    DateTime createdAt;
    StringList sources;
    std::optional<RawImage> image;
};

struct ParsedTagTypes {
    std::string error;
    std::vector<TagTypeWithId> types;
};

struct ParsedTags {
    std::string error;
    TagList tags;
};

// What an API call needs to know about the site it runs for
struct SiteInfo {
    std::string baseUrl;
    bool loggedIn = false;
    std::function<std::string(const std::string&)> fixUrl;       // site-ize and add login parameters
    std::function<std::string(const std::string&)> fixMediaUrl;  // site-ize only
    std::map<int, TagType> tagTypes;
    std::function<std::map<std::string, int>(const StringList&)> tagIds;
};

class Api;

class ApiEndpoint {
public:
    ApiEndpoint(Api* api, js::Value endpoint) : m_api(api), m_endpoint(std::move(endpoint)) {}
    std::string name() const;
    bool parseErrors() const;
    bool needAuth() const;
    PageUrl url(const nlohmann::json& query, int page, int limit, const PageInformation& last, const SiteInfo& site) const;
    ParsedPage parse(const std::string& source, int status, int first, const SiteInfo& site) const;
    nlohmann::json parseAny(const std::string& source, int status) const;

private:
    Api* m_api;
    js::Value m_endpoint;
};

class Api {
public:
    Api(js::Value source, std::string key);
    const std::string& name() const { return m_name; }  // "Json", "Xml", "Html", "Rss"...
    const std::string& key() const { return m_key; }
    bool needAuth() const;

    PageUrl pageUrl(const std::string& search, int page, int limit, const PageInformation& last, const SiteInfo& site) const;
    bool parsePageErrors() const { return jsConst("search.parseErrors").toBool(); }
    ParsedPage parsePage(const std::string& source, int status, int first, const SiteInfo& site) const;

    PageUrl galleryUrl(const std::string& id, const std::string& md5, const nlohmann::json& identity, int page, int limit,
                       const SiteInfo& site) const;
    bool parseGalleryErrors() const { return jsConst("gallery.parseErrors").toBool(); }
    ParsedPage parseGallery(const std::string& source, int status, int first, const SiteInfo& site) const;

    bool mustLoadTagTypes() const;
    bool canLoadTagTypes() const;
    PageUrl tagTypesUrl(const SiteInfo& site) const;
    ParsedTagTypes parseTagTypes(const std::string& source, int status) const;

    bool canLoadTags() const;
    PageUrl tagsUrl(int page, int limit, const std::string& order, const SiteInfo& site) const;
    ParsedTags parseTags(const std::string& source, int status, const SiteInfo& site) const;

    bool canLoadDetails() const;
    bool canLoadFullDetails() const;
    PageUrl detailsUrl(unsigned long long id, const std::string& md5, const nlohmann::json& identity, const SiteInfo& site) const;
    bool parseDetailsErrors() const { return jsConst("details.parseErrors").toBool(); }
    ParsedDetails parseDetails(const std::string& source, int status, const SiteInfo& site) const;

    bool canLoadCheck() const;
    PageUrl checkUrl() const;
    bool parseCheck(const std::string& source, int status, std::string* error = nullptr) const;

    int forcedLimit() const { return (int)jsConst("forcedLimit").toInt(); }
    int maxLimit() const { return (int)jsConst("maxLimit").toInt(); }
    StringList modifiers() const { return jsConst("modifiers").toStringList(); }
    StringList forcedTokens() const { return jsConst("forcedTokens").toStringList(); }
    const std::map<std::string, std::unique_ptr<ApiEndpoint>>& endpoints() const { return m_endpoints; }
    const ApiEndpoint* endpoint(const std::string& name) const;

    // internal helpers shared with endpoints
    js::Value jsConst(const std::string& fullKey) const;
    void fillUrl(const js::Value& result, const SiteInfo* site, PageUrl& ret) const;
    TagList makeTags(const js::Value& tags, const SiteInfo& site) const;
    std::optional<RawImage> makeImage(const js::Value& raw, const SiteInfo& site, int index, int first) const;
    ParsedPage parseResults(const js::Value& results, int first, const SiteInfo& site) const;
    js::Value previousInfo(const PageInformation& last) const;
    js::Value options(int limit, const SiteInfo& site) const;

private:
    js::Value api() const { return m_source.prop("apis").prop(m_key); }
    js::Value m_source;
    std::string m_key, m_name;
    std::map<std::string, std::unique_ptr<ApiEndpoint>> m_endpoints;
};

struct AuthField {
    enum Type { Text, Password, Hash, Const } type = Text;
    std::string id, key, def;
    std::string hashAlgo, salt, constValue;
    // Resolve the value from a site's settings ("auth/<id>")
    std::string value(const std::function<std::string(const std::string&)>& setting) const;
};

struct AuthSettingField {
    std::string id, def;
    bool isPassword = false;
};

struct Auth {
    std::string id;    // key in the source's auth map
    std::string type;  // url, get, post, http_basic, oauth2
    std::vector<AuthField> fields;
    int maxPage = 0;
    // get / post
    std::string url, cookie, redirectUrl, csrfUrl;
    StringList csrfFields;
    StringMap checkHeaders;
    // http_basic
    std::string passwordType = "password", tokenType = "Basic";
    // oauth2
    std::string authType, tokenUrl, authorizationUrl, oauthRedirectUrl, urlProtocol, clientAuthentication;
    StringList scopes;

    std::vector<AuthSettingField> settingFields() const;
};

class Source {
public:
    // readDir: shared sources directory entry; writeDir: user directory for the same source
    Source(const std::string& readDir, const std::string& writeDir);
    bool valid() const { return !m_apis.empty(); }
    const std::string& name() const { return m_name; }
    const std::string& diskName() const { return m_diskName; }
    const std::string& readDir() const { return m_readDir; }
    const std::string& writeDir() const { return m_writeDir; }
    const StringList& sites() const { return m_sites; }
    const StringList& supportedSites() const { return m_supported; }
    bool addSite(const std::string& site);
    bool removeSite(const std::string& site);
    const std::vector<std::unique_ptr<Api>>& apis() const { return m_apis; }
    Api* api(const std::string& name) const;
    const std::map<std::string, std::shared_ptr<Auth>>& auths() const { return m_auths; }
    const StringList& additionalTokens() const { return m_tokens; }
    std::string iconPath() const;
    js::Value model() const { return m_model; }

private:
    void load();
    std::string m_readDir, m_writeDir, m_diskName, m_name;
    js::Value m_model;
    std::vector<std::unique_ptr<Api>> m_apis;
    std::map<std::string, std::shared_ptr<Auth>> m_auths;
    StringList m_tokens, m_sites, m_supported;
};

}  // namespace bg
