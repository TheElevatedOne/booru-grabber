#include "gui/videoplayer.hpp"

#include <algorithm>
#include <cstring>

#include "core/logger.hpp"

#ifdef BG_HAVE_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#endif

namespace bg::gui {

#ifdef BG_HAVE_GSTREAMER

struct VideoPlayer::Impl {
    GstElement* pipeline = nullptr;
    std::mutex mutex;
    cairo_surface_t* frame = nullptr;  // guarded by mutex
    GtkWidget* area = nullptr;
    guint busWatch = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool paused = false;
};

bool VideoPlayer::available() { return true; }

static GstFlowReturn onSample(GstAppSink* sink, gpointer data) {
    auto* impl = static_cast<VideoPlayer::Impl*>(data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if(!sample) return GST_FLOW_OK;
    GstCaps* caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if(caps && gst_video_info_from_caps(&info, caps) && buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        int w = GST_VIDEO_INFO_WIDTH(&info), h = GST_VIDEO_INFO_HEIGHT(&info);
        int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
        unsigned char* dst = cairo_image_surface_get_data(s);
        int dstStride = cairo_image_surface_get_stride(s);
        for(int y = 0; y < h; y++) memcpy(dst + y * dstStride, map.data + y * stride, (size_t)std::min(stride, dstStride));
        cairo_surface_mark_dirty(s);
        gst_buffer_unmap(buffer, &map);
        {
            std::lock_guard<std::mutex> l(impl->mutex);
            if(impl->frame) cairo_surface_destroy(impl->frame);
            impl->frame = s;
        }
        std::weak_ptr<bool> alive = impl->alive;
        GtkWidget* area = impl->area;
        defer([alive, area] {
            if(alive.lock()) gtk_widget_queue_draw(area);
        });
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

VideoPlayer::VideoPlayer() : m_impl(std::make_unique<Impl>()) {
    static bool inited = false;
    if(!inited) {
        gst_init(nullptr, nullptr);
        inited = true;
    }
    m_root = vbox(0);
    m_area = gtk_drawing_area_new();
    addClass(m_area, "bg-viewer-area");
    gtk_widget_set_vexpand(m_area, TRUE);
    m_impl->area = m_area;
    Impl* impl = m_impl.get();
    on<gboolean, GtkWidget*, cairo_t*>(m_area, "draw", [impl](GtkWidget* w, cairo_t* cr) -> gboolean {
        int aw = gtk_widget_get_allocated_width(w), ah = gtk_widget_get_allocated_height(w);
        GtkStyleContext* ctx = gtk_widget_get_style_context(w);
        gtk_render_background(ctx, cr, 0, 0, aw, ah);
        std::lock_guard<std::mutex> l(impl->mutex);
        if(!impl->frame) return TRUE;
        int fw = cairo_image_surface_get_width(impl->frame), fh = cairo_image_surface_get_height(impl->frame);
        double scale = std::min((double)aw / fw, (double)ah / fh);
        cairo_translate(cr, (aw - fw * scale) / 2, (ah - fh * scale) / 2);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, impl->frame, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        return TRUE;
    });
    onButtonPress(m_area, [this](GdkEventButton* e) {
        if(e->button == 1 && e->type == GDK_BUTTON_PRESS) {
            togglePause();
            return true;
        }
        return false;
    });
    gtk_box_pack_start(GTK_BOX(m_root), m_area, TRUE, TRUE, 0);

    GtkWidget* controls = hbox(4);
    addClass(controls, "bg-bottombar");
    m_playBtn = iconButton("media-playback-pause-symbolic", "Play / pause (Space)");
    onClicked(m_playBtn, [this] { togglePause(); });
    m_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.001);
    gtk_scale_set_draw_value(GTK_SCALE(m_scale), FALSE);
    on<gboolean, GtkRange*, GtkScrollType, gdouble>(m_scale, "change-value", [this](GtkRange*, GtkScrollType, gdouble value) -> gboolean {
        if(!m_impl->pipeline) return FALSE;
        gint64 duration = 0;
        if(gst_element_query_duration(m_impl->pipeline, GST_FORMAT_TIME, &duration) && duration > 0)
            gst_element_seek_simple(m_impl->pipeline, GST_FORMAT_TIME, (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                    (gint64)(std::clamp(value, 0.0, 1.0) * duration));
        return FALSE;
    });
    m_time = label("0:00 / 0:00");
    m_muteBtn = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(m_muteBtn), gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(m_muteBtn, "Mute");
    onToggled(m_muteBtn, [this] {
        bool muted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_muteBtn));
        if(m_impl->pipeline) g_object_set(m_impl->pipeline, "mute", muted, nullptr);
        gtk_button_set_image(GTK_BUTTON(m_muteBtn),
                             gtk_image_new_from_icon_name(muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic", GTK_ICON_SIZE_BUTTON));
    });
    gtk_box_pack_start(GTK_BOX(controls), m_playBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), m_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), m_time, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(controls), m_muteBtn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), controls, FALSE, FALSE, 0);
    gtk_widget_show_all(m_root);
}

VideoPlayer::~VideoPlayer() {
    *m_impl->alive = false;
    stop();
}

void VideoPlayer::stop() {
    if(m_timer) g_source_remove(m_timer);
    m_timer = 0;
    if(m_impl->busWatch) g_source_remove(m_impl->busWatch);
    m_impl->busWatch = 0;
    if(m_impl->pipeline) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
    std::lock_guard<std::mutex> l(m_impl->mutex);
    if(m_impl->frame) cairo_surface_destroy(m_impl->frame);
    m_impl->frame = nullptr;
}

void VideoPlayer::play(const std::string& file) {
    stop();
    m_file = file;
    GstElement* playbin = gst_element_factory_make("playbin", nullptr);
    if(!playbin) {
        log("GStreamer playbin is not available, cannot play videos", LogLevel::Error);
        return;
    }
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, G_BYTE_ORDER == G_LITTLE_ENDIAN ? "BGRx" : "xRGB", nullptr);
    g_object_set(sink, "caps", caps, "max-buffers", 2, "drop", TRUE, "sync", TRUE, nullptr);
    gst_caps_unref(caps);
    GstAppSinkCallbacks cb{};
    cb.new_sample = onSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, m_impl.get(), nullptr);
    gchar* uri = g_filename_to_uri(file.c_str(), nullptr, nullptr);
    g_object_set(playbin, "uri", uri, "video-sink", sink, "mute", (gboolean)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m_muteBtn)), nullptr);
    g_free(uri);
    m_impl->pipeline = playbin;
    m_impl->paused = false;
    GstBus* bus = gst_element_get_bus(playbin);
    m_impl->busWatch = gst_bus_add_watch(bus, [](GstBus*, GstMessage* msg, gpointer data) -> gboolean {
        auto* self = static_cast<VideoPlayer*>(data);
        if(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            // Loop like the original player
            gst_element_seek_simple(self->m_impl->pipeline, GST_FORMAT_TIME, (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
        } else if(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            log("Video playback error: " + std::string(err ? err->message : "?") + " (missing GStreamer plugins?)", LogLevel::Error);
            if(err) g_error_free(err);
        }
        return G_SOURCE_CONTINUE;
    }, this);
    gst_object_unref(bus);
    gst_element_set_state(playbin, GST_STATE_PLAYING);
    gtk_button_set_image(GTK_BUTTON(m_playBtn), gtk_image_new_from_icon_name("media-playback-pause-symbolic", GTK_ICON_SIZE_BUTTON));
    m_timer = g_timeout_add(250, [](gpointer d) -> gboolean {
        static_cast<VideoPlayer*>(d)->updatePosition();
        return G_SOURCE_CONTINUE;
    }, this);
}

void VideoPlayer::togglePause() {
    if(!m_impl->pipeline) return;
    m_impl->paused = !m_impl->paused;
    gst_element_set_state(m_impl->pipeline, m_impl->paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
    gtk_button_set_image(GTK_BUTTON(m_playBtn), gtk_image_new_from_icon_name(m_impl->paused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic",
                                                                             GTK_ICON_SIZE_BUTTON));
}

static std::string formatTime(gint64 ns) {
    long s = (long)(ns / GST_SECOND);
    return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
}

void VideoPlayer::updatePosition() {
    if(!m_impl->pipeline) return;
    gint64 pos = 0, duration = 0;
    if(!gst_element_query_position(m_impl->pipeline, GST_FORMAT_TIME, &pos) || !gst_element_query_duration(m_impl->pipeline, GST_FORMAT_TIME, &duration) ||
       duration <= 0)
        return;
    gtk_range_set_value(GTK_RANGE(m_scale), (double)pos / (double)duration);
    gtk_label_set_text(GTK_LABEL(m_time), (formatTime(pos) + " / " + formatTime(duration)).c_str());
}

#else

struct VideoPlayer::Impl {};
bool VideoPlayer::available() { return false; }

VideoPlayer::VideoPlayer() : m_impl(std::make_unique<Impl>()) {
    m_root = vbox(8);
    m_area = label("This build has no video support.", 0.5f);
    gtk_widget_set_vexpand(m_area, TRUE);
    m_playBtn = button("Open in the default player", "media-playback-start-symbolic");
    gtk_widget_set_halign(m_playBtn, GTK_ALIGN_CENTER);
    onClicked(m_playBtn, [this] {
        if(!m_file.empty()) openUri(m_file);
    });
    m_scale = m_time = m_muteBtn = nullptr;
    gtk_box_pack_start(GTK_BOX(m_root), m_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(m_root), m_playBtn, FALSE, FALSE, 8);
    gtk_widget_show_all(m_root);
}
VideoPlayer::~VideoPlayer() = default;
void VideoPlayer::play(const std::string& file) { m_file = file; }
void VideoPlayer::stop() {}
void VideoPlayer::togglePause() {}
void VideoPlayer::updatePosition() {}

#endif

}  // namespace bg::gui
