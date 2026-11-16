#include "gui/logtab.hpp"

#include "core/datetime.hpp"
#include "gui/style.hpp"

namespace bg::gui {

LogTab::LogTab() {
    m_root = vbox(0);
    addClass(m_root, "bg");
    GtkWidget* bar = hbox(6);
    addClass(bar, "bg-toolbar");
    gtk_box_pack_start(GTK_BOX(bar), label("Show"), FALSE, FALSE, 0);
    m_level = combo({"0", "1", "2", "3"}, {"Debug", "Info", "Warnings", "Errors"}, "1");
    gtk_box_pack_start(GTK_BOX(bar), m_level, FALSE, FALSE, 0);
    GtkWidget* clear = button("_Clear", "edit-clear-all-symbolic");
    GtkWidget* open = button("Open log file", "document-open-symbolic");
    onClicked(open, [] {
        if(!Logger::instance().logFile().empty()) openUri(Logger::instance().logFile());
    });
    gtk_box_pack_end(GTK_BOX(bar), open, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), bar, FALSE, FALSE, 0);

    m_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(m_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(m_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(m_view), 6);
    addClass(m_view, "bg-mono");
    m_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(m_view));
    GdkRGBA error = themeColor(m_view, "error_color", "#cc0000");
    GdkRGBA warning = themeColor(m_view, "warning_color", "#c4a000");
    GdkRGBA dim = themeColor(m_view, "insensitive_fg_color", "#888888");
    gtk_text_buffer_create_tag(m_buffer, "error", "foreground-rgba", &error, nullptr);
    gtk_text_buffer_create_tag(m_buffer, "warning", "foreground-rgba", &warning, nullptr);
    gtk_text_buffer_create_tag(m_buffer, "debug", "foreground-rgba", &dim, nullptr);
    gtk_text_buffer_create_tag(m_buffer, "time", "foreground-rgba", &dim, nullptr);
    // A scrolled window without a horizontal scrollbar would make the window as wide as the longest line
    GtkWidget* sw = scrolled(m_view, true);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_pack_start(GTK_BOX(m_root), sw, TRUE, TRUE, 0);
    gtk_widget_show_all(m_root);

    auto reload = [this] {
        gtk_text_buffer_set_text(m_buffer, "", -1);
        for(auto& e : Logger::instance().history()) append(e);
    };
    onClicked(clear, [this] {
        Logger::instance().clearHistory();
        gtk_text_buffer_set_text(m_buffer, "", -1);
    });
    onChanged(m_level, reload);
    reload();
    m_connection = Logger::instance().logged.connect([this](const LogEntry& e) { append(e); });
}

LogTab::~LogTab() { Logger::instance().logged.disconnect(m_connection); }

void LogTab::append(const LogEntry& e) {
    int min = (int)toInt(comboId(m_level), 1);
    if((int)e.level < min) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(m_buffer, &end);
    std::string time = "[" + DateTime::fromUnixMs(e.time, false).format("HH:mm:ss.zzz") + "] ";
    gtk_text_buffer_insert_with_tags_by_name(m_buffer, &end, time.c_str(), -1, "time", nullptr);
    const char* tag = e.level == LogLevel::Error ? "error" : e.level == LogLevel::Warning ? "warning" : e.level == LogLevel::Debug ? "debug" : nullptr;
    std::string text = e.message + "\n";
    if(tag)
        gtk_text_buffer_insert_with_tags_by_name(m_buffer, &end, text.c_str(), -1, tag, nullptr);
    else
        gtk_text_buffer_insert(m_buffer, &end, text.c_str(), -1);
    // Keep the view bounded
    int lines = gtk_text_buffer_get_line_count(m_buffer);
    if(lines > 5000) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(m_buffer, &a);
        gtk_text_buffer_get_iter_at_line(m_buffer, &b, lines - 4000);
        gtk_text_buffer_delete(m_buffer, &a, &b);
    }
    GtkTextMark* mark = gtk_text_buffer_get_insert(m_buffer);
    gtk_text_buffer_get_end_iter(m_buffer, &end);
    gtk_text_buffer_place_cursor(m_buffer, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(m_view), mark);
}

}  // namespace bg::gui
