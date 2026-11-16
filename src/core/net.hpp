// HTTP on libsoup 3: one shared session (cookies, cache, proxy) and per-site schedulers that
// apply the concurrency limit and the per-request-type throttling intervals.
#pragma once

#include <gio/gio.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "core/util.hpp"

typedef struct _SoupSession SoupSession;
typedef struct _SoupMessage SoupMessage;
typedef struct _SoupCookieJar SoupCookieJar;
typedef struct _SoupCache SoupCache;

namespace bg {
class Settings;
}

namespace bg::net {

enum class Error { None, Canceled, NotFound, HostNotFound, Timeout, Http, Connection, Write, Other };

struct Response {
    int status = 0;
    std::string body;
    std::string url;  // final URL after redirects
    std::string requestUrl;
    std::string error;
    Error code = Error::None;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string header(const std::string& name) const;
    bool ok() const { return code == Error::None; }
    bool isCloudflare() const;
};

using Progress = std::function<void(int64_t received, int64_t total)>;
using Callback = std::function<void(const Response&)>;

struct Request {
    std::string url;
    std::string method = "GET";
    std::string body;
    std::string contentType;
    StringMap headers;
    std::string saveTo;  // stream the body into this file instead of memory
    Progress progress;
};

class Reply : public std::enable_shared_from_this<Reply> {
public:
    Reply(Request req, Callback cb);
    ~Reply();
    void start();
    void abort();  // the callback will not be called
    bool running() const { return m_state == State::Running || m_state == State::Pending; }
    bool started() const { return m_state != State::Pending; }
    const Request& request() const { return m_request; }
    Signal<> finished;  // internal (scheduler)

private:
    enum class State { Pending, Running, Done, Aborted };
    static void onSent(GObject* src, GAsyncResult* res, gpointer data);
    static void onRead(GObject* src, GAsyncResult* res, gpointer data);
    void readNext();
    void complete(Error code, const std::string& error);

    Request m_request;
    Callback m_callback;
    State m_state = State::Pending;
    SoupMessage* m_msg = nullptr;
    GCancellable* m_cancel = nullptr;
    GInputStream* m_stream = nullptr;
    FILE* m_file = nullptr;
    Response m_response;
    int64_t m_received = 0, m_total = -1;
    std::shared_ptr<Reply> m_self;  // keeps the reply alive while running
};

class Session {
public:
    static Session& instance();
    void init(const std::string& cookieFile, const std::string& cacheDir);
    void applySettings(const Settings& settings);  // proxy and timeout
    void shutdown();
    SoupSession* soup();
    SoupCookieJar* cookieJar() const { return m_jar; }

    // Cookies
    void addCookie(const std::string& domain, const std::string& raw);  // "name=value; ..." header form
    void clearCookies(const std::string& domain);
    std::map<std::string, std::string> cookies(const std::string& url) const;

    // Unthrottled request
    std::shared_ptr<Reply> send(Request req, Callback cb);

private:
    Session() = default;
    SoupSession* m_session = nullptr;
    SoupCookieJar* m_jar = nullptr;
    SoupCache* m_cache = nullptr;
};

class Scheduler {
public:
    void setMaxConcurrency(int n) { m_max = std::max(1, n); }
    void setInterval(int type, int ms) { m_intervals[type] = ms; }
    std::shared_ptr<Reply> enqueue(int type, Request req, Callback cb);
    void clear();
    int active() const { return m_active; }
    int queued() const { return (int)m_queue.size(); }

private:
    void next();
    struct Entry {
        int type;
        std::shared_ptr<Reply> reply;
    };
    std::deque<Entry> m_queue;
    std::map<int, int> m_intervals;
    std::map<int, int64_t> m_last;
    int m_max = 10, m_active = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

public:
    ~Scheduler() { *m_alive = false; }
};

std::string encodeForm(const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace bg::net
