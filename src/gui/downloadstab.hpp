// Downloads tab: queued search groups and single images, with progress.
#pragma once

#include "core/download.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class DownloadsTab {
public:
    DownloadsTab();
    ~DownloadsTab();
    GtkWidget* widget() const { return m_root; }
    GtkWidget* tabLabel() const { return m_tabLabel; }

private:
    void refresh();
    void updateProgress();
    std::vector<size_t> selectedGroups() const;
    std::vector<size_t> selectedUniques() const;
    void editGroup(size_t index);

    GtkWidget *m_root, *m_tabLabel, *m_groups, *m_uniques, *m_progress, *m_current, *m_startBtn, *m_pauseBtn;
    GtkListStore *m_groupStore, *m_uniqueStore;
    std::vector<std::pair<int, int>> m_connections;
    std::string m_currentFile;
};

}  // namespace bg::gui
