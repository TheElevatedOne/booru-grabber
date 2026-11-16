// Image viewer window: full image (zoom/pan, animations, videos), details and save actions.
#pragma once

#include <memory>
#include <vector>

#include "core/image.hpp"
#include "gui/widgets.hpp"

namespace bg::gui {

class MainWindow;
class VideoPlayer;

class ViewerWindow {
public:
    ViewerWindow(MainWindow* main, std::vector<ImagePtr> images, size_t index);
    ~ViewerWindow();
    void show(size_t index);
    void setImages(std::vector<ImagePtr> images, size_t index);
    GtkWindow* window() const { return GTK_WINDOW(m_window); }
    static ViewerWindow* single();  // the shared window in "single window" mode
    static ViewerWindow* last();    // most recently opened viewer

private:
    void build();
    void load();
    void loaded(const std::string& path, bool ok, const std::string& error);
    void showFile(const std::string& path);
    void clearDisplay();
    void updateDetails();
    void updateTitle();
    void next();
    void previous();
    void save(bool favorite, bool close);
    void saveAs();
    void toggleFullscreen();
    void toggleSlideshow();
    bool onKey(GdkEventKey* e);
    bool draw(cairo_t* cr);
    void scheduleAnimation();
    void setZoomFit(bool fit);

    MainWindow* m_main;
    std::vector<ImagePtr> m_images;
    size_t m_index = 0;
    ImagePtr m_image;
    GtkWidget *m_window, *m_stack, *m_area, *m_progress, *m_message, *m_details, *m_tagsBox, *m_infoGrid, *m_detailsRevealer;
    GtkWidget *m_saveBtn, *m_saveFavBtn, *m_slideBtn, *m_counter;
    std::unique_ptr<VideoPlayer> m_video;
    PixbufRef m_pixbuf;
    GdkPixbufAnimation* m_animation = nullptr;
    GdkPixbufAnimationIter* m_animIter = nullptr;
    guint m_animTimer = 0, m_slideTimer = 0;
    bool m_fit = true, m_fullscreen = false;
    double m_zoom = 1, m_offsetX = 0, m_offsetY = 0, m_dragX = 0, m_dragY = 0;
    bool m_dragging = false;
    std::shared_ptr<net::Reply> m_reply;
    std::string m_loadedPath;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

}  // namespace bg::gui
