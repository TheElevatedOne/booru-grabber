// Loads and caches thumbnails of images through their site (throttled like the original).
#pragma once

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/image.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class ThumbnailCache {
public:
    using Callback = std::function<void(GdkPixbuf*)>;  // borrowed reference, nullptr on failure

    // Returns a handle; dropping it cancels the callback (not the shared download)
    std::shared_ptr<void> request(const ImagePtr& img, int size, Callback cb);
    void clear();

private:
    struct Pending {
        std::shared_ptr<net::Reply> reply;
        std::vector<std::pair<std::weak_ptr<void>, Callback>> waiters;
        int size;
        Rect rect;
    };
    void store(const std::string& key, GdkPixbuf* pb);
    std::map<std::string, PixbufRef> m_cache;
    std::list<std::string> m_order;
    std::map<std::string, Pending> m_pending;
};

}  // namespace bg::gui
