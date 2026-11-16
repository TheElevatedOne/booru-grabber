#pragma once

#include <memory>
#include <vector>

#include "core/image.hpp"
#include "core/page.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class SearchTab;
class DownloadsTab;
class FavoritesTab;
class MonitorsTab;
class LogTab;

class MainWindow {
public:
    explicit MainWindow(GtkApplication* gtkApp);
    ~MainWindow();
    GtkWindow* window() const { return GTK_WINDOW(m_window); }

    SearchTab* addSearchTab(const StringList& tags = {}, const StringList& sites = {}, bool focus = true, const ImagePtr& gallery = nullptr,
                            bool searchNow = true);
    void closeTab(SearchTab* tab);
    SearchTab* currentSearchTab() const;
    void tabChanged(SearchTab* tab);  // a tab's results, tags or title changed

    void openViewer(const std::vector<ImagePtr>& images, size_t index, SearchTab* from = nullptr);
    void tagMenu(const std::string& tag, GdkEvent* event, SearchTab* from = nullptr);
    void openSearch(const std::string& tags, bool newTab, SearchTab* from = nullptr);
    void addImageToDownloads(const ImagePtr& img);
    void addSearchToDownloads(SearchTab* tab, bool allPages);
    void showDownloads();
    void showFavorites();
    bool showPage(const std::string& name);  // "favorites", "downloads", "monitors" or "log"
    void present();
    void setStatus(const std::string& text);
    void notify(const std::string& title, const std::string& body);
    void requestQuit();

    void saveSession();
    void restoreSession();
    void reloadSites();  // after sources/sites changes

private:
    void buildMenu(GtkWidget* box);
    void buildSidebar();
    void updateSidebarLists();
    void updateTags();
    bool onKey(GdkEventKey* e);
    bool onDelete();
    void restoreTab(const nlohmann::json& j);

    GtkWidget* m_window;
    GtkWidget* m_notebook;
    GtkWidget* m_paned;
    GtkWidget* m_sidebar;
    GtkWidget* m_tagsList;
    GtkWidget* m_favoritesList;
    GtkWidget* m_kflList;
    GtkWidget* m_wikiLabel;
    GtkWidget* m_status;
    GtkWidget* m_downloadStatus;
    std::vector<SearchTab*> m_tabs;
    std::vector<nlohmann::json> m_closedTabs;
    std::unique_ptr<DownloadsTab> m_downloads;
    std::unique_ptr<FavoritesTab> m_favorites;
    std::unique_ptr<MonitorsTab> m_monitors;
    std::unique_ptr<LogTab> m_log;
    int m_fixedTabs = 0;
    bool m_quitting = false;
    bool m_sidebarPlaced = false;
    std::vector<std::pair<Signal<>*, int>> m_connections;
};

}  // namespace bg::gui
