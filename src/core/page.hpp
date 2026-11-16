// One page of results for one site, trying the site's APIs in order until one works.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/filters.hpp"
#include "core/image.hpp"
#include "core/source.hpp"

namespace bg {

class Profile;
class Site;

struct SearchQuery {
    StringList tags;
    ImagePtr gallery;
    std::string endpoint;
    nlohmann::json input = nlohmann::json::object();
    std::map<std::string, std::string> urls;  // API name -> URL (restored searches)

    std::string toString() const;
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j, Profile* profile);
    bool operator==(const SearchQuery& o) const;
};

class PageApi;

class Page : public std::enable_shared_from_this<Page> {
public:
    using Done = std::function<void(Page*, bool ok)>;

    Page(Profile* profile, std::shared_ptr<Site> site, const std::vector<Site*>& sites, SearchQuery query, int page = 1, int limit = 25,
         const StringList& postFilters = {}, bool smart = false, const PageInformation& last = {});
    ~Page();

    void load(Done done, bool rateLimit = false);
    void abort();
    void setLastPage(const PageInformation& info);
    PageInformation pageInformation() const;

    Site* site() const { return m_site.get(); }
    std::shared_ptr<Site> sitePtr() const { return m_site; }
    const SearchQuery& query() const { return m_query; }
    const StringList& search() const { return m_search; }
    int page() const { return m_page; }
    int imagesPerPage() const { return m_limit; }
    const StringList& errors() const { return m_errors; }
    const std::vector<ImagePtr>& images() const;
    const TagList& tags() const;
    const std::string& wiki() const;
    std::string url() const;
    std::map<std::string, std::string> urls() const;
    int imagesCount(bool guess = true) const;
    int maxImagesCount() const;
    int pagesCount(bool guess = true) const;
    int maxPagesCount() const;
    bool hasNext() const;
    bool isLoaded() const;
    bool isValid() const { return !m_apis.empty(); }
    int pageImageCount() const;
    int filteredImageCount() const;
    int highLimit() const;
    std::string apiName() const;

private:
    void tryCurrent(bool rateLimit);
    PageApi* current() const;

    Profile* m_profile;
    std::shared_ptr<Site> m_site;
    SearchQuery m_query;
    StringList m_search, m_errors;
    int m_page, m_limit;
    std::vector<std::unique_ptr<PageApi>> m_apis;
    int m_current = 0;
    int m_htmlApi = -1;
    Done m_done;
};

using PagePtr = std::shared_ptr<Page>;

class PageApi {
public:
    PageApi(Page* parent, Profile* profile, std::shared_ptr<Site> site, Api* api, const SearchQuery& query, int page, int limit,
            PostFilter postFilter, bool smart, const PageInformation& last);
    ~PageApi();
    void load(std::function<void(bool ok)> done, bool rateLimit = false);
    void abort();
    void setLastPage(const PageInformation& info);
    PageInformation pageInformation() const;
    Api* api() const { return m_api; }

    const std::vector<ImagePtr>& images() const { return m_images; }
    const TagList& tags() const { return m_tags; }
    const std::string& wiki() const { return m_wiki; }
    const std::string& url() const { return m_url; }
    const StringList& errors() const { return m_errors; }
    bool isLoaded() const { return m_loaded; }
    bool hasNext() const;
    int imagesCount(bool guess = true) const;
    int maxImagesCount() const { return m_maxImagesCount; }
    int pagesCount(bool guess = true) const;
    int maxPagesCount() const;
    bool isImageCountSure() const { return m_imagesCountSafe; }
    bool isPageCountSure() const { return m_pagesCountSafe; }
    int pageImageCount() const { return m_pageImageCount; }
    int filteredImageCount() const { return m_filteredImageCount; }
    int highLimit() const { return m_api->maxLimit(); }

private:
    void updateUrls();
    void parse(const net::Response& r);
    void finish(bool ok);
    void setImageCount(int count, bool sure);
    void setPageCount(int count, bool sure);
    int perPage() const;

    Page* m_parent;
    Profile* m_profile;
    std::shared_ptr<Site> m_site;
    Api* m_api;
    SearchQuery m_query;
    int m_page, m_limit;
    PostFilter m_postFilter;
    bool m_smart;
    PageInformation m_last;
    std::string m_url, m_originalUrl, m_payload;
    StringMap m_headers;
    bool m_isPost = false;
    StringList m_errors;
    std::vector<ImagePtr> m_images;
    TagList m_tags;
    std::string m_wiki, m_urlNextPage, m_urlPrevPage;
    int m_imagesCount = -1, m_maxImagesCount = -1, m_pagesCount = -1;
    bool m_imagesCountSafe = false, m_pagesCountSafe = false;
    int m_pageImageCount = 0, m_filteredImageCount = 0;
    bool m_loaded = false, m_loading = false;
    int m_retries = 0;
    std::shared_ptr<net::Reply> m_reply;
    std::function<void(bool)> m_done;
};

}  // namespace bg
