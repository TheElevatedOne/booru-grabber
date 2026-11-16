#include "core/page.hpp"

#include <cmath>

#include "core/logger.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"

namespace bg {

// ---- SearchQuery -------------------------------------------------------------

std::string SearchQuery::toString() const {
    if(gallery) return "Gallery: " + gallery->name();
    return join(tags, " ");
}

nlohmann::json SearchQuery::toJson() const {
    nlohmann::json j = nlohmann::json::object();
    if(!endpoint.empty()) j["endpoint"] = endpoint;
    if(!tags.empty()) j["tags"] = tags;
    if(gallery) j["gallery"] = gallery->write();
    if(input.is_object() && !input.empty()) j["input"] = input;
    return j;
}

void SearchQuery::fromJson(const nlohmann::json& j, Profile* profile) {
    if(!j.is_object()) return;
    endpoint = j.value("endpoint", "");
    tags.clear();
    if(j.contains("tags") && j["tags"].is_array())
        for(auto& t : j["tags"])
            if(t.is_string()) tags.push_back(t.get<std::string>());
    if(j.contains("gallery") && j["gallery"].is_object() && profile) {
        auto g = std::make_shared<Image>(profile);
        if(g->read(j["gallery"])) gallery = g;
    }
    if(j.contains("input") && j["input"].is_object()) input = j["input"];
}

bool SearchQuery::operator==(const SearchQuery& o) const {
    return tags == o.tags && gallery == o.gallery && endpoint == o.endpoint && input == o.input;
}

// ---- Page ----------------------------------------------------------------------

Page::Page(Profile* profile, std::shared_ptr<Site> site, const std::vector<Site*>& sites, SearchQuery query, int page, int limit,
           const StringList& postFilters, bool smart, const PageInformation& last)
    : m_profile(profile), m_site(std::move(site)), m_query(std::move(query)), m_page(page), m_limit(limit) {
    // Site-level added/removed tags
    if(!m_query.gallery) {
        for(auto& t : split(m_site->setting("added_tags"), ' ', true))
            if(!contains(m_query.tags, t)) m_query.tags.push_back(t);
        for(auto& t : split(m_site->setting("removed_tags"), ' ', true)) removeAll(m_query.tags, t);
    }
    if(!m_query.tags.empty()) {
        // Normalize rating shortcuts
        StringList tags;
        static const std::map<std::string, std::string> ratings{{"s", "safe"}, {"g", "general"}, {"q", "questionable"}, {"e", "explicit"}};
        for(auto t : m_query.tags) {
            std::string lower = toLower(t);
            for(auto& [k, v] : ratings) {
                if(lower == "rating:" + k) t = "rating:" + v;
                if(lower == "-rating:" + k) t = "-rating:" + v;
            }
            tags.push_back(t);
        }
        // Remove modifiers supported by other selected sites but not by this one
        StringList modifiers;
        for(Site* s : sites)
            if(!s->apis().empty())
                for(auto& m : s->apis().front()->modifiers()) modifiers.push_back(m);
        if(!m_site->apis().empty())
            for(auto& m : m_site->apis().front()->modifiers()) removeAll(modifiers, m);
        for(auto& mod : modifiers) {
            size_t before = tags.size();
            removeAll(tags, mod);
            if(tags.size() != before) log("[" + m_site->url() + "] Unsupported modifier removed from search: " + mod, LogLevel::Warning);
        }
        m_search = tags;
        m_query.tags = tags;
    }

    PostFilter postFilter(postFilters, &profile->settings());
    auto apis = m_site->loggedInApis();
    for(Api* api : apis) {
        if(!m_query.endpoint.empty() && !api->endpoint(m_query.endpoint)) continue;
        m_apis.push_back(std::make_unique<PageApi>(this, profile, m_site, api, m_query, page, limit, postFilter, smart, last));
        if(api->name() == "Html" && m_htmlApi < 0) m_htmlApi = (int)m_apis.size() - 1;
    }
}

Page::~Page() {
    for(auto& a : m_apis) a->abort();
}

PageApi* Page::current() const {
    if(m_apis.empty()) return nullptr;
    return m_apis[(size_t)std::clamp(m_current, 0, (int)m_apis.size() - 1)].get();
}

void Page::load(Done done, bool rateLimit) {
    m_done = std::move(done);
    m_current = 0;
    m_errors.clear();
    if(m_apis.empty()) {
        log("[" + m_site->url() + "] No available API to perform the request", LogLevel::Error);
        m_errors.push_back("No available API to perform the request.");
        auto self = shared_from_this();
        Done d = std::move(m_done);
        if(d) d(this, false);
        return;
    }
    tryCurrent(rateLimit);
}

void Page::tryCurrent(bool rateLimit) {
    std::weak_ptr<Page> weak = weak_from_this();
    PageApi* api = m_apis[(size_t)m_current].get();
    api->load([weak, api](bool ok) {
        auto self = weak.lock();
        if(!self) return;
        if(ok) {
            Done d = std::move(self->m_done);
            if(d) d(self.get(), true);
            return;
        }
        for(auto& e : api->errors()) self->m_errors.push_back(e);
        if(self->m_current + 1 < (int)self->m_apis.size()) {
            log("[" + self->m_site->url() + "] Loading using " + api->api()->name() + " failed. Retry using " +
                    self->m_apis[(size_t)self->m_current + 1]->api()->name() + ".",
                LogLevel::Warning);
            self->m_current++;
            self->tryCurrent(false);
            return;
        }
        log("[" + self->m_site->url() + "] No valid source of the site returned result.", LogLevel::Warning);
        self->m_errors.push_back("No valid source of the site returned result.");
        Done d = std::move(self->m_done);
        if(d) d(self.get(), false);
    }, rateLimit);
}

void Page::abort() {
    for(auto& a : m_apis) a->abort();
    m_done = nullptr;
}

void Page::setLastPage(const PageInformation& info) {
    for(auto& a : m_apis) a->setLastPage(info);
}

PageInformation Page::pageInformation() const {
    PageApi* c = current();
    return c ? c->pageInformation() : PageInformation{};
}

static const std::vector<ImagePtr> kNoImages;
static const TagList kNoTags;
static const std::string kEmpty;

const std::vector<ImagePtr>& Page::images() const {
    PageApi* c = current();
    return c ? c->images() : kNoImages;
}

const TagList& Page::tags() const {
    if(m_htmlApi >= 0 && !m_apis[(size_t)m_htmlApi]->tags().empty()) return m_apis[(size_t)m_htmlApi]->tags();
    PageApi* c = current();
    return c ? c->tags() : kNoTags;
}

const std::string& Page::wiki() const {
    if(m_htmlApi >= 0 && !m_apis[(size_t)m_htmlApi]->wiki().empty()) return m_apis[(size_t)m_htmlApi]->wiki();
    PageApi* c = current();
    return c ? c->wiki() : kEmpty;
}

std::string Page::url() const {
    PageApi* c = current();
    return c ? c->url() : "";
}

std::map<std::string, std::string> Page::urls() const {
    std::map<std::string, std::string> out;
    for(auto& a : m_apis)
        if(!a->url().empty()) out[a->api()->name()] = a->url();
    return out;
}

int Page::imagesCount(bool guess) const {
    PageApi* c = current();
    if(!c) return -1;
    if(m_htmlApi >= 0 && !c->isImageCountSure()) {
        int n = m_apis[(size_t)m_htmlApi]->imagesCount(guess);
        if(n >= 0) return n;
    }
    return c->imagesCount(guess);
}

int Page::maxImagesCount() const {
    PageApi* c = current();
    if(!c) return -1;
    if(m_htmlApi >= 0 && !c->isImageCountSure()) {
        int n = m_apis[(size_t)m_htmlApi]->maxImagesCount();
        if(n >= 0) return n;
    }
    return c->maxImagesCount();
}

int Page::pagesCount(bool guess) const {
    PageApi* c = current();
    if(!c) return -1;
    if(m_htmlApi >= 0 && !c->isPageCountSure()) {
        int n = m_apis[(size_t)m_htmlApi]->pagesCount(guess);
        if(n >= 0) return n;
    }
    return c->pagesCount(guess);
}

int Page::maxPagesCount() const {
    PageApi* c = current();
    if(!c) return -1;
    if(m_htmlApi >= 0 && !c->isPageCountSure()) {
        int n = m_apis[(size_t)m_htmlApi]->maxPagesCount();
        if(n >= 0) return n;
    }
    return c->maxPagesCount();
}

bool Page::hasNext() const {
    PageApi* c = current();
    return c && c->hasNext();
}
bool Page::isLoaded() const {
    PageApi* c = current();
    return c && c->isLoaded();
}
int Page::pageImageCount() const {
    PageApi* c = current();
    return c ? c->pageImageCount() : 0;
}
int Page::filteredImageCount() const {
    PageApi* c = current();
    return c ? c->filteredImageCount() : 0;
}
int Page::highLimit() const {
    PageApi* c = current();
    return c ? c->highLimit() : 0;
}
std::string Page::apiName() const {
    PageApi* c = current();
    return c ? c->api()->name() : "";
}

// ---- PageApi ---------------------------------------------------------------------

PageApi::PageApi(Page* parent, Profile* profile, std::shared_ptr<Site> site, Api* api, const SearchQuery& query, int page, int limit,
                 PostFilter postFilter, bool smart, const PageInformation& last)
    : m_parent(parent), m_profile(profile), m_site(std::move(site)), m_api(api), m_query(query), m_page(page), m_limit(limit),
      m_postFilter(std::move(postFilter)), m_smart(smart), m_last(last) {
    updateUrls();
}

PageApi::~PageApi() {
    if(m_reply) m_reply->abort();
}

int PageApi::perPage() const {
    int forced = m_api->forcedLimit();
    return forced > 0 ? forced : m_limit;
}

void PageApi::updateUrls() {
    std::string url;
    m_errors.clear();
    auto it = m_query.urls.find(m_api->name());
    if(it != m_query.urls.end()) {
        url = it->second;
    } else if(m_query.tags.size() == 1 && isUrl(m_query.tags[0])) {
        url = m_query.tags[0];
    } else if(!m_url.empty()) {
        url = m_url;
    } else {
        SiteInfo info = m_site->info();
        PageUrl ret;
        if(const ApiEndpoint* ep = m_api->endpoint(m_query.endpoint)) {
            ret = ep->url(m_query.input, m_page, m_limit, m_last, info);
        } else if(m_query.gallery) {
            ret = m_api->galleryUrl(std::to_string(m_query.gallery->id()), m_query.gallery->md5(), m_query.gallery->identity(), m_page, m_limit, info);
        } else {
            ret = m_api->pageUrl(join(m_query.tags, " "), m_page, m_limit, m_last, info);
        }
        if(!ret.error.empty()) m_errors.push_back(ret.error);
        url = ret.url;
        m_headers = ret.headers;
        m_isPost = ret.isPost;
        m_payload = ret.payload;
    }
    url = m_site->fixUrl(url);
    m_originalUrl = url;
    m_url = url;
}

void PageApi::setLastPage(const PageInformation& info) {
    m_last = info;
    if(!info.nextPage.empty() && info.page == m_page - 1)
        m_url = info.nextPage;
    else if(!info.previousPage.empty() && info.page == m_page + 1)
        m_url = info.previousPage;
    else
        m_url.clear();
    updateUrls();
}

PageInformation PageApi::pageInformation() const {
    PageInformation ret;
    ret.page = m_page;
    for(auto& img : m_images) {
        if(img->id() > ret.maxId || ret.maxId == 0) ret.maxId = img->id();
        if(img->id() < ret.minId || ret.minId == 0) ret.minId = img->id();
        std::string d = img->dateRaw();
        if(d > ret.maxDate || ret.maxDate.empty()) ret.maxDate = d;
        if(d < ret.minDate || ret.minDate.empty()) ret.minDate = d;
    }
    ret.previousPage = m_urlPrevPage;
    ret.nextPage = m_urlNextPage;
    return ret;
}

void PageApi::abort() {
    if(m_reply) m_reply->abort();
    m_reply.reset();
    m_loading = false;
    m_done = nullptr;
}

void PageApi::load(std::function<void(bool)> done, bool rateLimit) {
    m_done = std::move(done);
    if(m_url.empty()) {
        for(auto& e : m_errors) log("[" + m_site->url() + "][" + m_api->name() + "] " + e, LogLevel::Warning);
        if(m_errors.empty()) m_errors.push_back("No URL for this request");
        finish(false);
        return;
    }
    m_images.clear();
    m_tags.clear();
    m_loaded = false;
    m_loading = true;
    m_pageImageCount = m_filteredImageCount = 0;
    m_imagesCount = m_maxImagesCount = m_pagesCount = -1;
    m_imagesCountSafe = m_pagesCountSafe = false;
    log("[" + m_site->url() + "][" + m_api->name() + "] Loading page `" + m_url + "`", LogLevel::Info);
    QueryType type = rateLimit ? QueryType::Retry : QueryType::List;
    Page* parent = m_parent;
    std::weak_ptr<Page> weak = parent->weak_from_this();
    auto cb = [this, weak](const net::Response& r) {
        if(!weak.lock()) return;
        parse(r);
    };
    if(m_reply) m_reply->abort();
    RequestOptions opts;
    opts.headers = m_headers;
    m_reply = m_isPost ? m_site->post(m_url, m_payload, m_headers.count("Content-Type") ? m_headers["Content-Type"] : "", type, cb, opts)
                       : m_site->get(m_url, type, cb, opts);
}

void PageApi::finish(bool ok) {
    m_loaded = true;
    m_loading = false;
    m_reply.reset();
    auto done = std::move(m_done);
    m_done = nullptr;
    if(done) done(ok);
}

void PageApi::parse(const net::Response& r) {
    const ApiEndpoint* endpoint = m_api->endpoint(m_query.endpoint);
    bool isGallery = (bool)m_query.gallery;
    bool parseErrors = endpoint ? endpoint->parseErrors() : (isGallery ? m_api->parseGalleryErrors() : m_api->parsePageErrors());
    log("[" + m_site->url() + "][" + m_api->name() + "] Receiving page `" + r.url + "`", LogLevel::Info);

    if(r.isCloudflare()) {
        m_errors.push_back("Cloudflare wall");
        log("[" + m_site->url() + "][" + m_api->name() + "] Cloudflare wall for '" + r.url + "'", LogLevel::Error);
        finish(false);
        return;
    }
    if((r.status == 429 || r.status == 503 || r.status == 509) && m_retries < 3) {
        m_retries++;
        log("[" + m_site->url() + "][" + m_api->name() + "] Limit reached (" + std::to_string(r.status) + "). New try.", LogLevel::Warning);
        auto done = std::move(m_done);
        load(done, true);
        return;
    }
    m_retries = 0;
    if(r.body.empty() || (!r.ok() && !parseErrors)) {
        if(r.code != net::Error::Canceled) {
            std::string err = r.error.empty() ? "Empty response" : r.error;
            log("[" + m_site->url() + "][" + m_api->name() + "] Loading error: " + err, LogLevel::Error);
            m_errors.push_back(err);
        }
        finish(false);
        return;
    }

    int offset = (m_page - 1) * m_limit;
    SiteInfo info = m_site->info();
    ParsedPage page = endpoint ? endpoint->parse(r.body, r.status, offset, info)
                               : (isGallery ? m_api->parseGallery(r.body, r.status, offset, info) : m_api->parsePage(r.body, r.status, offset, info));
    if(!page.error.empty()) {
        m_errors.push_back(page.error);
        log("[" + m_site->url() + "][" + m_api->name() + "] " + page.error, LogLevel::Warning);
        finish(false);
        return;
    }

    m_tags = page.tags;
    std::string pageUrl = m_url;
    for(auto& raw : page.images) {
        auto img = std::make_shared<Image>(m_site, raw, m_profile, m_query.tags, pageUrl);
        m_pageImageCount++;
        StringList filters = m_postFilter.match(img->tokens());
        if(!filters.empty()) {
            m_filteredImageCount++;
            log("[" + m_site->url() + "][" + m_api->name() + "] Image filtered. Reason: " + join(filters, ", ") + ".", LogLevel::Info);
            continue;
        }
        if(isGallery) img->setParentGallery(m_query.gallery);
        m_images.push_back(img);
    }
    if(page.pageCount >= 0) setPageCount(page.pageCount, true);
    if(page.imageCount >= 0) setImageCount(page.imageCount, true);
    if(!page.urlNextPage.empty()) m_urlNextPage = page.urlNextPage;
    if(!page.urlPrevPage.empty()) m_urlPrevPage = page.urlPrevPage;
    if(!page.wiki.empty()) m_wiki = page.wiki;

    // Image count from the tag counts of the search
    if(m_imagesCount < 1 || !m_imagesCountSafe) {
        int found = 0, min = -1;
        for(auto& tag : m_tags) {
            if(contains(m_query.tags, tag.text())) {
                found++;
                if(min == -1 || min > tag.count()) min = tag.count();
            }
        }
        int searchTagsCount = (int)m_query.tags.size();
        if((int)m_query.tags.size() > found) {
            StringList modifiers = m_api->modifiers();
            modifiers.insert(modifiers.begin(), "-");
            for(auto& s : m_query.tags)
                for(auto& mod : modifiers)
                    if(!mod.empty() && startsWith(s, mod)) {
                        searchTagsCount--;
                        break;
                    }
        }
        if(searchTagsCount == found && found > 0) {
            if(m_query.tags.size() == 1) {
                int expected = (int)std::ceil((double)min / perPage());
                setImageCount(min, m_pagesCountSafe && expected == m_pagesCount);
            }
            m_maxImagesCount = min;
        }
    }

    // Tags from the images when the page has none
    if(m_tags.empty()) {
        std::map<std::string, size_t> index;
        for(auto& img : m_images) {
            for(auto& tag : img->tags()) {
                auto it = index.find(tag.text());
                if(it != index.end()) {
                    m_tags[it->second].setCount(m_tags[it->second].count() + 1);
                } else {
                    index[tag.text()] = m_tags.size();
                    Tag t = tag;
                    t.setCount(1);
                    m_tags.push_back(t);
                }
            }
        }
    }

    // Skip the first images (site setting)
    int skip = (int)m_site->settings().getInt("ignore/always", 0);
    if(m_page == 1) skip = std::max(skip, (int)m_site->settings().getInt("ignore/1", 0));
    if(m_api->name() == "Html" && skip > 0) {
        if((int)m_images.size() >= skip) {
            m_images.erase(m_images.begin(), m_images.begin() + skip);
            m_pageImageCount -= skip;
        } else {
            log("Wanting to skip " + std::to_string(skip) + " images but only " + std::to_string(m_images.size()) + " returned", LogLevel::Warning);
        }
    }
    if(m_smart && (int)m_images.size() > m_limit) m_images.resize((size_t)m_limit);

    log("[" + m_site->url() + "][" + m_api->name() + "] Parsed page `" + r.url + "`: " + std::to_string(m_images.size()) + " images (" +
            std::to_string(m_pageImageCount) + "), " + std::to_string(m_tags.size()) + " tags, " + std::to_string(imagesCount(true)) +
            " total, " + std::to_string(pagesCount(true)) + " pages",
        LogLevel::Info);
    finish(true);
}

bool PageApi::hasNext() const {
    int pages = pagesCount();
    int maxPages = maxPagesCount();
    if(pages <= 0 && maxPages > 0) pages = maxPages;
    return pages > m_page || (pages <= 0 && m_pageImageCount > 0);
}

int PageApi::imagesCount(bool guess) const {
    if(m_imagesCountSafe) return m_imagesCount;
    if(m_pagesCount == 1) return m_pageImageCount;
    if(!guess) return -1;
    if(m_imagesCount < 0 && m_pagesCount >= 0) return m_pagesCount * perPage();
    return m_imagesCount;
}

int PageApi::pagesCount(bool guess) const {
    if(m_pagesCountSafe) return m_pagesCount;
    if(!guess) return -1;
    if(m_pagesCount < 0 && m_imagesCount >= 0) return (int)std::ceil((double)m_imagesCount / perPage());
    return m_pagesCount;
}

int PageApi::maxPagesCount() const {
    if(m_maxImagesCount < 0) return -1;
    return (int)std::ceil((double)m_maxImagesCount / perPage());
}

void PageApi::setImageCount(int count, bool sure) {
    if(m_imagesCount <= 0 || (!m_imagesCountSafe && sure)) {
        m_imagesCount = count;
        m_imagesCountSafe = sure;
        if(sure) setPageCount((int)std::ceil((double)count / perPage()), true);
    }
}

void PageApi::setPageCount(int count, bool sure) {
    if(m_pagesCount <= 0 || (!m_pagesCountSafe && sure)) {
        m_pagesCount = count;
        m_pagesCountSafe = sure;
        if(sure) {
            int forced = m_api->forcedLimit();
            int pp = forced > 0 ? forced : std::max(m_pageImageCount, m_limit);
            setImageCount(count * pp, false);
        }
    }
}

}  // namespace bg
