// Monitors tab: searches checked periodically for new images.
#pragma once

#include "gui/widgets.hpp"

namespace bg::gui {

class MonitorsTab {
public:
    MonitorsTab();
    ~MonitorsTab();
    GtkWidget* widget() const { return m_root; }
    void refresh();

private:
    struct Ref {
        bool favorite;
        std::string favoriteName;
        size_t index;
    };
    bool selected(Ref& ref) const;
    GtkWidget *m_root, *m_view, *m_status;
    GtkListStore* m_store;
    std::vector<Ref> m_refs;
    std::vector<std::pair<int, int>> m_connections;
    guint m_timer = 0;
};

}  // namespace bg::gui
