#include "core/site.hpp"

#include <libsoup/soup.h>

#include <algorithm>

#include "config.hpp"
#include "core/logger.hpp"
#include "core/page.hpp"
#include "core/paths.hpp"
#include "core/profile.hpp"

namespace bg {

// ---- Login implementations ---------------------------------------------------

namespace {

std::function<std::string(const std::string&)> settingGetter(Site* site) {
    return [site](const std::string& k) { return site->setting(k); };
}

// Logged in when an anonymous-limited page (maxPage) returns images
class PageTestLogin : public Login {
public:
    PageTestLogin(Site* site, Auth* auth) : m_site(site), m_auth(auth) {}
    bool isTestable() const override { return m_auth->maxPage > 0; }
    void login(std::function<void(bool)> done) override {
        auto site = m_site->shared_from_this();
        m_page = std::make_shared<Page>(m_site->profile(), site, std::vector<Site*>{m_site}, SearchQuery(), m_auth->maxPage, 20);
        auto page = m_page;
        page->load([done, page](Page* p, bool) { done(!p->images().empty()); });
    }
    void abort() override {
        if(m_page) m_page->abort();
    }

protected:
    Site* m_site;
    Auth* m_auth;
    std::shared_ptr<Page> m_page;
};

class UrlLogin : public PageTestLogin {
public:
    using PageTestLogin::PageTestLogin;
    std::string complementUrl(const std::string& url) const override {
        std::string out = url;
        bool hasQuery = url.find('?') != std::string::npos;
        auto getter = settingGetter(m_site);
        for(auto& f : m_auth->fields) {
            if(f.key.empty()) continue;
            std::string val = f.value(getter);
            if(val.empty()) continue;
            // Don't add the parameter twice
            if(contains(out, "?" + f.key + "=") || contains(out, "&" + f.key + "=")) continue;
            out += (hasQuery ? "&" : "?") + f.key + "=" + urlEncode(val);
            hasQuery = true;
        }
        return out;
    }
};

class HttpBasicLogin : public PageTestLogin {
public:
    using PageTestLogin::PageTestLogin;
    void complementHeaders(StringMap& h) override {
        std::string user = m_site->setting("auth/pseudo");
        std::string pass = m_site->setting("auth/" + m_auth->passwordType);
        if(user.empty() && pass.empty()) return;
        h["Authorization"] = m_auth->tokenType + " " + base64Encode(user + ":" + pass);
    }
};

class HttpLogin : public Login {
public:
    HttpLogin(Site* site, Auth* auth) : m_site(site), m_auth(auth) {}
    bool isTestable() const override { return !m_auth->url.empty(); }

    void login(std::function<void(bool)> done) override {
        std::string url = m_site->fixUrl(m_auth->url);
        if(hasCookie(url)) {
            done(true);
            return;
        }
        std::vector<std::pair<std::string, std::string>> fields;
        auto getter = settingGetter(m_site);
        for(auto& f : m_auth->fields)
            if(!f.key.empty()) fields.emplace_back(f.key, f.value(getter));
        if(m_auth->csrfUrl.empty()) {
            send(url, fields, done);
            return;
        }
        std::weak_ptr<Site> weak = m_site->shared_from_this();
        RequestOptions opts;
        opts.autoLogin = false;
        m_reply = m_site->get(m_site->fixUrl(m_auth->csrfUrl), QueryType::Unknown, [this, weak, url, fields, done](const net::Response& r) mutable {
            if(!weak.lock()) return;
            if(!r.ok()) {
                log("Error loading CSRF information (" + r.error + ")", LogLevel::Error);
                done(false);
                return;
            }
            StringMap values = js::htmlInputValues(r.body, m_auth->csrfFields);
            for(auto& name : m_auth->csrfFields) {
                auto it = values.find(name);
                if(it == values.end()) {
                    log("Could not find HTML field '" + name + "'", LogLevel::Warning);
                    continue;
                }
                fields.emplace_back(name, it->second);
            }
            send(url, fields, done);
        }, opts);
    }

    void abort() override {
        if(m_reply) m_reply->abort();
    }

private:
    void send(const std::string& url, const std::vector<std::pair<std::string, std::string>>& fields, std::function<void(bool)> done) {
        std::weak_ptr<Site> weak = m_site->shared_from_this();
        auto finished = [this, weak, done](const net::Response& r) {
            if(!weak.lock()) return;
            bool ok = hasCookie(r.url) || hasCookie(m_site->baseUrl() + "/");
            if(!m_auth->redirectUrl.empty()) ok = contains(r.url, m_auth->redirectUrl);
            done(ok);
        };
        RequestOptions opts;
        opts.autoLogin = false;
        opts.headers = m_auth->checkHeaders;
        std::string body = net::encodeForm(fields);
        if(m_auth->type == "post")
            m_reply = m_site->post(url, body, "application/x-www-form-urlencoded", QueryType::Unknown, finished, opts);
        else
            m_reply = m_site->get(url + (contains(url, "?") ? "&" : "?") + body, QueryType::Unknown, finished, opts);
    }

    bool hasCookie(const std::string& url) const {
        if(m_auth->cookie.empty()) return false;
        auto cookies = net::Session::instance().cookies(url);
        auto it = cookies.find(m_auth->cookie);
        return it != cookies.end() && !it->second.empty() && it->second != "0";
    }

    Site* m_site;
    Auth* m_auth;
    std::shared_ptr<net::Reply> m_reply;
};

class OAuth2Login;
std::vector<OAuth2Login*>& pendingCallbacks() {
    static std::vector<OAuth2Login*> v;
    return v;
}

class OAuth2Login : public Login {
public:
    OAuth2Login(Site* site, Auth* auth) : m_site(site), m_auth(auth) {
        m_accessToken = site->setting("auth/accessToken");
        m_refreshToken = site->setting("auth/refreshToken");
        m_accessExpiration = DateTime::fromIso(site->setting("auth/accessTokenExpiration"));
        m_refreshExpiration = DateTime::fromIso(site->setting("auth/refreshTokenExpiration"));
    }
    ~OAuth2Login() override {
        stopServer();
        removeAll();
        if(m_refreshTimer) g_source_remove(m_refreshTimer);
    }

    bool isTestable() const override {
        return !m_auth->tokenUrl.empty() &&
               ((m_auth->authType != "authorization_code" && m_auth->authType != "pkce") || !m_auth->authorizationUrl.empty());
    }

    void login(std::function<void(bool)> done) override {
        m_done = done;
        DateTime now = DateTime::nowUtc();
        bool accessValid = !m_accessToken.empty() && (!m_accessExpiration.valid() || m_accessExpiration > now);
        bool refreshValid = !m_refreshToken.empty() && (!m_refreshExpiration.valid() || m_refreshExpiration > now);
        if(!accessValid && refreshValid) {
            refresh(true);
            return;
        }
        if(accessValid && (m_accessExpiration.valid() || m_auth->authType != "client_credentials")) {
            finish(true);
            return;
        }
        const std::string& type = m_auth->authType;
        if(type == "client_credentials") {
            postForm({{"grant_type", "client_credentials"}});
        } else if(type == "password") {
            std::vector<std::pair<std::string, std::string>> body{
                {"grant_type", "password"}, {"username", m_site->setting("auth/pseudo")}, {"password", m_site->setting("auth/password")}};
            StringMap headers;
            if(contains(m_auth->tokenUrl, "pixiv.net")) {
                std::string time = DateTime::nowUtc().format("yyyy-MM-ddTHH:mm:ss") + "+00:00";
                headers["X-Client-Time"] = time;
                headers["X-Client-Hash"] = md5Hex(time + "28c1fdd170a5204386cb1313c7077b34f83e4aaf4aa829ce78c231e05b0bae2c");
            }
            postForm(body, headers);
        } else if(type == "password_json") {
            nlohmann::json j{{"login", m_site->setting("auth/pseudo")}, {"password", m_site->setting("auth/password")}};
            postToken(j.dump(), "application/json", {}, false);
        } else if(type == "refresh_token") {
            refresh(true);
        } else if(type == "authorization_code" || type == "pkce") {
            authorizationCode();
        } else {
            finish(false);
        }
    }

    void complementHeaders(StringMap& h) override {
        DateTime now = DateTime::nowUtc();
        bool refreshValid = !m_refreshToken.empty() && (!m_refreshExpiration.valid() || m_refreshExpiration > now);
        if(refreshValid && m_accessExpiration.valid() && m_accessExpiration <= now) refresh(false);
        if(!m_accessToken.empty()) h["Authorization"] = "Bearer " + m_accessToken;
    }

    bool handleCallback(const std::string& url) {
        if(m_auth->urlProtocol.empty() || !startsWith(url, m_auth->urlProtocol + "://")) return false;
        std::string code = urlQueryValue(url, "code");
        if(code.empty()) return false;
        removeAll();
        exchangeCode(code, m_site->fixUrl(m_auth->oauthRedirectUrl));
        return true;
    }

private:
    void removeAll() {
        auto& v = pendingCallbacks();
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }

    void finish(bool ok) {
        auto done = std::move(m_done);
        m_done = nullptr;
        if(done) done(ok);
    }

    void clientCredentials(std::vector<std::pair<std::string, std::string>>& body, StringMap& headers) {
        std::string key = m_site->setting("auth/consumerKey"), secret = m_site->setting("auth/consumerSecret");
        if(key.empty() || secret.empty()) return;
        if(m_auth->clientAuthentication == "header")
            headers["Authorization"] = "Basic " + base64Encode(urlEncode(key) + ":" + urlEncode(secret));
        else {
            body.emplace_back("client_id", key);
            body.emplace_back("client_secret", secret);
        }
    }

    void postForm(std::vector<std::pair<std::string, std::string>> body, StringMap headers = {}) {
        clientCredentials(body, headers);
        if(!m_auth->scopes.empty()) body.emplace_back("scope", join(m_auth->scopes, " "));
        postToken(net::encodeForm(body), "application/x-www-form-urlencoded;charset=UTF-8", headers, false);
    }

    void postToken(const std::string& body, const std::string& contentType, const StringMap& headers, bool isRefresh) {
        net::Request req;
        req.url = m_site->fixUrl(m_auth->tokenUrl);
        req.method = "POST";
        req.body = body;
        req.contentType = contentType;
        req.headers = m_site->requestHeaders();
        for(auto& [k, v] : headers) req.headers[k] = v;
        std::weak_ptr<Site> weak = m_site->shared_from_this();
        m_reply = net::Session::instance().send(req, [this, weak, isRefresh](const net::Response& r) {
            if(!weak.lock()) return;
            bool ok = readResponse(r);
            if(isRefresh)
                refreshFinished(ok);
            else
                finish(ok);
        });
    }

    void refresh(bool forLogin) {
        if(forLogin) m_refreshForLogin = true;
        if(m_refreshing) return;
        if(m_refreshToken.empty()) {
            log("[" + m_site->url() + "] Cannot refresh OAuth2 token without a refresh token", LogLevel::Warning);
            if(forLogin) finish(false);
            return;
        }
        log("[" + m_site->url() + "] Refreshing OAuth2 token...", LogLevel::Info);
        m_refreshing = true;
        if(m_auth->authType == "password_json") {
            nlohmann::json j{{"refresh_token", m_refreshToken}};
            postToken(j.dump(), "application/json", {}, true);
            return;
        }
        std::vector<std::pair<std::string, std::string>> body{{"grant_type", "refresh_token"}, {"refresh_token", m_refreshToken}};
        StringMap headers;
        std::string key = m_site->setting("auth/consumerKey"), secret = m_site->setting("auth/consumerSecret");
        if(m_auth->clientAuthentication == "header" && !key.empty())
            headers["Authorization"] = "Basic " + base64Encode(urlEncode(key) + ":" + urlEncode(secret));
        else {
            body.emplace_back("client_id", key);
            body.emplace_back("client_secret", secret);
        }
        postToken(net::encodeForm(body), "application/x-www-form-urlencoded;charset=UTF-8", headers, true);
    }

    void refreshFinished(bool ok) {
        m_refreshing = false;
        bool forLogin = m_refreshForLogin;
        m_refreshForLogin = false;
        if(!forLogin) return;
        if(ok) {
            finish(true);
            return;
        }
        if(m_auth->authType == "refresh_token") {
            log("[" + m_site->url() + "] OAuth2 token refresh failed", LogLevel::Warning);
            finish(false);
            return;
        }
        log("[" + m_site->url() + "] Refresh failed, clearing tokens and logging in again", LogLevel::Warning);
        clearTokens();
        login(std::move(m_done));
    }

    void clearTokens() {
        m_accessToken.clear();
        m_refreshToken.clear();
        m_accessExpiration = DateTime();
        m_refreshExpiration = DateTime();
        for(const char* k : {"auth/accessToken", "auth/refreshToken", "auth/accessTokenExpiration", "auth/refreshTokenExpiration"})
            m_site->settings().remove(k);
    }

    static DateTime jwtExpiration(const std::string& jwt) {
        StringList parts = split(jwt, '.');
        if(parts.size() != 3) return {};
        try {
            auto j = nlohmann::json::parse(base64UrlDecode(parts[1]));
            if(j.contains("exp") && j["exp"].is_number()) return DateTime::fromUnix(j["exp"].get<long long>());
        } catch(...) {
        }
        return {};
    }

    bool readResponse(const net::Response& r) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(r.body);
        } catch(const std::exception& e) {
            log("[" + m_site->url() + "] Error parsing OAuth2 JSON response: " + std::string(e.what()), LogLevel::Warning);
            return false;
        }
        if(!j.is_object()) return false;
        if(!j.contains("token_type") && j.contains("response") && j["response"].is_object()) j = j["response"];
        if(!j.contains("token_type")) {
            if(j.contains("error")) log("[" + m_site->url() + "] Error during OAuth2 login: " + j["error"].dump(), LogLevel::Warning);
            else log("[" + m_site->url() + "] No OAuth2 token type received", LogLevel::Warning);
            return false;
        }
        if(toLower(j["token_type"].get<std::string>()) != "bearer") {
            log("[" + m_site->url() + "] Wrong OAuth2 token type received: " + j["token_type"].dump(), LogLevel::Warning);
            return false;
        }
        Settings& s = m_site->settings();
        m_accessToken = j.value("access_token", "");
        s.set("auth/accessToken", m_accessToken);
        if(j.contains("refresh_token") && j["refresh_token"].is_string()) {
            m_refreshToken = j["refresh_token"].get<std::string>();
            m_refreshExpiration = jwtExpiration(m_refreshToken);
            s.set("auth/refreshToken", m_refreshToken);
        }
        long long expiresIn = 0;
        for(const char* k : {"expires_in", "expires"})
            if(j.contains(k) && j[k].is_number()) expiresIn = j[k].get<long long>();
        m_accessExpiration = expiresIn > 0 ? DateTime::nowUtc().addSecs(expiresIn) : jwtExpiration(m_accessToken);
        if(m_accessExpiration.valid()) {
            s.set("auth/accessTokenExpiration", m_accessExpiration.toIso());
            int64_t secs = DateTime::nowUtc().secsTo(m_accessExpiration);
            if(m_refreshTimer) g_source_remove(m_refreshTimer);
            std::weak_ptr<Site> weak = m_site->shared_from_this();
            m_refreshTimer = after((unsigned)std::clamp<int64_t>(secs / 2, 60, 86400) * 1000, [this, weak] {
                if(!weak.lock()) return;
                m_refreshTimer = 0;
                refresh(false);
            });
        } else {
            s.remove("auth/accessTokenExpiration");
        }
        if(m_refreshExpiration.valid())
            s.set("auth/refreshTokenExpiration", m_refreshExpiration.toIso());
        else
            s.remove("auth/refreshTokenExpiration");
        s.sync();
        log("[" + m_site->url() + "] Received an OAuth2 access token", LogLevel::Info);
        return true;
    }

    // Authorization code flow: browser + local redirect server, or a custom URL scheme (pkce)
    void authorizationCode() {
        std::string key = m_site->setting("auth/consumerKey");
        bool pkce = m_auth->authType == "pkce";
        if(key.empty() && !pkce) {
            log("[" + m_site->url() + "] You need a client ID for the OAuth2 authorization code flow", LogLevel::Warning);
            finish(false);
            return;
        }
        m_state = hexEncode(randomBytes(12));
        m_verifier = base64UrlEncode(randomBytes(32));
        std::string redirect;
        if(!m_auth->urlProtocol.empty()) {
            redirect = m_site->fixUrl(m_auth->oauthRedirectUrl);
            pendingCallbacks().push_back(this);
        } else {
            if(!startServer()) {
                finish(false);
                return;
            }
            redirect = "http://127.0.0.1:58923/";
        }
        m_redirect = redirect;
        std::vector<std::pair<std::string, std::string>> q{{"response_type", "code"}, {"state", m_state}};
        if(!key.empty()) q.emplace_back("client_id", key);
        if(!redirect.empty() && m_auth->urlProtocol.empty()) q.emplace_back("redirect_uri", redirect);
        if(!m_auth->scopes.empty()) q.emplace_back("scope", join(m_auth->scopes, " "));
        if(pkce) {
            q.emplace_back("code_challenge", base64UrlEncode(sha256Raw(m_verifier)));
            q.emplace_back("code_challenge_method", "S256");
            q.emplace_back("client", "pixiv-android");
        }
        std::string auth = m_site->fixUrl(m_auth->authorizationUrl);
        auth += (contains(auth, "?") ? "&" : "?") + net::encodeForm(q);
        log("[" + m_site->url() + "] Login with OAuth2 in the browser: " + auth, LogLevel::Info);
        openUri(auth);
    }

    bool startServer() {
        stopServer();
        m_server = soup_server_new("server-header", "booru-grabber", nullptr);
        soup_server_add_handler(m_server, nullptr, serverCallback, this, nullptr);
        GError* err = nullptr;
        if(!soup_server_listen_local(m_server, 58923, SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
            log("[" + m_site->url() + "] OAuth2 redirect server could not start: " + std::string(err ? err->message : "?"), LogLevel::Error);
            if(err) g_error_free(err);
            stopServer();
            return false;
        }
        return true;
    }

    void stopServer() {
        if(m_server) {
            soup_server_disconnect(m_server);
            g_object_unref(m_server);
            m_server = nullptr;
        }
    }

    static void serverCallback(SoupServer*, SoupServerMessage* msg, const char*, GHashTable* query, gpointer data) {
        auto* self = static_cast<OAuth2Login*>(data);
        const char* code = query ? (const char*)g_hash_table_lookup(query, "code") : nullptr;
        const char* state = query ? (const char*)g_hash_table_lookup(query, "state") : nullptr;
        const char* body = code ? "<html><body>Login complete, you can close this window.</body></html>"
                                : "<html><body>Login failed: no authorization code received.</body></html>";
        soup_server_message_set_status(msg, SOUP_STATUS_OK, nullptr);
        soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY, body, strlen(body));
        if(!code) return;
        if(state && self->m_state != state) log("OAuth2 state mismatch, continuing anyway", LogLevel::Warning);
        std::string c = code;
        defer([self, c] {
            self->stopServer();
            self->exchangeCode(c, self->m_redirect);
        });
    }

    void exchangeCode(const std::string& code, const std::string& redirect) {
        std::vector<std::pair<std::string, std::string>> body{{"grant_type", "authorization_code"}, {"code", code}};
        if(!redirect.empty()) body.emplace_back("redirect_uri", redirect);
        if(m_auth->authType == "pkce") {
            body.emplace_back("code_verifier", m_verifier);
            body.emplace_back("include_policy", "true");
        }
        StringMap headers;
        std::string key = m_site->setting("auth/consumerKey"), secret = m_site->setting("auth/consumerSecret");
        if(!key.empty()) {
            if(m_auth->clientAuthentication == "header")
                headers["Authorization"] = "Basic " + base64Encode(urlEncode(key) + ":" + urlEncode(secret));
            else {
                body.emplace_back("client_id", key);
                if(!secret.empty()) body.emplace_back("client_secret", secret);
            }
        }
        postToken(net::encodeForm(body), "application/x-www-form-urlencoded;charset=UTF-8", headers, false);
    }

    friend bool bg::handleOAuthCallback(const std::string& url);

    Site* m_site;
    Auth* m_auth;
    std::string m_accessToken, m_refreshToken, m_state, m_verifier, m_redirect;
    DateTime m_accessExpiration, m_refreshExpiration;
    bool m_refreshing = false, m_refreshForLogin = false;
    std::function<void(bool)> m_done;
    std::shared_ptr<net::Reply> m_reply;
    SoupServer* m_server = nullptr;
    guint m_refreshTimer = 0;
};

std::unique_ptr<Login> buildLogin(Site* site, Auth* auth) {
    if(!auth) return nullptr;
    if(auth->type == "oauth2") return std::make_unique<OAuth2Login>(site, auth);
    if(auth->type == "http_basic") return std::make_unique<HttpBasicLogin>(site, auth);
    if(auth->type == "get" || auth->type == "post") return std::make_unique<HttpLogin>(site, auth);
    if(auth->type == "url") return std::make_unique<UrlLogin>(site, auth);
    return nullptr;
}

}  // namespace

bool handleOAuthCallback(const std::string& url) {
    for(auto* l : std::vector<OAuth2Login*>(pendingCallbacks()))
        if(l->handleCallback(url)) return true;
    return false;
}

// ---- Site --------------------------------------------------------------------

Site::Site(std::string url, Source* source, Profile* profile) : m_url(std::move(url)), m_source(source), m_profile(profile) {
    std::string disk = replaceAll(m_url, ":", "_");
    m_readDir = buildPath(source->readDir(), disk);
    m_configDir = buildPath(paths::configDir(), "sites/" + source->diskName() + "/" + disk);
    m_dataDir = buildPath(paths::dataDir(), "sites/" + source->diskName() + "/" + disk);
    loadConfig();
}

Site::~Site() {
    if(m_login) m_login->abort();
    m_scheduler.clear();
    if(m_tagDatabase) m_tagDatabase->save();
}

void Site::loadConfig() {
    if(m_settings) m_settings->sync();
    m_settings = std::make_unique<Settings>(buildPath(m_configDir, "settings.ini"), StringList{buildPath(m_readDir, "defaults.ini")});
    m_name = m_settings->get("name", m_url);

    // API order
    Settings& global = m_profile->settings();
    StringList defaults;
    for(int i = 1; i <= 4; i++) {
        std::string s = global.get("source_" + std::to_string(i));
        if(!s.empty()) defaults.push_back(s);
    }
    if(defaults.empty()) defaults = {"Xml", "Json", "Regex", "Rss"};
    StringList order;
    if(!m_settings->getBool("sources/usedefault", true)) {
        for(int i = 0; i < 4; i++) {
            std::string def = (size_t)i < defaults.size() ? defaults[i] : "";
            std::string s = m_settings->get("sources/source_" + std::to_string(i + 1), def);
            if(!s.empty()) order.push_back(s);
        }
        if(order.empty()) order = defaults;
    } else {
        order = defaults;
    }
    m_apis.clear();
    for(auto name : order) {
        if(iequals(name, "regex")) name = "Html";
        Api* api = m_source->api(name);
        if(api && std::find(m_apis.begin(), m_apis.end(), api) == m_apis.end()) m_apis.push_back(api);
    }

    // Auth
    m_auth.reset();
    if(m_login) m_login->abort();
    m_login.reset();
    std::string type = m_settings->get("login/type", "url");
    if(type != "disabled") {
        const auto& auths = m_source->auths();
        auto it = auths.find(type);
        if(it != auths.end())
            m_auth = it->second;
        else if(!auths.empty())
            m_auth = auths.begin()->second;
        m_login = buildLogin(this, m_auth.get());
    }

    // Cookies from the settings
    for(auto& c : m_settings->getList("cookies")) net::Session::instance().addCookie(host(), c);

    // Tag database
    if(m_tagDatabase) m_tagDatabase->save();
    std::string tagsTxt = buildPath(m_dataDir, "tags.txt");
    m_tagDatabase = std::make_unique<TagDatabase>(buildPath(m_readDir, "tag-types.txt"), buildPath(m_dataDir, "tag-types.txt"),
                                                  fileExists(tagsTxt) ? tagsTxt : buildPath(m_dataDir, "tags.db"));
    m_tagDatabase->loadTypes();
    m_tagDatabase->open();

    // Throttling
    m_scheduler.setMaxConcurrency((int)m_settings->getInt("download/simultaneous", 10));
    m_scheduler.setInterval((int)QueryType::List, (int)(m_settings->getDouble("download/throttle_page", 1) * 1000));
    m_scheduler.setInterval((int)QueryType::Img, (int)(m_settings->getDouble("download/throttle_image", 1) * 1000));
    m_scheduler.setInterval((int)QueryType::Thumbnail, (int)(m_settings->getDouble("download/throttle_thumbnail", 0) * 1000));
    m_scheduler.setInterval((int)QueryType::Details, (int)(m_settings->getDouble("download/throttle_details", 1) * 1000));
    m_scheduler.setInterval((int)QueryType::Retry, (int)(m_settings->getDouble("download/throttle_retry", 60) * 1000));

    // User-Agent
    m_userAgent = m_settings->get("Headers/User-Agent");
    if(m_userAgent.empty()) {
        m_userAgent = global.get("userAgent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0");
    } else {
        m_userAgent = replaceAll(m_userAgent, "%version%", BG_VERSION);
        if(contains(m_userAgent, "%userAgentID%")) {
            if(!m_settings->containsOwn("userAgentID")) m_settings->set("userAgentID", uuid());
            m_userAgent = replaceAll(m_userAgent, "%userAgentID%", m_settings->get("userAgentID"));
        }
    }
    configChanged.emit();
}

std::string Site::baseUrl() const {
    bool ssl = m_settings->getBool("ssl", false);
    return std::string(ssl ? "https" : "http") + "://" + m_url;
}

std::string Site::host() const {
    size_t slash = m_url.find('/');
    std::string h = slash == std::string::npos ? m_url : m_url.substr(0, slash);
    size_t colon = h.find(':');
    return colon == std::string::npos ? h : h.substr(0, colon);
}

std::string Site::fixUrl(const std::string& url, const std::string& old) const {
    if(url.empty()) return "";
    bool ssl = m_settings->getBool("ssl", false);
    std::string protocol = ssl ? "https" : "http";
    if(startsWith(url, "//")) return protocol + ":" + url;
    if(startsWith(url, "/")) {
        size_t slash = m_url.find('/');
        std::string basePath = slash == std::string::npos ? "" : m_url.substr(slash);
        std::string right = !basePath.empty() && startsWith(url, basePath) ? url.substr(basePath.size()) : url;
        return protocol + "://" + m_url + right;
    }
    if(!startsWith(url, "http")) {
        if(!old.empty()) return urlResolve(old, url);
        return protocol + "://" + m_url + "/" + url;
    }
    if(startsWith(url, "http://") && ssl && contains(url, m_url)) return protocol + "://" + url.substr(7);
    return url;
}

std::string Site::fixLoginUrl(const std::string& url) const {
    if(!m_auth || !m_login) return url;
    return m_login->complementUrl(url);
}

SiteInfo Site::info() const {
    SiteInfo i;
    i.baseUrl = baseUrl();
    i.loggedIn = isLoggedIn(false, true);
    const Site* self = this;
    i.fixUrl = [self](const std::string& u) { return self->fixUrl(self->fixLoginUrl(u)); };
    i.fixMediaUrl = [self](const std::string& u) { return self->fixUrl(u); };
    i.tagTypes = m_tagDatabase->tagTypes();
    TagDatabase* db = m_tagDatabase.get();
    i.tagIds = [db](const StringList& tags) { return db->getTagIds(tags); };
    return i;
}

StringMap Site::requestHeaders() const {
    StringMap h;
    // Legacy "headers" map and the [Headers] group
    for(auto& key : m_settings->childKeys("headers")) h[key] = m_settings->get("headers/" + key);
    for(auto& key : m_settings->childKeys("Headers")) h[key] = m_settings->get("Headers/" + key);
    h["User-Agent"] = m_userAgent;
    return h;
}

net::Request Site::makeRequest(std::string url, const RequestOptions& opts) {
    if(m_autoLogin && opts.autoLogin && m_loginStatus == LoginStatus::Unknown) login();
    if(m_settings->getBool("ssl", false) && urlScheme(url) == "http" && contains(url, m_url)) url = urlSetScheme(url, "https");
    net::Request req;
    req.url = url;
    std::string def = opts.ref == "preview" ? "page" : (opts.ref == "image" ? "details" : "");
    std::string referer = m_settings->get("referer" + (opts.ref.empty() ? "" : "_" + opts.ref), def);
    if(referer.empty() && !opts.ref.empty()) referer = m_settings->get("referer", "none");
    if(!referer.empty() && referer != "none" && (referer != "page" || !opts.pageUrl.empty())) {
        std::string ref;
        if(referer == "host") ref = urlScheme(url) + "://" + urlHost(url);
        else if(referer == "image") ref = fixUrl(url);
        else if(referer == "page") ref = fixUrl(opts.pageUrl);
        else if(referer == "details") ref = fixUrl(opts.imagePageUrl);
        if(!ref.empty()) req.headers["Referer"] = ref;
    }
    if(m_login) m_login->complementHeaders(req.headers);
    for(auto& [k, v] : requestHeaders()) req.headers[k] = v;
    for(auto& [k, v] : opts.headers) req.headers[k] = startsWith(v, "md5:") ? md5Hex(v.substr(4)) : v;
    req.saveTo = opts.saveTo;
    req.progress = opts.progress;
    return req;
}

std::shared_ptr<net::Reply> Site::get(const std::string& url, QueryType type, net::Callback cb, RequestOptions opts) {
    return m_scheduler.enqueue((int)type, makeRequest(url, opts), std::move(cb));
}

std::shared_ptr<net::Reply> Site::post(const std::string& url, const std::string& body, const std::string& contentType, QueryType type,
                                       net::Callback cb, RequestOptions opts) {
    net::Request req = makeRequest(url, opts);
    req.method = "POST";
    req.body = body;
    req.contentType = contentType;
    return m_scheduler.enqueue((int)type, std::move(req), std::move(cb));
}

void Site::clearCookies() {
    net::Session::instance().clearCookies(host());
    for(auto& c : m_settings->getList("cookies")) net::Session::instance().addCookie(host(), c);
}

std::vector<Api*> Site::loggedInApis() const {
    std::vector<Api*> out;
    bool loggedIn = isLoggedIn(true);
    for(auto* a : m_apis)
        if(!a->needAuth() || loggedIn) out.push_back(a);
    return out;
}

Api* Site::firstValidApi() const {
    auto apis = loggedInApis();
    return apis.empty() ? nullptr : apis.front();
}

Api* Site::detailsApi() const {
    for(auto* a : m_apis)
        if(a->canLoadDetails()) return a;
    return nullptr;
}

Api* Site::fullDetailsApi() const {
    for(auto* a : m_apis)
        if(a->canLoadFullDetails()) return a;
    return nullptr;
}

Api* Site::tagsApi() const {
    for(auto* a : m_apis)
        if(a->canLoadTags()) return a;
    return nullptr;
}

Api* Site::tagTypesApi() const {
    for(auto* a : m_apis)
        if(a->canLoadTagTypes()) return a;
    return nullptr;
}

const ApiEndpoint* Site::endpoint(const std::string& name) const {
    for(auto* a : m_apis)
        if(auto* e = a->endpoint(name)) return e;
    return nullptr;
}

bool Site::canTestLogin() const { return m_auth && m_login && m_login->isTestable(); }

bool Site::hasCredentials() const {
    if(!m_auth) return false;
    auto get = [this](const std::string& k) { return setting(k); };
    for(auto& f : m_auth->fields)
        if((f.type == AuthField::Text || f.type == AuthField::Password) && !f.value(get).empty()) return true;
    for(auto& f : m_auth->settingFields())
        if(!setting("auth/" + f.id, f.def).empty()) return true;
    for(const char* k : {"auth/pseudo", "auth/password", "auth/accessToken", "auth/refreshToken"})
        if(!setting(k).empty()) return true;
    return false;
}

bool Site::isLoggedIn(bool unknown, bool pending) const {
    if(unknown) return m_loginStatus != LoginStatus::LoggedOut;
    if(pending && m_loginStatus == LoginStatus::Pending) return true;
    return m_loginStatus == LoginStatus::LoggedIn;
}

void Site::login(bool force, std::function<void(LoginResult)> done) {
    if(!force && m_loginStatus == LoginStatus::Pending) {
        if(done) m_loginWaiters.push_back(done);
        return;
    }
    if(!force && m_loginStatus != LoginStatus::Unknown) {
        if(done) done(LoginResult::Already);
        loggedIn.emit(LoginResult::Already);
        return;
    }
    // Automatic logins without any credentials would only send empty forms to the site
    if(!canTestLogin() || (!force && !hasCredentials())) {
        if(done) done(LoginResult::Impossible);
        loggedIn.emit(LoginResult::Impossible);
        return;
    }
    if(done) m_loginWaiters.push_back(done);
    log("[" + m_url + "] Logging in...", LogLevel::Info);
    if(force) clearCookies();
    m_loginStatus = LoginStatus::Pending;
    std::weak_ptr<Site> weak = weak_from_this();
    m_login->login([weak](bool ok) {
        if(auto s = weak.lock()) s->loginFinished(ok);
    });
}

void Site::loginFinished(bool ok) {
    m_loginStatus = ok ? LoginStatus::LoggedIn : LoginStatus::LoggedOut;
    log("[" + m_url + "] Login finished: " + (ok ? "success" : "failure"), ok ? LogLevel::Info : LogLevel::Warning);
    LoginResult r = ok ? LoginResult::Success : LoginResult::Error;
    auto waiters = std::move(m_loginWaiters);
    m_loginWaiters.clear();
    for(auto& w : waiters) w(r);
    loggedIn.emit(r);
}

void Site::loadTagTypes(std::function<void(bool)> done) {
    Api* api = tagTypesApi();
    if(!api) {
        done(false);
        return;
    }
    PageUrl url = api->tagTypesUrl(info());
    if(!url.error.empty()) {
        log("[" + m_url + "] " + url.error, LogLevel::Warning);
        done(false);
        return;
    }
    std::weak_ptr<Site> weak = weak_from_this();
    get(url.url, QueryType::Details, [weak, api, done](const net::Response& r) {
        auto self = weak.lock();
        if(!self) return;
        if(!r.ok() && !api->jsConst("tagTypes.parseErrors").toBool()) {
            done(false);
            return;
        }
        ParsedTagTypes types = api->parseTagTypes(r.body, r.status);
        if(!types.error.empty() || types.types.empty()) {
            log("[" + self->url() + "] Error loading tag types: " + types.error, LogLevel::Warning);
            done(false);
            return;
        }
        self->tagDatabase().setTagTypes(types.types);
        self->tagDatabase().save();
        done(true);
    });
}

}  // namespace bg
