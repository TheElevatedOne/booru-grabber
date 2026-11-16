#include "gui/viewer.hpp"

#include <cmath>

#include "core/logger.hpp"
#include "core/profile.hpp"
#include "core/site.hpp"
#include "gui/dialogs.hpp"
#include "gui/mainwindow.hpp"
#include "gui/videoplayer.hpp"

// GdkPixbufAnimation is deprecated in gdk-pixbuf 2.44 but remains the GTK 3 way to play GIF/APNG/WebP animations
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

namespace bg::gui {

static ViewerWindow* g_single = nullptr;
static ViewerWindow* g_last = nullptr;
ViewerWindow* ViewerWindow::single() { return g_single; }
ViewerWindow* ViewerWindow::last() { return g_last; }

ViewerWindow::ViewerWindow(MainWindow* main, std::vector<ImagePtr> images, size_t index) : m_main(main), m_images(std::move(images)) {
    build();
    if(app().profile->settings().getBool("Viewer/singleWindow", false)) g_single = this;
    g_last = this;
    show(index);
}

ViewerWindow::~ViewerWindow() {
    *m_alive = false;
    if(g_single == this) g_single = nullptr;
    if(g_last == this) g_last = nullptr;
    clearDisplay();
    if(m_slideTimer) g_source_remove(m_slideTimer);
    if(m_reply) m_reply->abort();
}

void ViewerWindow::setImages(std::vector<ImagePtr> images, size_t index) {
    m_images = std::move(images);
    show(index);
    gtk_window_present(GTK_WINDOW(m_window));
}

void ViewerWindow::build() {
    Settings& s = app().profile->settings();
    m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    addClass(m_window, "bg");
    gtk_window_set_default_size(GTK_WINDOW(m_window), (int)s.getInt("Viewer/width", 1100), (int)s.getInt("Viewer/height", 800));
    gtk_window_set_transient_for(GTK_WINDOW(m_window), nullptr);
    gtk_window_set_icon_name(GTK_WINDOW(m_window), "booru-grabber");
    onDestroy(m_window, [this] {
        int w = 0, h = 0;
        gtk_window_get_size(GTK_WINDOW(m_window), &w, &h);
        if(!m_fullscreen) {
            app().profile->settings().setInt("Viewer/width", w);
            app().profile->settings().setInt("Viewer/height", h);
        }
        delete this;
    });
    onKeyPress(m_window, [this](GdkEventKey* e) { return onKey(e); });

    GtkWidget* root = vbox(0);
    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Display: image area, video player and messages in a stack, progress on top
    GtkWidget* overlay = gtk_overlay_new();
    m_stack = gtk_stack_new();
    m_area = gtk_drawing_area_new();
    addClass(m_area, "bg-viewer-area");
    gtk_widget_add_events(m_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    on<gboolean, GtkWidget*, cairo_t*>(m_area, "draw", [this](GtkWidget*, cairo_t* cr) -> gboolean { return draw(cr); });
    onButtonPress(m_area, [this](GdkEventButton* e) {
        if(e->button == 1 && e->type == GDK_2BUTTON_PRESS) {
            toggleFullscreen();
            return true;
        }
        if(e->button == 1) {
            if(!m_fit) {
                m_dragging = true;
                m_dragX = e->x;
                m_dragY = e->y;
            } else {
                setZoomFit(false);
            }
            return true;
        }
        if(e->button == 2 && app().profile->settings().getBool("imageCloseMiddleClick", true)) {
            gtk_widget_destroy(m_window);
            return true;
        }
        if(e->button == 8) previous();
        if(e->button == 9) next();
        return false;
    });
    on<gboolean, GtkWidget*, GdkEventButton*>(m_area, "button-release-event", [this](GtkWidget*, GdkEventButton* e) -> gboolean {
        if(e->button == 1 && m_dragging) {
            bool moved = std::abs(e->x - m_dragX) > 3 || std::abs(e->y - m_dragY) > 3;
            m_dragging = false;
            if(!moved) setZoomFit(true);
        }
        return FALSE;
    });
    on<gboolean, GtkWidget*, GdkEventMotion*>(m_area, "motion-notify-event", [this](GtkWidget*, GdkEventMotion* e) -> gboolean {
        if(!m_dragging) return FALSE;
        m_offsetX += e->x - m_dragX;
        m_offsetY += e->y - m_dragY;
        m_dragX = e->x;
        m_dragY = e->y;
        gtk_widget_queue_draw(m_area);
        return TRUE;
    });
    on<gboolean, GtkWidget*, GdkEventScroll*>(m_area, "scroll-event", [this](GtkWidget*, GdkEventScroll* e) -> gboolean {
        double dy = 0;
        if(e->direction == GDK_SCROLL_UP) dy = -1;
        else if(e->direction == GDK_SCROLL_DOWN) dy = 1;
        else if(e->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent*)e, nullptr, &dy);
        if(dy == 0) return FALSE;
        if((e->state & GDK_CONTROL_MASK) || !app().profile->settings().getBool("imageNavigateScroll", true)) {
            if(m_fit) setZoomFit(false);
            m_zoom = std::clamp(m_zoom * (dy < 0 ? 1.15 : 1 / 1.15), 0.05, 20.0);
            gtk_widget_queue_draw(m_area);
        } else if(dy > 0) {
            next();
        } else {
            previous();
        }
        return TRUE;
    });
    gtk_stack_add_named(GTK_STACK(m_stack), m_area, "image");
    m_video = std::make_unique<VideoPlayer>();
    gtk_stack_add_named(GTK_STACK(m_stack), m_video->widget(), "video");
    gtk_container_add(GTK_CONTAINER(overlay), m_stack);
    m_progress = gtk_progress_bar_new();
    gtk_widget_set_valign(m_progress, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), m_progress);
    m_message = label("");
    addClass(m_message, "bg-overlay");
    gtk_widget_set_halign(m_message, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(m_message, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), m_message);
    gtk_paned_pack1(GTK_PANED(paned), overlay, TRUE, FALSE);

    // Details panel
    m_details = vbox(6);
    addClass(m_details, "bg-viewer-details");
    gtk_widget_set_size_request(m_details, 280, -1);
    gtk_box_pack_start(GTK_BOX(m_details), section("Tags"), FALSE, FALSE, 0);
    m_tagsBox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(m_tagsBox), GTK_SELECTION_NONE);
    GtkWidget* tagsScroll = scrolled(m_tagsBox);
    gtk_widget_set_vexpand(tagsScroll, TRUE);
    gtk_box_pack_start(GTK_BOX(m_details), tagsScroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(m_details), section("Information"), FALSE, FALSE, 0);
    m_infoGrid = grid();
    gtk_grid_set_row_spacing(GTK_GRID(m_infoGrid), 2);
    GtkWidget* infoScroll = scrolled(m_infoGrid);
    gtk_widget_set_size_request(infoScroll, -1, 220);
    gtk_box_pack_start(GTK_BOX(m_details), infoScroll, FALSE, FALSE, 0);
    m_detailsRevealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(m_detailsRevealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_container_add(GTK_CONTAINER(m_detailsRevealer), m_details);
    gtk_revealer_set_reveal_child(GTK_REVEALER(m_detailsRevealer), s.getBool("Viewer/showDetails", true));
    gtk_paned_pack2(GTK_PANED(paned), m_detailsRevealer, FALSE, FALSE);
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);

    // Buttons
    GtkWidget* bar = hbox(4);
    addClass(bar, "bg-bottombar");
    GtkWidget* prev = iconButton("go-previous-symbolic", "Previous (Left)");
    onClicked(prev, [this] { previous(); });
    m_counter = label("");
    GtkWidget* nxt = iconButton("go-next-symbolic", "Next (Right)");
    onClicked(nxt, [this] { next(); });
    m_saveBtn = button("_Save", "document-save-symbolic", "Save with the default filename (Ctrl+S)");
    onClicked(m_saveBtn, [this] { save(false, false); });
    m_saveFavBtn = button("Save (_fav)", "starred-symbolic", "Save with the favorites filename and folder (Ctrl+Alt+S)");
    onClicked(m_saveFavBtn, [this] { save(true, false); });
    GtkWidget* saveClose = button("Save and close", nullptr, "Save and close the viewer (Ctrl+W)");
    onClicked(saveClose, [this] { save(false, true); });
    GtkWidget* saveAsBtn = button("Save _as…", nullptr, "Choose where to save (Ctrl+Shift+S)");
    onClicked(saveAsBtn, [this] { saveAs(); });
    GtkWidget* browser = iconButton("web-browser-symbolic", "Open in the browser");
    onClicked(browser, [this] {
        if(m_image) openUri(m_image->pageUrl().empty() ? m_image->url() : m_image->pageUrl());
    });
    GtkWidget* folder = iconButton("folder-open-symbolic", "Open the download folder");
    onClicked(folder, [this] {
        std::string p = m_image && !m_image->savePath().empty() && fileExists(m_image->savePath()) ? dirName(m_image->savePath()) : app().profile->savePath();
        openUri(p);
    });
    GtkWidget* copy = iconButton("edit-copy-symbolic", "Copy the file URL (Ctrl+C)");
    onClicked(copy, [this] {
        if(m_image) copyToClipboard(m_image->url());
    });
    GtkWidget* details = iconButton("dialog-information-symbolic", "Show details (D)");
    onClicked(details, [this] {
        bool r = !gtk_revealer_get_reveal_child(GTK_REVEALER(m_detailsRevealer));
        gtk_revealer_set_reveal_child(GTK_REVEALER(m_detailsRevealer), r);
        app().profile->settings().setBool("Viewer/showDetails", r);
    });
    m_slideBtn = iconButton("media-playback-start-symbolic", "Slideshow (Space)");
    onClicked(m_slideBtn, [this] { toggleSlideshow(); });
    GtkWidget* full = iconButton("view-fullscreen-symbolic", "Fullscreen (F11)");
    onClicked(full, [this] { toggleFullscreen(); });
    for(GtkWidget* w : {prev, m_counter, nxt}) gtk_box_pack_start(GTK_BOX(bar), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    for(GtkWidget* w : {m_saveBtn, m_saveFavBtn, saveClose, saveAsBtn}) gtk_box_pack_start(GTK_BOX(bar), w, FALSE, FALSE, 0);
    for(GtkWidget* w : {full, m_slideBtn, details, copy, folder, browser}) gtk_box_pack_end(GTK_BOX(bar), w, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(m_window), root);
    gtk_widget_show_all(m_window);
    gtk_widget_hide(m_message);
}

void ViewerWindow::clearDisplay() {
    if(m_animTimer) g_source_remove(m_animTimer);
    m_animTimer = 0;
    if(m_animIter) g_object_unref(m_animIter);
    m_animIter = nullptr;
    if(m_animation) g_object_unref(m_animation);
    m_animation = nullptr;
    m_pixbuf = PixbufRef();
    if(m_video) m_video->stop();
}

void ViewerWindow::show(size_t index) {
    if(m_images.empty()) return;
    m_index = std::min(index, m_images.size() - 1);
    m_image = m_images[m_index];
    gtk_label_set_text(GTK_LABEL(m_counter), (std::to_string(m_index + 1) + " / " + std::to_string(m_images.size())).c_str());
    updateTitle();
    updateDetails();
    load();
}

void ViewerWindow::updateTitle() {
    if(!m_image) return;
    std::string title = (m_image->site() ? m_image->site()->name() : std::string("Image"));
    if(m_image->id()) title += " #" + std::to_string(m_image->id());
    if(m_image->width() > 0) title += " — " + std::to_string(m_image->width()) + "×" + std::to_string(m_image->height());
    gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
}

void ViewerWindow::updateDetails() {
    GList* children = gtk_container_get_children(GTK_CONTAINER(m_tagsBox));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    children = gtk_container_get_children(GTK_CONTAINER(m_infoGrid));
    for(GList* l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    if(!m_image) return;
    TagList tags = m_image->tags();
    sortTags(tags, app().profile->settings().get("Viewer/tagOrder", "type"));
    bool showCount = app().profile->settings().getBool("Viewer/showTagCount", false);
    for(auto& t : tags) {
        GtkWidget* l = label(tagMarkup(t.text(), t.type().name(), t.count(), showCount), 0, true);
        GtkWidget* ev = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(ev), l);
        std::string text = t.text();
        onButtonPress(ev, [this, text](GdkEventButton* e) {
            if(e->type != GDK_BUTTON_PRESS) return false;
            if(e->button == 1) m_main->openSearch(text, true);
            if(e->button == 2) m_main->openSearch(text, true);
            if(e->button == 3) m_main->tagMenu(text, (GdkEvent*)e);
            return true;
        });
        gtk_widget_set_tooltip_text(ev, (t.type().name() + (t.count() > 0 ? " · " + std::to_string(t.count()) : "")).c_str());
        gtk_list_box_insert(GTK_LIST_BOX(m_tagsBox), ev, -1);
    }
    int row = 0;
    for(auto& [key, value] : m_image->details()) {
        GtkWidget* k = label("<b>" + htmlEscape(key) + "</b>", 0, true);
        gtk_widget_set_valign(k, GTK_ALIGN_START);
        GtkWidget* v;
        if(startsWith(value, "http"))
            v = label("<a href=\"" + htmlEscape(value) + "\">" + htmlEscape(value) + "</a>", 0, true);
        else
            v = label(value);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_max_width_chars(GTK_LABEL(v), 32);
        gtk_grid_attach(GTK_GRID(m_infoGrid), k, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(m_infoGrid), v, 1, row, 1, 1);
        row++;
    }
    gtk_widget_show_all(m_tagsBox);
    gtk_widget_show_all(m_infoGrid);
}

void ViewerWindow::load() {
    if(m_reply) m_reply->abort();
    m_reply.reset();
    clearDisplay();
    m_loadedPath.clear();
    gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "image");
    gtk_widget_hide(m_message);
    gtk_widget_show(m_progress);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(m_progress), 0);
    m_fit = true;
    gtk_widget_queue_draw(m_area);

    ImagePtr img = m_image;
    std::weak_ptr<bool> alive = m_alive;
    // Details (tags, real file URL) when the listing didn't have them
    if(!img->detailsLoaded() && (img->hasUnknownTag() || img->url().empty())) {
        img->setDetailsErrorsAsWarnings(true);
        img->loadDetails([this, alive, img](Image::LoadResult) {
            if(!alive.lock() || img != m_image) return;
            updateDetails();
            updateTitle();
        });
    }

    SizeType size = img->preferredDisplaySize();
    // Already downloaded or saved before
    std::string existing = img->size(size).temporaryPath();
    if(existing.empty() || !fileExists(existing)) existing = img->savePath(size);
    if(!existing.empty() && fileExists(existing)) {
        loaded(existing, true, "");
        return;
    }
    std::string url = img->url(size);
    if(url.empty() && size == SizeType::Sample) url = img->url(SizeType::Full);
    if(url.empty() || !img->site()) {
        loaded("", false, "This image has no file URL");
        return;
    }
    std::string ext = getExtension(url);
    std::string tmp = buildPath(app().profile->tempPath(), uuid() + (ext.empty() ? "" : "." + ext));
    RequestOptions opts;
    opts.pageUrl = img->parentUrl();
    opts.ref = "image";
    opts.imagePageUrl = img->pageUrl();
    opts.saveTo = tmp;
    GtkWidget* progress = m_progress;
    opts.progress = [alive, progress](int64_t received, int64_t total) {
        if(!alive.lock() || total <= 0) return;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), (double)received / (double)total);
    };
    m_reply = img->site()->get(url, QueryType::Img, [this, alive, img, tmp, size](const net::Response& r) {
        if(!alive.lock() || img != m_image) {
            removeFile(tmp);
            return;
        }
        if(!r.ok() || startsWith(toLower(r.header("content-type")), "text/html")) {
            removeFile(tmp);
            loaded("", false, r.ok() ? "The site returned a web page instead of the file" : r.error);
            return;
        }
        img->setTemporaryPath(tmp, size);
        loaded(tmp, true, "");
    }, opts);
}

void ViewerWindow::loaded(const std::string& path, bool ok, const std::string& error) {
    gtk_widget_hide(m_progress);
    m_reply.reset();
    if(!ok) {
        gtk_label_set_text(GTK_LABEL(m_message), ("Could not load the image: " + error).c_str());
        addClass(m_message, "error");
        gtk_widget_show(m_message);
        return;
    }
    removeClass(m_message, "error");
    m_loadedPath = path;
    showFile(path);
    updateTitle();
}

void ViewerWindow::showFile(const std::string& path) {
    std::string ext = toLower(getExtension(path));
    std::string header = headerExtension(path);
    bool video = m_image->isVideo() || header == "webm" || header == "mp4" || ext == "webm" || ext == "mp4";
    if(video && app().profile->settings().getBool("Viewer/useVideoPlayer", true)) {
        gtk_stack_set_visible_child_name(GTK_STACK(m_stack), "video");
        m_video->play(path);
        return;
    }
    if(header == "zip" || ext == "zip" || header == "swf") {
        gtk_label_set_text(GTK_LABEL(m_message), header == "zip" ? "Ugoira archives can't be displayed; save the file to convert it."
                                                                 : "Flash files can't be displayed.");
        gtk_widget_show(m_message);
        return;
    }
    GError* err = nullptr;
    GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_file(path.c_str(), &err);
    if(!anim) {
        gtk_label_set_text(GTK_LABEL(m_message), ("Unsupported image: " + std::string(err ? err->message : "?")).c_str());
        if(err) g_error_free(err);
        gtk_widget_show(m_message);
        return;
    }
    if(gdk_pixbuf_animation_is_static_image(anim)) {
        GdkPixbuf* pb = gdk_pixbuf_animation_get_static_image(anim);
        GdkPixbuf* rotated = gdk_pixbuf_apply_embedded_orientation(pb);
        m_pixbuf = PixbufRef(rotated ? rotated : GDK_PIXBUF(g_object_ref(pb)));
        g_object_unref(anim);
        if(m_image->width() <= 0 && m_pixbuf) m_image->setSize(gdk_pixbuf_get_width(m_pixbuf.p), gdk_pixbuf_get_height(m_pixbuf.p), m_image->preferredDisplaySize());
    } else {
        m_animation = anim;
        m_animIter = gdk_pixbuf_animation_get_iter(anim, nullptr);
        m_pixbuf = PixbufRef(gdk_pixbuf_animation_iter_get_pixbuf(m_animIter), false);
        scheduleAnimation();
    }
    gtk_widget_queue_draw(m_area);
}

void ViewerWindow::scheduleAnimation() {
    if(!m_animIter) return;
    int delay = gdk_pixbuf_animation_iter_get_delay_time(m_animIter);
    if(delay < 0) return;
    m_animTimer = g_timeout_add((guint)std::max(20, delay), [](gpointer d) -> gboolean {
        auto* self = static_cast<ViewerWindow*>(d);
        self->m_animTimer = 0;
        if(!self->m_animIter) return G_SOURCE_REMOVE;
        gdk_pixbuf_animation_iter_advance(self->m_animIter, nullptr);
        self->m_pixbuf = PixbufRef(gdk_pixbuf_animation_iter_get_pixbuf(self->m_animIter), false);
        gtk_widget_queue_draw(self->m_area);
        self->scheduleAnimation();
        return G_SOURCE_REMOVE;
    }, this);
}

void ViewerWindow::setZoomFit(bool fit) {
    m_fit = fit;
    m_offsetX = m_offsetY = 0;
    m_zoom = 1;
    gtk_widget_queue_draw(m_area);
}

bool ViewerWindow::draw(cairo_t* cr) {
    int aw = gtk_widget_get_allocated_width(m_area), ah = gtk_widget_get_allocated_height(m_area);
    gtk_render_background(gtk_widget_get_style_context(m_area), cr, 0, 0, aw, ah);
    if(!m_pixbuf) return TRUE;
    int w = gdk_pixbuf_get_width(m_pixbuf.p), h = gdk_pixbuf_get_height(m_pixbuf.p);
    double scale;
    if(m_fit) {
        scale = std::min((double)aw / w, (double)ah / h);
        if(scale > 1 && !app().profile->settings().getBool("Viewer/scaleUp", false)) scale = 1;
    } else {
        scale = m_zoom;
    }
    double dw = w * scale, dh = h * scale;
    double x = (aw - dw) / 2 + (m_fit ? 0 : m_offsetX), y = (ah - dh) / 2 + (m_fit ? 0 : m_offsetY);
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, m_pixbuf.p, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), scale < 1 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return TRUE;
}

void ViewerWindow::next() {
    if(m_images.empty()) return;
    show((m_index + 1) % m_images.size());
}

void ViewerWindow::previous() {
    if(m_images.empty()) return;
    show(m_index == 0 ? m_images.size() - 1 : m_index - 1);
}

void ViewerWindow::save(bool favorite, bool close) {
    if(!m_image) return;
    ImagePtr img = m_image;
    gtk_widget_set_sensitive(favorite ? m_saveFavBtn : m_saveBtn, FALSE);
    std::weak_ptr<bool> alive = m_alive;
    GtkWidget* btn = favorite ? m_saveFavBtn : m_saveBtn;
    quickSave(img, favorite, [alive, btn](const SaveResults& r) {
        if(!alive.lock()) return;
        gtk_widget_set_sensitive(btn, TRUE);
        std::string what = r.empty() ? "error" : saveResultName(r[0].result);
        gtk_widget_set_tooltip_text(btn, ("Last: " + what).c_str());
    });
    if(close) gtk_widget_destroy(m_window);
}

void ViewerWindow::saveAs() {
    if(!m_image) return;
    Settings& s = app().profile->settings();
    StringList names = m_image->paths(Filename(s.get("Save/filename", "%md5%.%ext%")), app().profile->savePath(), 0);
    std::string initial = names.empty() ? buildPath(app().profile->savePath(), m_image->md5() + "." + m_image->extension()) : names[0];
    std::string target = chooseFile(GTK_WINDOW(m_window), "Save image as", true, initial);
    if(target.empty()) return;
    ImagePtr img = m_image;
    auto dl = std::make_shared<ImageDownloader>(app().profile, img, StringList{target}, 0, true, false, true, true);
    dl->save([dl](const ImagePtr&, const SaveResults& r) {
        std::string what = r.empty() ? "error" : saveResultName(r[0].result);
        app().window->setStatus("Save as: " + what + (r.empty() ? "" : " — " + r[0].path));
    });
}

void ViewerWindow::toggleFullscreen() {
    m_fullscreen = !m_fullscreen;
    if(m_fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(m_window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(m_window));
}

void ViewerWindow::toggleSlideshow() {
    if(m_slideTimer) {
        g_source_remove(m_slideTimer);
        m_slideTimer = 0;
        gtk_button_set_image(GTK_BUTTON(m_slideBtn), gtk_image_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_BUTTON));
        return;
    }
    int interval = (int)std::max(1LL, app().profile->settings().getInt("Viewer/slideshow", 5));
    m_slideTimer = g_timeout_add_seconds((guint)interval, [](gpointer d) -> gboolean {
        static_cast<ViewerWindow*>(d)->next();
        return G_SOURCE_CONTINUE;
    }, this);
    gtk_button_set_image(GTK_BUTTON(m_slideBtn), gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON));
}

bool ViewerWindow::onKey(GdkEventKey* e) {
    bool ctrl = e->state & GDK_CONTROL_MASK, shift = e->state & GDK_SHIFT_MASK, alt = e->state & GDK_MOD1_MASK;
    switch(e->keyval) {
        case GDK_KEY_Left: previous(); return true;
        case GDK_KEY_Right: next(); return true;
        case GDK_KEY_Escape:
            if(m_fullscreen)
                toggleFullscreen();
            else
                gtk_widget_destroy(m_window);
            return true;
        case GDK_KEY_F11: toggleFullscreen(); return true;
        case GDK_KEY_space:
            if(gtk_stack_get_visible_child(GTK_STACK(m_stack)) == m_video->widget())
                m_video->togglePause();
            else
                toggleSlideshow();
            return true;
        case GDK_KEY_d:
        case GDK_KEY_D:
            if(!ctrl) {
                gtk_revealer_set_reveal_child(GTK_REVEALER(m_detailsRevealer), !gtk_revealer_get_reveal_child(GTK_REVEALER(m_detailsRevealer)));
                return true;
            }
            break;
        case GDK_KEY_s:
        case GDK_KEY_S:
            if(ctrl && shift) {
                saveAs();
                return true;
            }
            if(ctrl) {
                save(alt, false);
                return true;
            }
            break;
        case GDK_KEY_w:
        case GDK_KEY_W:
            if(ctrl) {
                save(alt, true);
                return true;
            }
            break;
        case GDK_KEY_c:
        case GDK_KEY_C:
            if(ctrl && m_image) {
                copyToClipboard(m_image->url());
                return true;
            }
            break;
        case GDK_KEY_plus:
        case GDK_KEY_KP_Add:
            if(m_fit) setZoomFit(false);
            m_zoom *= 1.2;
            gtk_widget_queue_draw(m_area);
            return true;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            if(m_fit) setZoomFit(false);
            m_zoom /= 1.2;
            gtk_widget_queue_draw(m_area);
            return true;
        case GDK_KEY_0:
            setZoomFit(!m_fit);
            return true;
        default: break;
    }
    return false;
}

}  // namespace bg::gui

G_GNUC_END_IGNORE_DEPRECATIONS
