#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <gst/video/videooverlay.h>
#include <f1x/openauto/autoapp/Projection/GstVideoOutput.hpp>
#include <f1x/openauto/Common/Log.hpp>

namespace f1x { namespace openauto { namespace autoapp { namespace projection {

// Decode on slice threads (no frame-threading delay), draw on the GPU, never wait
// on the clock: a late frame is shown at once, not held for its timestamp.
// Nothing may be dropped before the decoder: losing compressed H.264 data
// corrupts every following frame until the next keyframe.
static const char* kPipeline =
    "appsrc name=src is-live=true do-timestamp=true format=time "
    "caps=video/x-h264,stream-format=byte-stream "
    "! h264parse "
    "! avdec_h264 thread-type=slice max-threads=4 "
    "! videoconvert ! %s name=sink sync=false force-aspect-ratio=true";

GstVideoOutput::GstVideoOutput(configuration::IConfiguration::Pointer configuration)
    : VideoOutput(std::move(configuration))
{
    // The GL sink must use the same window system as Qt: under XWayland Qt hands
    // us an X11 window id, but GStreamer would pick Wayland (WAYLAND_DISPLAY is
    // set) and crash drawing into it.
    if (QGuiApplication::platformName() == "xcb") {
        g_setenv("GST_GL_WINDOW", "x11", FALSE);
    }
    gst_init(nullptr, nullptr);
    // Widgets must live on the Qt GUI thread; aasdk calls us from its own threads.
    this->moveToThread(QApplication::instance()->thread());
    connect(this, &GstVideoOutput::startPlayback, this, &GstVideoOutput::onStartPlayback, Qt::BlockingQueuedConnection);
    connect(this, &GstVideoOutput::stopPlayback, this, &GstVideoOutput::onStopPlayback, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(this, "createVideoOutput", Qt::BlockingQueuedConnection);
}

GstVideoOutput::~GstVideoOutput()
{
    teardownPipeline();
}

void GstVideoOutput::createVideoOutput()
{
    OPENAUTO_LOG(info) << "[GstVideoOutput] createVideoOutput()";
    videoWidget_ = std::make_unique<QWidget>();
    videoWidget_->setWindowTitle("autoapp");          // aa-video-watch places this window
    videoWidget_->setAttribute(Qt::WA_NativeWindow);  // real X11 window for the GL sink
    videoWidget_->setAttribute(Qt::WA_PaintOnScreen); // GStreamer paints it, not Qt
    videoWidget_->setAttribute(Qt::WA_NoSystemBackground);
    videoWidget_->setAttribute(Qt::WA_AcceptTouchEvents);
    videoWidget_->setStyleSheet("background: black;");
    videoWidget_->setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
}

bool GstVideoOutput::open()
{
    return true;
}

bool GstVideoOutput::init()
{
    emit startPlayback();
    return pipeline_ != nullptr;
}

void GstVideoOutput::stop()
{
    emit stopPlayback();
}

void GstVideoOutput::write(uint64_t, const aasdk::common::DataConstBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (appsrc_ == nullptr) {
        return;
    }
    GstBuffer* gstBuffer = gst_buffer_new_allocate(nullptr, buffer.size, nullptr);
    gst_buffer_fill(gstBuffer, 0, buffer.cdata, buffer.size);
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), gstBuffer);   // takes ownership
}

void GstVideoOutput::onStartPlayback()
{
    OPENAUTO_LOG(info) << "[GstVideoOutput] onStartPlayback()";

    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen != nullptr) {
        videoWidget_->setGeometry(screen->geometry());
    }
    videoWidget_->show();
    videoWidget_->raise();
    videoWidget_->activateWindow();

    // Sink is selectable for experiments: OPENAUTO_GST_SINK=glimagesink|xvimagesink|ximagesink.
    // Default xvimagesink: XWayland's Xv adaptor scales on the GPU and draws
    // straight into our window (no foreign GL child window or render thread).
    const char* sinkName = g_getenv("OPENAUTO_GST_SINK");
    gchar* description = g_strdup_printf(kPipeline, sinkName ? sinkName : "xvimagesink");
    OPENAUTO_LOG(info) << "[GstVideoOutput] pipeline: " << description;
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description, &error);
    g_free(description);
    if (pipeline == nullptr || error != nullptr) {
        OPENAUTO_LOG(error) << "[GstVideoOutput] pipeline error: " << (error ? error->message : "unknown");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), static_cast<guintptr>(videoWidget_->winId()));
    // The GL sink draws in a child window of ours; let touch/mouse events pass
    // through to the Qt widget so InputDevice still receives them.
    gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink), FALSE);
    gst_object_unref(sink);

    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::lock_guard<std::mutex> lock(mutex_);
    pipeline_ = pipeline;
    appsrc_ = appsrc;
    OPENAUTO_LOG(info) << "[GstVideoOutput] pipeline playing";
}

void GstVideoOutput::onStopPlayback()
{
    OPENAUTO_LOG(info) << "[GstVideoOutput] onStopPlayback()";
    teardownPipeline();
    if (videoWidget_) {
        videoWidget_->hide();
    }
}

void GstVideoOutput::teardownPipeline()
{
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(pipeline, pipeline_);
        std::swap(appsrc, appsrc_);
    }
    if (appsrc != nullptr) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
        gst_object_unref(appsrc);
    }
    if (pipeline != nullptr) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}

}}}}
