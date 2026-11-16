#include "core/source.hpp"

#include "core/logger.hpp"

namespace bg {

using js::Value;

static std::string normalizeApiName(std::string key) {
    key = toLower(key);
    if(!key.empty()) key[0] = (char)g_ascii_toupper(key[0]);
    return key;
}

// getProperty() semantics of the original: numbers and numeric strings for ints
static void readInt(const Value& obj, const char* key, int& out) {
    Value v = obj.prop(key);
    if(v.isNumber() || v.isString()) out = (int)v.toInt();
}
static void readString(const Value& obj, const char* key, std::string& out) {
    Value v = obj.prop(key);
    if(v.isString()) out = v.toString();
}

// ---- ApiEndpoint -------------------------------------------------------------

std::string ApiEndpoint::name() const {
    Value n = m_endpoint.prop("name");
    return n.isString() ? n.toString() : "";
}

bool ApiEndpoint::parseErrors() const { return m_endpoint.prop("parseErrors").toBool(); }

bool ApiEndpoint::needAuth() const {
    Value a = m_endpoint.prop("auth");
    if(a.isBool()) return a.toBool();
    if(a.isArray()) return a.length() > 0;
    return false;
}

PageUrl ApiEndpoint::url(const nlohmann::json& query, int page, int limit, const PageInformation& last, const SiteInfo& site) const {
    PageUrl ret;
    js::Context& ctx = js::sourceContext();
    Value q = ctx.fromJson(query.is_object() ? query : nlohmann::json::object());
    Value opts = m_api->options(limit, site);
    opts.set("page", page);
    Value r = m_endpoint.prop("url").call({q, opts, m_api->previousInfo(last)});
    m_api->fillUrl(r, &site, ret);
    return ret;
}

ParsedPage ApiEndpoint::parse(const std::string& source, int status, int first, const SiteInfo& site) const {
    js::Context& ctx = js::sourceContext();
    Value r = m_endpoint.prop("parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) {
        ParsedPage p;
        p.error = r.errorMessage();
        return p;
    }
    return m_api->parseResults(r, first, site);
}

nlohmann::json ApiEndpoint::parseAny(const std::string& source, int status) const {
    js::Context& ctx = js::sourceContext();
    Value r = m_endpoint.prop("parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) return nullptr;
    return r.toJson();
}

// ---- Api -------------------------------------------------------------------

Api::Api(Value source, std::string key) : m_source(std::move(source)), m_key(std::move(key)), m_name(normalizeApiName(m_key)) {
    Value eps = api().prop("endpoints");
    if(eps.isObject())
        for(auto& k : eps.keys()) m_endpoints[k] = std::make_unique<ApiEndpoint>(this, eps.prop(k));
}

const ApiEndpoint* Api::endpoint(const std::string& name) const {
    auto it = m_endpoints.find(name);
    return it == m_endpoints.end() ? nullptr : it->second.get();
}

Value Api::jsConst(const std::string& fullKey) const {
    Value fromApi = api().path(fullKey);
    if(!fromApi.isUndefined()) return fromApi;
    return m_source.path(fullKey);
}

bool Api::needAuth() const { return !jsConst("auth").toStringList().empty(); }

void Api::fillUrl(const Value& result, const SiteInfo* site, PageUrl& ret) const {
    if(result.isError()) {
        ret.error = result.errorMessage();
        log(ret.error, LogLevel::Error);
        return;
    }
    std::string url;
    if(result.isObject()) {
        if(result.has("error")) {
            ret.error = result.prop("error").toString();
            return;
        }
        url = result.prop("url").toString();
        Value method = result.prop("method");
        ret.isPost = method.isString() && method.toString() == "POST";
        Value data = result.prop("data");
        if(!data.isUndefined()) {
            if(data.isString()) {
                ret.payload = data.toString();
            } else {
                ret.payload = data.toJson().dump();
                ret.headers["Content-Type"] = "application/json";
            }
        }
        Value headers = result.prop("headers");
        if(headers.isObject())
            for(auto& k : headers.keys()) ret.headers[k] = headers.prop(k).toString();
    } else {
        url = result.isNullish() ? "" : result.toString();
    }
    if(site && site->fixUrl && !url.empty()) url = site->fixUrl(url);
    ret.url = url;
}

Value Api::previousInfo(const PageInformation& last) const {
    js::Context& ctx = js::sourceContext();
    if(last.page <= 0) return ctx.undefined();
    Value prev = ctx.newObject();
    prev.set("page", last.page);
    prev.set("minIdM1", std::to_string(last.minId - 1));
    prev.set("minId", std::to_string(last.minId));
    prev.set("maxId", std::to_string(last.maxId));
    prev.set("maxIdP1", std::to_string(last.maxId + 1));
    prev.set("minDate", last.minDate);
    prev.set("maxDate", last.maxDate);
    return prev;
}

Value Api::options(int limit, const SiteInfo& site) const {
    js::Context& ctx = js::sourceContext();
    Value opts = ctx.newObject();
    opts.set("limit", limit);
    opts.set("baseUrl", site.baseUrl);
    opts.set("loggedIn", site.loggedIn);
    return opts;
}

PageUrl Api::pageUrl(const std::string& search, int page, int limit, const PageInformation& last, const SiteInfo& site) const {
    PageUrl ret;
    Value fn = api().path("search.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support search";
        return ret;
    }
    js::Context& ctx = js::sourceContext();
    Value parsed = ctx.undefined();
    if(jsConst("search.parseInput").toBool() && !trim(search).empty()) {
        StringList operands = split(search, ' ', true);
        auto ids = site.tagIds ? site.tagIds(operands) : std::map<std::string, int>();
        auto makeTag = [&](const std::string& name) {
            Value t = ctx.newObject();
            t.set("name", name);
            auto it = ids.find(name);
            if(it != ids.end() && it->second > 0) t.set("id", it->second);
            return t;
        };
        parsed = makeTag(operands[0]);
        for(size_t i = 1; i < operands.size(); i++) {
            Value op = ctx.newObject();
            op.set("operator", "and");
            op.set("left", parsed);
            op.set("right", makeTag(operands[i]));
            parsed = op;
        }
    }
    Value query = ctx.newObject();
    query.set("search", search);
    query.set("parsedSearch", parsed);
    query.set("page", page);
    Value r = fn.call({query, options(limit, site), previousInfo(last)});
    fillUrl(r, &site, ret);
    return ret;
}

TagList Api::makeTags(const Value& tags, const SiteInfo& site) const {
    TagList ret;
    uint32_t n = tags.length();
    for(uint32_t i = 0; i < n; i++) {
        Value tag = tags.at(i);
        if(tag.isString()) {
            ret.emplace_back(tag.toString());
            continue;
        }
        if(!tag.isObject()) continue;
        int id = 0, count = 0, typeId = -1;
        readInt(tag, "id", id);
        readInt(tag, "count", count);
        std::string name = tag.prop("name").isString() ? tag.prop("name").toString() : "";
        std::string type;
        Value t = tag.prop("type");
        if(t.isNumber())
            typeId = (int)t.toInt();
        else if(!t.isNullish())
            type = t.toString();
        readInt(tag, "typeId", typeId);
        StringList related;
        Value rel = tag.prop("related");
        if(rel.isArray())
            related = rel.toStringList();
        else if(!rel.isNullish())
            related = split(rel.toString(), ' ', true);
        TagType tagType;
        if(!type.empty())
            tagType = TagType(Tag::getType(type));
        else {
            auto it = site.tagTypes.find(typeId);
            if(it != site.tagTypes.end()) tagType = it->second;
        }
        ret.emplace_back(id, name, tagType, count, related);
    }
    return ret;
}

std::optional<RawImage> Api::makeImage(const Value& raw, const SiteInfo& site, int index, int first) const {
    RawImage img;
    if(!raw.isObject()) return std::nullopt;
    for(auto& key : raw.keys()) {
        Value val = raw.prop(key);
        if(val.isNullish()) continue;
        if(key == "identity") {
            img.identity = val.toJson();
            if(!img.identity.is_object()) img.identity = nlohmann::json::object();
        } else if(key == "tags_obj" || (key == "tags" && val.isArray())) {
            img.tags = makeTags(val, site);
        } else if(key == "tokens") {
            for(auto& tk : val.keys()) {
                Value tv = val.prop(tk);
                if(tv.isString()) {
                    std::string s = tv.toString();
                    if(startsWith(s, "date:")) {
                        DateTime d = DateTime::parse(s.substr(5));
                        if(d.valid()) {
                            img.data[tk] = Var(d);
                            continue;
                        }
                    }
                    if(startsWith(s, "b64:")) {
                        img.data[tk] = Var(hexEncode(base64Decode(s.substr(4))));
                        continue;
                    }
                    img.data[tk] = Var(s);
                } else {
                    img.data[tk] = Var::fromJson(tv.toJson());
                }
            }
        } else if(key == "medias") {
            uint32_t n = val.length();
            for(uint32_t i = 0; i < n; i++) {
                Value m = val.at(i);
                RawMedia media;
                if(m.has("url")) media.url = site.fixMediaUrl ? site.fixMediaUrl(m.prop("url").toString()) : m.prop("url").toString();
                readInt(m, "width", media.width);
                readInt(m, "height", media.height);
                if(m.has("bitrate")) media.bitRate = m.prop("bitrate").toInt();
                if(m.has("file_size")) media.fileSize = m.prop("file_size").toInt();
                Value rect = m.prop("rect");
                if(rect.isObject()) {
                    readInt(rect, "x", media.rect.x);
                    readInt(rect, "y", media.rect.y);
                    readInt(rect, "width", media.rect.w);
                    readInt(rect, "height", media.rect.h);
                }
                std::string type = m.prop("type").isString() ? m.prop("type").toString() : "";
                if(type == "full") media.type = SizeType::Full;
                else if(type == "sample") media.type = SizeType::Sample;
                else if(type == "preview") media.type = SizeType::Thumbnail;
                img.medias.push_back(media);
            }
        } else if(val.isArray()) {
            img.d[key] = join(val.toStringList(), key == "sources" ? "\n" : " ");
        } else {
            img.d[key] = val.toString();
        }
    }
    if(img.d.empty()) return std::nullopt;

    // Api::parseImage of the original
    int pos = first + (img.d.count("position") ? (int)toInt(img.d["position"]) : index);
    img.d["position"] = std::to_string(pos + 1);
    if(img.d.count("ext") && startsWith(img.d["ext"], ".")) img.d["ext"] = img.d["ext"].substr(1);
    std::string& fileUrl = img.d["file_url"];
    std::string& sampleUrl = img.d["sample_url"];
    std::string& previewUrl = img.d["preview_url"];
    if(fileUrl.empty()) fileUrl = !sampleUrl.empty() ? sampleUrl : previewUrl;
    if(sampleUrl.empty()) sampleUrl = fileUrl;
    if(img.d.count("ext") && !img.d["ext"].empty() && endsWith(fileUrl, "/." + img.d["ext"])) {
        log("[" + m_name + "] Image #" + std::to_string(pos + 1) + " ignored. Reason: file url.", LogLevel::Info);
        return std::nullopt;
    }
    return img;
}

ParsedPage Api::parseResults(const Value& results, int first, const SiteInfo& site) const {
    ParsedPage ret;
    if(results.isError()) {
        ret.error = results.errorMessage();
        return ret;
    }
    if(results.has("error")) ret.error = results.prop("error").toString();
    Value tags = results.prop("tags");
    if(!tags.isNullish()) ret.tags = makeTags(tags, site);
    Value images = results.prop("images");
    uint32_t n = images.length();
    for(uint32_t i = 0; i < n; i++) {
        auto img = makeImage(images.at(i), site, (int)i, first);
        if(img) ret.images.push_back(std::move(*img));
    }
    readInt(results, "imageCount", ret.imageCount);
    readInt(results, "pageCount", ret.pageCount);
    readString(results, "urlNextPage", ret.urlNextPage);
    readString(results, "urlPrevPage", ret.urlPrevPage);
    Value wiki = results.prop("wiki");
    if(wiki.isString()) ret.wiki = replaceAll(wiki.toString(), "href=\"/", "href=\"" + site.baseUrl + "/");
    return ret;
}

ParsedPage Api::parsePage(const std::string& source, int status, int first, const SiteInfo& site) const {
    js::Context& ctx = js::sourceContext();
    Value r = api().path("search.parse").call({ctx.newString(source), ctx.newInt(status)});
    return parseResults(r, first, site);
}

PageUrl Api::galleryUrl(const std::string& id, const std::string& md5, const nlohmann::json& identity, int page, int limit,
                        const SiteInfo& site) const {
    PageUrl ret;
    Value fn = api().path("gallery.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support galleries";
        return ret;
    }
    js::Context& ctx = js::sourceContext();
    Value query = ctx.newObject();
    query.set("id", id);
    query.set("md5", md5);
    query.set("identity", ctx.fromJson(identity.is_object() ? identity : nlohmann::json::object()));
    query.set("page", page);
    Value r = fn.call({query, options(limit, site)});
    fillUrl(r, &site, ret);
    return ret;
}

ParsedPage Api::parseGallery(const std::string& source, int status, int first, const SiteInfo& site) const {
    js::Context& ctx = js::sourceContext();
    Value r = api().path("gallery.parse").call({ctx.newString(source), ctx.newInt(status)});
    return parseResults(r, first, site);
}

bool Api::mustLoadTagTypes() const {
    Value t = api().prop("tagTypes");
    return t.isUndefined() || !t.isBool();
}

bool Api::canLoadTagTypes() const { return api().path("tagTypes.url").isFunction(); }

PageUrl Api::tagTypesUrl(const SiteInfo& site) const {
    PageUrl ret;
    Value fn = api().path("tagTypes.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support tag type loading";
        return ret;
    }
    fillUrl(fn.call(), &site, ret);
    return ret;
}

ParsedTagTypes Api::parseTagTypes(const std::string& source, int status) const {
    ParsedTagTypes ret;
    js::Context& ctx = js::sourceContext();
    Value r = api().path("tagTypes.parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) {
        ret.error = r.errorMessage();
        return ret;
    }
    if(r.has("error")) ret.error = r.prop("error").toString();
    Value types = r.prop("types");
    uint32_t n = types.length();
    for(uint32_t i = 0; i < n; i++) {
        Value t = types.at(i);
        if(!t.isObject()) continue;
        ret.types.push_back({(int)t.prop("id").toInt(), t.prop("name").toString()});
    }
    return ret;
}

bool Api::canLoadTags() const { return api().path("tags.url").isFunction(); }

PageUrl Api::tagsUrl(int page, int limit, const std::string& order, const SiteInfo& site) const {
    PageUrl ret;
    Value fn = api().path("tags.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support tag loading";
        return ret;
    }
    js::Context& ctx = js::sourceContext();
    Value query = ctx.newObject();
    query.set("page", page);
    query.set("order", order);
    fillUrl(fn.call({query, options(limit, site)}), &site, ret);
    return ret;
}

ParsedTags Api::parseTags(const std::string& source, int status, const SiteInfo& site) const {
    ParsedTags ret;
    js::Context& ctx = js::sourceContext();
    Value r = api().path("tags.parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) {
        ret.error = r.errorMessage();
        return ret;
    }
    if(r.has("error")) ret.error = r.prop("error").toString();
    Value tags = r.prop("tags");
    if(!tags.isNullish()) ret.tags = makeTags(tags, site);
    return ret;
}

bool Api::canLoadDetails() const { return api().path("details.url").isFunction(); }
bool Api::canLoadFullDetails() const { return canLoadDetails() && jsConst("details.fullResults").toBool(); }

PageUrl Api::detailsUrl(unsigned long long id, const std::string& md5, const nlohmann::json& identity, const SiteInfo& site) const {
    PageUrl ret;
    Value fn = api().path("details.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support details loading";
        return ret;
    }
    js::Context& ctx = js::sourceContext();
    Value opts = ctx.newObject();
    opts.set("baseUrl", site.baseUrl);
    opts.set("loggedIn", site.loggedIn);
    Value r = fn.call({ctx.newString(std::to_string(id)), ctx.newString(md5), opts,
                       ctx.fromJson(identity.is_object() ? identity : nlohmann::json::object())});
    fillUrl(r, &site, ret);
    return ret;
}

ParsedDetails Api::parseDetails(const std::string& source, int status, const SiteInfo& site) const {
    ParsedDetails ret;
    js::Context& ctx = js::sourceContext();
    Value r = api().path("details.parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) {
        ret.error = r.errorMessage();
        return ret;
    }
    if(r.has("error") && r.prop("error").isString()) {
        ret.error = r.prop("error").toString();
        return ret;
    }
    if(canLoadFullDetails()) {
        ret.image = makeImage(r, site, 0, 0);
        if(!ret.image) ret.error = "No image returned";
        return ret;
    }
    Value tags = r.prop("tags");
    if(!tags.isNullish()) ret.tags = makeTags(tags, site);
    readString(r, "imageUrl", ret.imageUrl);
    std::string created;
    readString(r, "createdAt", created);
    if(!created.empty()) ret.createdAt = DateTime::parse(created);
    Value source1 = r.prop("source");
    if(source1.isString()) ret.sources = {source1.toString()};
    Value sources = r.prop("sources");
    if(sources.isArray()) ret.sources = sources.toStringList();
    Value pools = r.prop("pools");
    uint32_t n = pools.length();
    for(uint32_t i = 0; i < n; i++) {
        Value p = pools.at(i);
        if(!p.isObject()) continue;
        Pool pool;
        readInt(p, "id", pool.id);
        pool.name = p.prop("name").toString();
        readInt(p, "next", pool.next);
        readInt(p, "previous", pool.previous);
        ret.pools.push_back(pool);
    }
    return ret;
}

bool Api::canLoadCheck() const { return api().path("check.url").isFunction(); }

PageUrl Api::checkUrl() const {
    PageUrl ret;
    Value fn = api().path("check.url");
    if(!fn.isFunction()) {
        ret.error = "This API does not support checking";
        return ret;
    }
    fillUrl(fn.call(), nullptr, ret);
    return ret;
}

bool Api::parseCheck(const std::string& source, int status, std::string* error) const {
    js::Context& ctx = js::sourceContext();
    Value r = api().path("check.parse").call({ctx.newString(source), ctx.newInt(status)});
    if(r.isError()) {
        if(error) *error = r.errorMessage();
        return false;
    }
    return r.toBool();
}

// ---- Auth ------------------------------------------------------------------

std::string AuthField::value(const std::function<std::string(const std::string&)>& setting) const {
    switch(type) {
        case Const: return constValue;
        case Hash: {
            std::string user = setting("auth/pseudo"), password = setting("auth/password");
            std::string siteSalt = replaceAll(setting("auth/salt"), "--your-password--", "--%password%--");
            if(password.size() >= 32) return password;  // already hashed
            std::string data = password;
            if(!salt.empty() && (!user.empty() || !password.empty())) {
                data = replaceAll(salt, "%salt%", siteSalt);
                data = replaceAll(data, "%pseudo%", user);
                data = replaceAll(data, "%pseudo:lower%", toLower(user));
                data = replaceAll(data, "%password%", password);
            }
            if(data.empty()) return data;
            return hashAlgo == "sha1" ? sha1Hex(data) : md5Hex(data);
        }
        default: {
            std::string v = setting("auth/" + id);
            return v.empty() ? def : v;
        }
    }
}

std::vector<AuthSettingField> Auth::settingFields() const {
    std::vector<AuthSettingField> out;
    if(type == "oauth2") {
        out.push_back({"consumerKey", "", false});
        out.push_back({"consumerSecret", "", true});
        if(authType == "password" || authType == "password_json") {
            out.push_back({"pseudo", "", false});
            out.push_back({"password", "", true});
        }
        if(authType == "refresh_token") {
            out.push_back({"accessToken", "", true});
            out.push_back({"refreshToken", "", true});
        }
        return out;
    }
    if(type == "http_basic") {
        out.push_back({"pseudo", "", false});
        out.push_back({passwordType, "", true});
        return out;
    }
    for(auto& f : fields) {
        if(f.id.empty() || f.type == AuthField::Hash || f.type == AuthField::Const) continue;
        out.push_back({f.id, f.def, f.type == AuthField::Password});
    }
    // Hash fields derive from pseudo/password
    bool hasHash = false;
    for(auto& f : fields) hasHash = hasHash || f.type == AuthField::Hash;
    if(hasHash) {
        bool hasPseudo = false, hasPassword = false;
        for(auto& s : out) {
            hasPseudo = hasPseudo || s.id == "pseudo";
            hasPassword = hasPassword || s.id == "password";
        }
        if(!hasPseudo) out.insert(out.begin(), {"pseudo", "", false});
        if(!hasPassword) out.push_back({"password", "", true});
    }
    return out;
}

// ---- Source ----------------------------------------------------------------

Source::Source(const std::string& readDir, const std::string& writeDir) : m_readDir(readDir), m_writeDir(writeDir) {
    m_diskName = baseName(readDir);
    load();
    std::string sitesFile = fileExists(buildPath(m_writeDir, "sites.txt")) ? buildPath(m_writeDir, "sites.txt") : buildPath(m_readDir, "sites.txt");
    m_sites = readLines(sitesFile);
    m_supported = readLines(buildPath(m_readDir, "supported.txt"));
}

void Source::load() {
    std::string path = fileExists(buildPath(m_writeDir, "model.js")) ? buildPath(m_writeDir, "model.js") : buildPath(m_readDir, "model.js");
    auto code = readFile(path);
    if(!code) {
        log("JavaScript model.js not found for '" + m_diskName + "'", LogLevel::Warning);
        return;
    }
    static const Regex exportRx("export\\s+(?:var|const|let)\\s+source\\s*=");
    std::string wrapped = "(function() { var window = {}; " + exportRx.replace(*code, "return ") + "\n})()";
    js::Context& ctx = js::sourceContext();
    Value model = ctx.eval(wrapped, path);
    if(model.isError() || !model.isObject()) {
        log("Error loading source '" + m_diskName + "': " + model.errorMessage(), LogLevel::Error);
        return;
    }
    m_model = model;
    m_name = model.prop("name").toString();
    m_tokens = model.prop("tokens").toStringList();

    Value apis = model.prop("apis");
    for(auto& key : apis.keys()) m_apis.push_back(std::make_unique<Api>(model, key));
    if(m_apis.empty()) log("No valid API found in the model.js file of " + m_name, LogLevel::Warning);

    Value auths = model.prop("auth");
    for(auto& id : auths.keys()) {
        Value a = auths.prop(id);
        auto auth = std::make_shared<Auth>();
        auth->id = id;
        auth->type = a.prop("type").toString();
        Value check = a.prop("check");
        std::string checkType = check.isObject() ? check.prop("type").toString() : "";
        if(checkType == "max_page") auth->maxPage = (int)check.prop("value").toInt();
        if(auth->type == "oauth2") {
            readString(a, "authType", auth->authType);
            readString(a, "clientAuthentication", auth->clientAuthentication);
            readString(a, "tokenUrl", auth->tokenUrl);
            readString(a, "authorizationUrl", auth->authorizationUrl);
            readString(a, "redirectUrl", auth->oauthRedirectUrl);
            readString(a, "urlProtocol", auth->urlProtocol);
            auth->scopes = a.prop("scopes").toStringList();
        } else if(auth->type == "http_basic") {
            readString(a, "passwordType", auth->passwordType);
            readString(a, "tokenType", auth->tokenType);
        } else if(auth->type != "oauth1") {
            Value fields = a.prop("fields");
            uint32_t n = fields.length();
            for(uint32_t i = 0; i < n; i++) {
                Value f = fields.at(i);
                AuthField field;
                if(f.prop("id").isString()) field.id = f.prop("id").toString();
                if(f.prop("key").isString()) field.key = f.prop("key").toString();
                std::string type = f.prop("type").toString();
                if(type == "hash") {
                    field.type = AuthField::Hash;
                    field.hashAlgo = f.prop("hash").toString();
                    field.salt = f.prop("salt").isString() ? f.prop("salt").toString() : "";
                } else if(type == "const") {
                    field.type = AuthField::Const;
                    field.constValue = f.prop("value").toString();
                } else {
                    field.type = type == "password" ? AuthField::Password : AuthField::Text;
                    if(f.prop("def").isString()) field.def = f.prop("def").toString();
                }
                auth->fields.push_back(field);
            }
            if(auth->type == "get" || auth->type == "post") {
                auth->url = a.prop("url").toString();
                if(checkType == "cookie") auth->cookie = check.prop("key").toString();
                if(checkType == "redirect") auth->redirectUrl = check.prop("url").toString();
                if(check.isObject()) auth->checkHeaders = check.prop("headers").toStringMap();
                Value csrf = a.prop("csrf");
                if(csrf.isObject()) {
                    auth->csrfUrl = csrf.prop("url").toString();
                    auth->csrfFields = csrf.prop("fields").toStringList();
                }
            }
        }
        m_auths[id] = auth;
    }
}

Api* Source::api(const std::string& name) const {
    for(auto& a : m_apis)
        if(iequals(a->name(), name)) return a.get();
    return nullptr;
}

bool Source::addSite(const std::string& site) {
    if(!contains(m_sites, site)) m_sites.push_back(site);
    std::sort(m_sites.begin(), m_sites.end());
    return writeFile(buildPath(m_writeDir, "sites.txt"), join(m_sites, "\n") + "\n");
}

bool Source::removeSite(const std::string& site) {
    removeAll(m_sites, site);
    return writeFile(buildPath(m_writeDir, "sites.txt"), join(m_sites, "\n") + "\n");
}

std::string Source::iconPath() const {
    std::string p = buildPath(m_writeDir, "icon.png");
    return fileExists(p) ? p : buildPath(m_readDir, "icon.png");
}

}  // namespace bg
