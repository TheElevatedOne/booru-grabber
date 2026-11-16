#pragma once

#include "core/logger.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class LogTab {
public:
    LogTab();
    ~LogTab();
    GtkWidget* widget() const { return m_root; }

private:
    void append(const LogEntry& e);
    GtkWidget *m_root, *m_view, *m_level;
    GtkTextBuffer* m_buffer;
    int m_connection = 0;
};

}  // namespace bg::gui
