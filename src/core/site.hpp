// A site (e.g. danbooru.donmai.us) using a Source, with its own settings, login and throttling.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/net.hpp"
#include "core/settings.hpp"
#include "core/source.hpp"
#include "core/tags.hpp"

namespace bg {

class Profile;
class Site;
class Page;

enum class QueryType { Unknown = -1, List = 0, Img = 1, Thumbnail = 2, Details = 3, Retry = 4 };
enum class LoginResult { Success, Error, Impossible, Already };

struct RequestOptions {
    std::string pageUrl;       // page the request comes from (referer "page")
    std::string ref;           // "", "preview", "image"
    std::string imagePageUrl;  // details page of the image (referer "details")
    StringMap headers;
    std::string saveTo;
    net::Progress progress;
    bool autoLogin = true;
};

class Login {
public:
    virtual ~Login() = default;
    virtual bool isTestable() const = 0;
    virtual void login(std::function<void(bool)> done) = 0;
    virtual void complementHeaders(StringMap&) {}
    virtual std::string complementUrl(const std::string& url) const { return url; }
    virtual void abort() {}
};

class Site : public std::enable_shared_from_this<Site> {
public:
    enum class LoginStatus { Unknown, Pending, LoggedOut, LoggedIn };

    Site(std::string url, Source* source, Profile* profile);
    ~Site();
    void loadConfig();

    const std::string& url() const { return m_url; }
    const std::string& name() const { return m_name; }
    std::string type() const { return m_source->name(); }
    std::string baseUrl() const;
    std::string host() const;
    const std::string& userAgent() const { return m_userAgent; }
    Source* source() const { return m_source; }
    Profile* profile() const { return m_profile; }
    Settings& settings() { return *m_settings; }
    const Settings& settings() const { return *m_settings; }
    std::string setting(const std::string& key, const std::string& def = "") const { return m_settings->get(key, def); }
    TagDatabase& tagDatabase() { return *m_tagDatabase; }
    const std::string& configDir() const { return m_configDir; }
    const std::string& dataDir() const { return m_dataDir; }

    // Network
    std::shared_ptr<net::Reply> get(const std::string& url, QueryType type, net::Callback cb, RequestOptions opts = {});
    std::shared_ptr<net::Reply> post(const std::string& url, const std::string& body, const std::string& contentType, QueryType type,
                                     net::Callback cb, RequestOptions opts = {});
    std::string fixUrl(const std::string& url, const std::string& old = "") const;
    std::string fixLoginUrl(const std::string& url) const;
    SiteInfo info() const;
    StringMap requestHeaders() const;  // custom headers + User-Agent
    void clearCookies();

    // APIs
    const std::vector<Api*>& apis() const { return m_apis; }
    std::vector<Api*> loggedInApis() const;
    Api* firstValidApi() const;
    Api* detailsApi() const;
    Api* fullDetailsApi() const;
    Api* tagsApi() const;
    Api* tagTypesApi() const;
    const ApiEndpoint* endpoint(const std::string& name) const;

    // Login
    Auth* auth() const { return m_auth.get(); }
    void login(bool force = false, std::function<void(LoginResult)> done = nullptr);
    bool isLoggedIn(bool unknown = false, bool pending = false) const;
    LoginStatus loginStatus() const { return m_loginStatus; }
    bool canTestLogin() const;
    bool hasCredentials() const;  // the user set a login, key or token for this site
    void setAutoLogin(bool a) { m_autoLogin = a; }
    bool autoLogin() const { return m_autoLogin; }

    // Tag types loading (for sources that need them)
    void loadTagTypes(std::function<void(bool)> done);

    Signal<LoginResult> loggedIn;
    Signal<> configChanged;

private:
    net::Request makeRequest(std::string url, const RequestOptions& opts);
    void loginFinished(bool ok);

    std::string m_url, m_name, m_userAgent;
    Source* m_source;
    Profile* m_profile;
    std::string m_readDir, m_configDir, m_dataDir;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<TagDatabase> m_tagDatabase;
    std::vector<Api*> m_apis;
    std::shared_ptr<Auth> m_auth;
    std::unique_ptr<Login> m_login;
    LoginStatus m_loginStatus = LoginStatus::Unknown;
    bool m_autoLogin = true;
    std::vector<std::function<void(LoginResult)>> m_loginWaiters;
    net::Scheduler m_scheduler;
};

// Handles OAuth2 redirect URLs received through a custom URL scheme (e.g. pixiv://)
bool handleOAuthCallback(const std::string& url);

}  // namespace bg
