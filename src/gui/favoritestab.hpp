// Favorites tab: favorite searches with their last thumbnail and monitors.
#pragma once

#include "gui/widgets.hpp"

namespace bg::gui {

class MainWindow;

class FavoritesTab {
public:
    explicit FavoritesTab(MainWindow* window);
    ~FavoritesTab();
    GtkWidget* widget() const { return m_root; }
    void refresh();

private:
    void favoriteMenu(const std::string& name, GdkEvent* e);
    MainWindow* m_window;
    GtkWidget *m_root, *m_flow, *m_sort, *m_reverse, *m_filter;
    int m_connection = 0;
};

}  // namespace bg::gui
