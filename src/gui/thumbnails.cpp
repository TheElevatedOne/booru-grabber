#include "gui/thumbnails.hpp"

#include "core/logger.hpp"
#include "core/site.hpp"

namespace bg::gui {

static const size_t kMaxCached = 800;

void ThumbnailCache::store(const std::string& key, GdkPixbuf* pb) {
    m_cache[key] = PixbufRef(pb, false);
    m_order.push_back(key);
    while(m_order.size() > kMaxCached) {
        m_cache.erase(m_order.front());
        m_order.pop_front();
    }
}

void ThumbnailCache::clear() {
    m_cache.clear();
    m_order.clear();
}

std::shared_ptr<void> ThumbnailCache::request(const ImagePtr& img, int size, Callback cb) {
    auto handle = std::make_shared<int>(0);
    const ImageSize& media = img->mediaForSize(size * 2, size * 2, true);
    std::string url = media.url.empty() ? img->url(SizeType::Thumbnail) : media.url;
    if(url.empty() || !img->site()) {
        cb(nullptr);
        return handle;
    }
    std::string key = url + "@" + std::to_string(size);
    auto it = m_cache.find(key);
    if(it != m_cache.end()) {
        cb(it->second.p);
        return handle;
    }
    auto pending = m_pending.find(key);
    if(pending != m_pending.end()) {
        pending->second.waiters.emplace_back(handle, cb);
        return handle;
    }
    Pending& p = m_pending[key];
    p.size = size;
    p.rect = media.rect;
    p.waiters.emplace_back(handle, cb);
    RequestOptions opts;
    opts.pageUrl = img->parentUrl();
    opts.ref = "preview";
    p.reply = img->site()->get(url, QueryType::Thumbnail, [this, key](const net::Response& r) {
        auto it = m_pending.find(key);
        if(it == m_pending.end()) return;
        Pending pending = std::move(it->second);
        m_pending.erase(it);
        GdkPixbuf* pb = nullptr;
        if(r.ok()) {
            GdkPixbuf* full = pixbufFromData(r.body);
            if(full && !pending.rect.empty()) {
                int w = gdk_pixbuf_get_width(full), h = gdk_pixbuf_get_height(full);
                Rect rc = pending.rect;
                if(rc.x >= 0 && rc.y >= 0 && rc.x + rc.w <= w && rc.y + rc.h <= h) {
                    GdkPixbuf* sub = gdk_pixbuf_new_subpixbuf(full, rc.x, rc.y, rc.w, rc.h);
                    GdkPixbuf* copy = gdk_pixbuf_copy(sub);
                    g_object_unref(sub);
                    g_object_unref(full);
                    full = copy;
                }
            }
            if(full) {
                pb = scaleToFit(full, pending.size, pending.size);
                g_object_unref(full);
            }
        } else if(r.code != net::Error::Canceled) {
            log("Error loading thumbnail " + r.url + ": " + r.error, LogLevel::Warning);
        }
        if(pb) store(key, pb);
        for(auto& [weak, waiter] : pending.waiters)
            if(weak.lock()) waiter(pb);
        if(pb) g_object_unref(pb);
    }, opts);
    return handle;
}

}  // namespace bg::gui
