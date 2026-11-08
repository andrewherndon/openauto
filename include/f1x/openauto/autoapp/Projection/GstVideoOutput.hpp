/*
*  Low-latency Android Auto video output.
*
*  QtVideoOutput feeds the H.264 stream to QMediaPlayer, which is built for file
*  playback: it buffers and clock-syncs frames, adding visible input lag.
*  This output pushes each frame straight into a GStreamer pipeline
*      appsrc -> h264parse -> avdec_h264 -> glimagesink (sync=false)
*  that decodes and draws it immediately into a Qt widget, so touches still
*  arrive through Qt (InputDevice) exactly as before.
*
*  Select with OPENAUTO_VIDEO_BACKEND=gst (default) or =qt for the old path.
*/
#pragma once

#include <QObject>
#include <QWidget>
#include <memory>
#include <mutex>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <boost/noncopyable.hpp>
#include <f1x/openauto/autoapp/Projection/VideoOutput.hpp>

namespace f1x { namespace openauto { namespace autoapp { namespace projection {

class GstVideoOutput: public QObject, public VideoOutput, boost::noncopyable
{
    Q_OBJECT

public:
    GstVideoOutput(configuration::IConfiguration::Pointer configuration);
    ~GstVideoOutput() override;
    bool open() override;
    bool init() override;
    void write(uint64_t timestamp, const aasdk::common::DataConstBuffer& buffer) override;
    void stop() override;

signals:
    void startPlayback();
    void stopPlayback();

protected slots:
    void createVideoOutput();
    void onStartPlayback();
    void onStopPlayback();

private:
    void teardownPipeline();

    std::unique_ptr<QWidget> videoWidget_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    std::mutex mutex_;   // guards pipeline_/appsrc_ between the aasdk and Qt threads
};

}}}}
