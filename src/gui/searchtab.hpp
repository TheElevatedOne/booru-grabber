// A search tab: query bar, results of one or more sites, pagination.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/page.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class MainWindow;

class SearchTab {
public:
    SearchTab(MainWindow* window, const StringList& tags, const StringList& sites, const ImagePtr& gallery = nullptr);
    ~SearchTab();
    GtkWidget* widget() const { return m_root; }
    GtkWidget* tabLabel() const { return m_tabLabel; }
    std::string title() const;

    void search(int page = 1);
    void setQuery(const std::string& tags);
    std::string query() const;
    void focusSearch();
    const StringList& sites() const { return m_sites; }
    void setSites(const StringList& sites);
    int page() const { return m_page; }
    int perPage() const;
    StringList postFilters() const;
    const TagList& tags() const { return m_tags; }
    std::string wiki() const;
    std::vector<ImagePtr> images() const;
    std::vector<ImagePtr> selectedImages() const;
    bool isGallery() const { return (bool)m_gallery; }
    const std::map<std::string, PagePtr>& pages() const { return m_pages; }

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
    void refreshStates();  // favorites/blacklist changed

    std::function<void()> onClose;

private:
    void build();
    void clearResults();
    void pageLoaded(const std::string& siteUrl, Page* page, bool ok);
    void rebuildGrid();
    GtkWidget* makeThumbnail(const ImagePtr& img, size_t index);
    void imageMenu(const ImagePtr& img, GdkEvent* e);
    void updatePagination();
    void chooseSites();
    void navigate(int page);
    int maxPage() const;

    MainWindow* m_window;
    GtkWidget *m_root, *m_tabLabel, *m_titleLabel, *m_entry, *m_sitesButton, *m_resultsBox, *m_scroll;
    GtkWidget *m_pageSpin, *m_perPageSpin, *m_mergeCheck, *m_postFilterEntry, *m_status, *m_spinner;
    GtkWidget *m_firstBtn, *m_prevBtn, *m_nextBtn, *m_lastBtn;
    StringList m_sites;
    ImagePtr m_gallery;
    int m_page = 1;
    std::map<std::string, PagePtr> m_pages;          // site url -> current page
    std::map<std::string, PageInformation> m_lastInfo;  // site url -> previously loaded page info
    std::map<std::string, std::string> m_errors;
    int m_pending = 0;
    TagList m_tags;
    std::vector<ImagePtr> m_shown;
    std::set<Image*> m_selected;
    std::vector<std::shared_ptr<void>> m_thumbHandles;
    std::map<Image*, GtkWidget*> m_thumbWidgets;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

}  // namespace bg::gui
