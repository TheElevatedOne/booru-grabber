#include "core/net.hpp"

#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "core/logger.hpp"
#include "core/settings.hpp"

namespace bg::net {

std::string Response::header(const std::string& name) const {
    auto it = headers.find(toLower(name));
    return it == headers.end() ? "" : it->second;
}

bool Response::isCloudflare() const {
    return (status == 403 || status == 429 || status == 503) && contains(toLower(header("server")), "cloudflare");
}

std::string encodeForm(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for(auto& [k, v] : fields) {
        if(!out.empty()) out += "&";
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

// ---- Reply -----------------------------------------------------------------

Reply::Reply(Request req, Callback cb) : m_request(std::move(req)), m_callback(std::move(cb)) {}

Reply::~Reply() {
    if(m_cancel) g_object_unref(m_cancel);
    if(m_stream) g_object_unref(m_stream);
    if(m_msg) g_object_unref(m_msg);
    if(m_file) fclose(m_file);
}

void Reply::start() {
    if(m_state != State::Pending) return;
    m_response.requestUrl = m_request.url;
    m_msg = soup_message_new(m_request.method.c_str(), m_request.url.c_str());
    if(!m_msg) {
        m_state = State::Running;
        m_self = shared_from_this();
        complete(Error::Other, "Invalid URL: " + m_request.url);
        return;
    }
    SoupMessageHeaders* h = soup_message_get_request_headers(m_msg);
    for(auto& [k, v] : m_request.headers) soup_message_headers_replace(h, k.c_str(), v.c_str());
    if(!m_request.body.empty() || m_request.method == "POST") {
        GBytes* b = g_bytes_new(m_request.body.data(), m_request.body.size());
        std::string ct = m_request.contentType.empty() ? "application/x-www-form-urlencoded" : m_request.contentType;
        soup_message_set_request_body_from_bytes(m_msg, ct.c_str(), b);
        g_bytes_unref(b);
    }
    if(!m_request.saveTo.empty()) {
        ensureParent(m_request.saveTo);
        m_file = g_fopen(m_request.saveTo.c_str(), "wb");
        if(!m_file) {
            m_state = State::Running;
            m_self = shared_from_this();
            complete(Error::Write, "Cannot write to " + m_request.saveTo);
            return;
        }
    }
    m_cancel = g_cancellable_new();
    m_state = State::Running;
    m_self = shared_from_this();
    soup_session_send_async(Session::instance().soup(), m_msg, G_PRIORITY_DEFAULT, m_cancel, onSent, this);
}

static Error mapError(GError* err) {
    if(!err) return Error::Other;
    if(g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return Error::Canceled;
    if(g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) return Error::Timeout;
    if(err->domain == G_RESOLVER_ERROR) return Error::HostNotFound;
    if(g_error_matches(err, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND) || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
       g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE))
        return Error::HostNotFound;
    return Error::Connection;
}

void Reply::onSent(GObject* src, GAsyncResult* res, gpointer data) {
    auto* self = static_cast<Reply*>(data);
    GError* err = nullptr;
    GInputStream* stream = soup_session_send_finish(SOUP_SESSION(src), res, &err);
    if(self->m_state == State::Aborted) {
        if(stream) g_object_unref(stream);
        if(err) g_error_free(err);
        self->m_self.reset();
        return;
    }
    if(!stream) {
        Error code = mapError(err);
        std::string msg = err ? err->message : "Unknown network error";
        if(err) g_error_free(err);
        self->complete(code, msg);
        return;
    }
    self->m_stream = stream;
    Response& r = self->m_response;
    r.status = (int)soup_message_get_status(self->m_msg);
    GUri* uri = soup_message_get_uri(self->m_msg);
    gchar* u = g_uri_to_string(uri);
    r.url = u;
    g_free(u);
    SoupMessageHeaders* rh = soup_message_get_response_headers(self->m_msg);
    SoupMessageHeadersIter it;
    soup_message_headers_iter_init(&it, rh);
    const char *name, *value;
    while(soup_message_headers_iter_next(&it, &name, &value)) {
        std::string key = toLower(name);
        auto existing = r.headers.find(key);
        if(existing != r.headers.end())
            existing->second += ", " + std::string(value);
        else
            r.headers[key] = value;
    }
    goffset len = soup_message_headers_get_content_length(rh);
    self->m_total = len > 0 ? len : -1;
    self->readNext();
}

void Reply::readNext() {
    g_input_stream_read_bytes_async(m_stream, 64 * 1024, G_PRIORITY_DEFAULT, m_cancel, onRead, this);
}

void Reply::onRead(GObject* src, GAsyncResult* res, gpointer data) {
    auto* self = static_cast<Reply*>(data);
    GError* err = nullptr;
    GBytes* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(src), res, &err);
    if(self->m_state == State::Aborted) {
        if(bytes) g_bytes_unref(bytes);
        if(err) g_error_free(err);
        self->m_self.reset();
        return;
    }
    if(!bytes) {
        Error code = mapError(err);
        std::string msg = err ? err->message : "Read error";
        if(err) g_error_free(err);
        self->complete(code, msg);
        return;
    }
    gsize n = 0;
    const char* d = (const char*)g_bytes_get_data(bytes, &n);
    if(n == 0) {
        g_bytes_unref(bytes);
        Response& r = self->m_response;
        Error code = Error::None;
        std::string msg;
        if(r.status == 404 || r.status == 410) {
            code = Error::NotFound;
            msg = "HTTP " + std::to_string(r.status);
        } else if(r.status >= 400) {
            code = Error::Http;
            msg = "HTTP " + std::to_string(r.status) + " " + soup_status_get_phrase((guint)r.status);
        }
        self->complete(code, msg);
        return;
    }
    if(self->m_file) {
        if(fwrite(d, 1, n, self->m_file) != n) {
            g_bytes_unref(bytes);
            self->complete(Error::Write, "Error writing " + self->m_request.saveTo);
            return;
        }
    } else {
        self->m_response.body.append(d, n);
    }
    g_bytes_unref(bytes);
    self->m_received += (int64_t)n;
    if(self->m_request.progress) self->m_request.progress(self->m_received, self->m_total);
    if(self->m_state == State::Running) self->readNext();
}

void Reply::complete(Error code, const std::string& error) {
    if(m_state != State::Running) return;
    m_state = State::Done;
    if(m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    m_response.code = code;
    m_response.error = error;
    if(m_response.url.empty()) m_response.url = m_request.url;
    auto keep = m_self;  // stay alive through the callbacks
    m_self.reset();
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    if(cb) cb(m_response);
    finished.emit();
}

void Reply::abort() {
    if(m_state == State::Done || m_state == State::Aborted) return;
    bool wasRunning = m_state == State::Running;
    m_state = State::Aborted;
    m_callback = nullptr;
    if(m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    if(wasRunning && m_cancel) g_cancellable_cancel(m_cancel);  // m_self released in the async handler
    auto keep = shared_from_this();
    finished.emit();
}

// ---- Session -----------------------------------------------------------------

Session& Session::instance() {
    static Session s;
    return s;
}

SoupSession* Session::soup() {
    if(!m_session) init("", "");
    return m_session;
}

void Session::init(const std::string& cookieFile, const std::string& cacheDir) {
    if(m_session) return;
    m_session = soup_session_new_with_options("max-conns", 64, "max-conns-per-host", 8, "timeout", 60, "idle-timeout", 60, nullptr);
    if(!cookieFile.empty()) {
        ensureParent(cookieFile);
        m_jar = soup_cookie_jar_text_new(cookieFile.c_str(), FALSE);
    } else {
        m_jar = soup_cookie_jar_new();
    }
    soup_cookie_jar_set_accept_policy(m_jar, SOUP_COOKIE_JAR_ACCEPT_ALWAYS);
    soup_session_add_feature(m_session, SOUP_SESSION_FEATURE(m_jar));
    if(!cacheDir.empty()) {
        mkpath(cacheDir);
        m_cache = soup_cache_new(cacheDir.c_str(), SOUP_CACHE_SINGLE_USER);
        soup_cache_set_max_size(m_cache, 64 * 1024 * 1024);
        soup_cache_load(m_cache);
        soup_session_add_feature(m_session, SOUP_SESSION_FEATURE(m_cache));
    }
}

void Session::applySettings(const Settings& s) {
    SoupSession* session = soup();
    if(!s.getBool("Proxy/use", false)) {
        GProxyResolver* direct = g_simple_proxy_resolver_new(nullptr, nullptr);
        soup_session_set_proxy_resolver(session, direct);
        g_object_unref(direct);
    } else if(s.getBool("Proxy/useSystem", false)) {
        soup_session_set_proxy_resolver(session, g_proxy_resolver_get_default());
    } else {
        std::string type = toLower(s.get("Proxy/type", "http"));
        std::string scheme = type == "socks5" || type == "socks" ? "socks5" : "http";
        std::string user = s.get("Proxy/user"), pass = s.get("Proxy/password");
        std::string auth = user.empty() ? "" : urlEncode(user) + (pass.empty() ? "" : ":" + urlEncode(pass)) + "@";
        std::string uri = scheme + "://" + auth + s.get("Proxy/hostName") + ":" + std::to_string(s.getInt("Proxy/port", 8080));
        GProxyResolver* r = g_simple_proxy_resolver_new(uri.c_str(), nullptr);
        soup_session_set_proxy_resolver(session, r);
        g_object_unref(r);
    }
    int timeout = (int)s.getInt("Network/timeout", 60);
    soup_session_set_timeout(session, (guint)std::max(0, timeout));
}

void Session::shutdown() {
    if(m_cache) soup_cache_dump(m_cache);
}

void Session::addCookie(const std::string& domain, const std::string& raw) {
    soup();
    std::string base = "https://" + domain + "/";
    GUri* origin = g_uri_parse(base.c_str(), G_URI_FLAGS_NONE, nullptr);
    for(auto& part : split(raw, '\n', true)) {
        SoupCookie* c = soup_cookie_parse(trim(part).c_str(), origin);
        if(!c) continue;
        if(!soup_cookie_get_domain(c) || !*soup_cookie_get_domain(c)) soup_cookie_set_domain(c, ("." + domain).c_str());
        soup_cookie_set_path(c, "/");
        soup_cookie_jar_add_cookie(m_jar, c);  // takes ownership
    }
    if(origin) g_uri_unref(origin);
}

void Session::clearCookies(const std::string& domain) {
    soup();
    GSList* all = soup_cookie_jar_all_cookies(m_jar);
    for(GSList* l = all; l; l = l->next) {
        auto* c = (SoupCookie*)l->data;
        if(soup_cookie_domain_matches(c, domain.c_str())) soup_cookie_jar_delete_cookie(m_jar, c);
    }
    g_slist_free_full(all, (GDestroyNotify)soup_cookie_free);
}

std::map<std::string, std::string> Session::cookies(const std::string& url) const {
    std::map<std::string, std::string> out;
    if(!m_jar) return out;
    GUri* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
    if(!uri) return out;
    GSList* list = soup_cookie_jar_get_cookie_list(m_jar, uri, TRUE);
    for(GSList* l = list; l; l = l->next) {
        auto* c = (SoupCookie*)l->data;
        out[soup_cookie_get_name(c)] = soup_cookie_get_value(c);
    }
    g_slist_free_full(list, (GDestroyNotify)soup_cookie_free);
    g_uri_unref(uri);
    return out;
}

std::shared_ptr<Reply> Session::send(Request req, Callback cb) {
    auto r = std::make_shared<Reply>(std::move(req), std::move(cb));
    r->start();
    return r;
}

// ---- Scheduler ---------------------------------------------------------------

std::shared_ptr<Reply> Scheduler::enqueue(int type, Request req, Callback cb) {
    auto reply = std::make_shared<Reply>(std::move(req), std::move(cb));
    m_queue.push_back({type, reply});
    std::weak_ptr<bool> alive = m_alive;
    defer([this, alive] {
        if(alive.lock()) next();
    });
    return reply;
}

void Scheduler::clear() {
    for(auto& e : m_queue) e.reply->abort();
    m_queue.clear();
}

void Scheduler::next() {
    while(m_active < m_max && !m_queue.empty()) {
        Entry e = m_queue.front();
        m_queue.pop_front();
        if(e.reply->started() || !e.reply->running()) continue;  // aborted while queued
        int64_t now = nowMs();
        int interval = m_intervals.count(e.type) ? m_intervals[e.type] : 0;
        int64_t last = m_last.count(e.type) ? m_last[e.type] : 0;
        int64_t wait = std::max<int64_t>(0, last + interval - now);
        m_last[e.type] = now + wait;
        m_active++;
        std::weak_ptr<bool> alive = m_alive;
        std::shared_ptr<Reply> reply = e.reply;  // kept until started, even if the caller dropped it
        auto done = std::make_shared<bool>(false);
        auto release = [this, alive, done] {
            if(*done || !alive.lock()) return;
            *done = true;
            m_active--;
            next();
        };
        e.reply->finished.connect(release);
        after((unsigned)wait, [reply, release] {
            if(reply->running() && !reply->started())
                reply->start();
            else
                release();
        });
    }
}

}  // namespace bg::net
