// Video playback through GStreamer: frames are pulled from an appsink and painted with cairo,
// so no GTK video sink plugin is required. Without GStreamer the widget offers to open the file.
#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "gui/widgets.hpp"

namespace bg::gui {

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();
    GtkWidget* widget() const { return m_root; }
    void play(const std::string& file);
    void stop();
    void togglePause();
    static bool available();
    struct Impl;

private:
    void updatePosition();
    GtkWidget *m_root, *m_area, *m_playBtn, *m_scale, *m_time, *m_muteBtn;
    std::string m_file;
    std::unique_ptr<Impl> m_impl;
    guint m_timer = 0;
    bool m_seeking = false;
};

}  // namespace bg::gui
