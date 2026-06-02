#include "rtsp_server.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <linux/videodev2.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "shared_buffer.h"

#define RTSP_PORT 8554
#define RTSP_MOUNT "/live"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define DEFAULT_FPS 25

const char* kHlsDir = "/usr/share/webserver_camera/resources/hls";
const char* kHlsPlaylist = "/usr/share/webserver_camera/resources/hls/stream.m3u8";
const char* kHlsSegment = "/usr/share/webserver_camera/resources/hls/segment%05d.ts";

namespace {
GMainLoop* g_loop = NULL;
GstRTSPServer* g_server = NULL;
GstElement* g_appsrc = NULL;
GMainLoop* g_hls_loop = NULL;
GstElement* g_hls_pipeline = NULL;
GstElement* g_hls_appsrc = NULL;
pthread_t g_push_thread;
int g_push_thread_started = 0;
guint64 g_frame_id = 0;

const char* select_h264_encoder() {
    const char* candidates[] = {
        "awh264enc",
        "cedarh264enc",
        "v4l2h264enc",
        "v4l2slh264enc",
        "sunxih264enc",
        NULL
    };

    for (int i = 0; candidates[i] != NULL; ++i) {
        GstElementFactory* factory = gst_element_factory_find(candidates[i]);
        if (factory) {
            gst_object_unref(factory);
            return candidates[i];
        }
    }
    return NULL;
}

void ensure_hls_dir() {
    if (mkdir(kHlsDir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[HLS] Failed to create dir: %s\n", kHlsDir);
    }
}

const char* get_raw_format_caps() {
    int fmt = 0;
    pthread_mutex_lock(&g_frame.lock);
    fmt = g_frame.pixel_format;
    pthread_mutex_unlock(&g_frame.lock);

    if (fmt == V4L2_PIX_FMT_YUYV) {
        return "YUY2";
    }
    return "NV12";
}

bool is_mjpeg_format() {
    int fmt = 0;
    pthread_mutex_lock(&g_frame.lock);
    fmt = g_frame.pixel_format;
    pthread_mutex_unlock(&g_frame.lock);

    return fmt == V4L2_PIX_FMT_MJPEG;
}

void* push_frame_thread(void*) {
    while (g_loop && g_main_loop_is_running(g_loop)) {
        pthread_mutex_lock(&g_frame.lock);
        pthread_cond_wait(&g_frame.cond_new_frame, &g_frame.lock);
        size_t len = g_frame.length;
        if ((!g_appsrc && !g_hls_appsrc) || len == 0) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }

        GstBuffer* buffer = gst_buffer_new_allocate(NULL, len, NULL);
        if (!buffer) {
            pthread_mutex_unlock(&g_frame.lock);
            continue;
        }
        gst_buffer_fill(buffer, 0, g_frame.data, len);
        GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(g_frame_id, GST_SECOND, DEFAULT_FPS);
        GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, DEFAULT_FPS);
        g_frame_id++;
        pthread_mutex_unlock(&g_frame.lock);

        GstFlowReturn ret = GST_FLOW_OK;
        if (g_appsrc) {
            g_signal_emit_by_name(g_appsrc, "push-buffer", buffer, &ret);
        }
        if (g_hls_appsrc) {
            GstBuffer* buffer_ref = gst_buffer_ref(buffer);
            GstFlowReturn hls_ret = GST_FLOW_OK;
            g_signal_emit_by_name(g_hls_appsrc, "push-buffer", buffer_ref, &hls_ret);
            gst_buffer_unref(buffer_ref);
        }
        gst_buffer_unref(buffer);
        if (ret != GST_FLOW_OK) {
            usleep(5 * 1000);
        }
    }
    return NULL;
}

void on_media_configure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data) {
    (void)factory;
    (void)user_data;

    GstElement* element = gst_rtsp_media_get_element(media);
    GstElement* appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "src");
    if (!appsrc) {
        g_object_unref(element);
        return;
    }

    GstCaps* caps = NULL;
    if (is_mjpeg_format()) {
        caps = gst_caps_new_simple("image/jpeg",
                                   "width", G_TYPE_INT, DEFAULT_WIDTH,
                                   "height", G_TYPE_INT, DEFAULT_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, DEFAULT_FPS, 1,
                                   NULL);
    } else {
        const char* raw_format = get_raw_format_caps();
        caps = gst_caps_new_simple("video/x-raw",
                                   "format", G_TYPE_STRING, raw_format,
                                   "width", G_TYPE_INT, DEFAULT_WIDTH,
                                   "height", G_TYPE_INT, DEFAULT_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, DEFAULT_FPS, 1,
                                   NULL);
    }
    g_object_set(appsrc,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE,
                 "caps", caps,
                 NULL);
    gst_caps_unref(caps);

    g_appsrc = appsrc;

    g_object_unref(element);
}
} // namespace

void* rtsp_server_thread(void* arg) {
    (void)arg;
    int argc = 0;
    char** argv = NULL;
    gst_init(&argc, &argv);

    g_loop = g_main_loop_new(NULL, FALSE);
    if (!g_loop) {
        fprintf(stderr, "[RTSP] Failed to create main loop\n");
        return NULL;
    }

    g_server = gst_rtsp_server_new();
    if (!g_server) {
        fprintf(stderr, "[RTSP] Failed to create server\n");
        g_main_loop_unref(g_loop);
        g_loop = NULL;
        return NULL;
    }

    char service[16];
    snprintf(service, sizeof(service), "%d", RTSP_PORT);
    gst_rtsp_server_set_service(g_server, service);

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(g_server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    const char* encoder = select_h264_encoder();
    if (!encoder) {
        fprintf(stderr, "[RTSP] No hardware H.264 encoder found\n");
        g_object_unref(mounts);
        g_object_unref(g_server);
        g_server = NULL;
        g_main_loop_unref(g_loop);
        g_loop = NULL;
        return NULL;
    }

    const char* raw_format = get_raw_format_caps();
    const char* convert = "";
    if (is_mjpeg_format()) {
        convert = "jpegdec ! videoconvert ! video/x-raw,format=NV12 ! ";
    } else if (strcmp(raw_format, "YUY2") == 0) {
        convert = "videoconvert ! video/x-raw,format=NV12 ! ";
    }

    printf("[RTSP] Using encoder: %s\n", encoder);
    char pipeline[512];
    snprintf(pipeline, sizeof(pipeline),
             "( appsrc name=src is-live=true format=time do-timestamp=true "
             "! queue "
             "! %s"
             "! %s "
             "! h264parse config-interval=1 "
             "! rtph264pay name=pay0 pt=96 )",
             convert,
             encoder);

    gst_rtsp_media_factory_set_launch(factory, pipeline);
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), NULL);

    gst_rtsp_mount_points_add_factory(mounts, RTSP_MOUNT, factory);
    g_object_unref(mounts);

    if (gst_rtsp_server_attach(g_server, NULL) == 0) {
        fprintf(stderr, "[RTSP] Failed to attach server\n");
        g_object_unref(g_server);
        g_server = NULL;
        g_main_loop_unref(g_loop);
        g_loop = NULL;
        return NULL;
    }

    pthread_mutex_lock(&g_frame.lock);
    if (g_frame.client_count == 0) {
        g_frame.client_count = 1;
        pthread_cond_signal(&g_frame.cond_start_cap);
    }
    pthread_mutex_unlock(&g_frame.lock);

    if (!g_push_thread_started) {
        if (pthread_create(&g_push_thread, NULL, push_frame_thread, NULL) == 0) {
            g_push_thread_started = 1;
        }
    }

    printf("[RTSP] Listening on rtsp://0.0.0.0:%d%s\n", RTSP_PORT, RTSP_MOUNT);
    g_main_loop_run(g_loop);

    if (g_push_thread_started) {
        pthread_join(g_push_thread, NULL);
        g_push_thread_started = 0;
    }

    if (g_appsrc) {
        g_appsrc = NULL;
    }

    if (g_server) {
        g_object_unref(g_server);
        g_server = NULL;
    }
    if (g_loop) {
        g_main_loop_unref(g_loop);
        g_loop = NULL;
    }
    return NULL;
}

void* hls_streamer_thread(void* arg) {
    (void)arg;
    int argc = 0;
    char** argv = NULL;
    gst_init(&argc, &argv);

    ensure_hls_dir();

    const char* encoder = select_h264_encoder();
    if (!encoder) {
        fprintf(stderr, "[HLS] No hardware H.264 encoder found\n");
        return NULL;
    }

    const char* raw_format = get_raw_format_caps();
    const char* convert = "";
    if (is_mjpeg_format()) {
        convert = "jpegdec ! videoconvert ! video/x-raw,format=NV12 ! ";
    } else if (strcmp(raw_format, "YUY2") == 0) {
        convert = "videoconvert ! video/x-raw,format=NV12 ! ";
    }

    printf("[HLS] Using encoder: %s\n", encoder);
    char pipeline_desc[512];
    snprintf(pipeline_desc, sizeof(pipeline_desc),
             "appsrc name=src is-live=true format=time do-timestamp=true "
             "! queue "
             "! %s"
             "! %s "
             "! h264parse config-interval=1 "
             "! mpegtsmux "
             "! hlssink playlist-location=%s location=%s target-duration=2 max-files=5",
             convert, encoder, kHlsPlaylist, kHlsSegment);

    GError* error = NULL;
    g_hls_pipeline = gst_parse_launch(pipeline_desc, &error);
    if (!g_hls_pipeline) {
        fprintf(stderr, "[HLS] Failed to create pipeline: %s\n", error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
        return NULL;
    }

    g_hls_appsrc = gst_bin_get_by_name(GST_BIN(g_hls_pipeline), "src");
    if (!g_hls_appsrc) {
        fprintf(stderr, "[HLS] Failed to get appsrc\n");
        gst_object_unref(g_hls_pipeline);
        g_hls_pipeline = NULL;
        return NULL;
    }

    GstCaps* caps = NULL;
    if (is_mjpeg_format()) {
        caps = gst_caps_new_simple("image/jpeg",
                                   "width", G_TYPE_INT, DEFAULT_WIDTH,
                                   "height", G_TYPE_INT, DEFAULT_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, DEFAULT_FPS, 1,
                                   NULL);
    } else {
        const char* raw_format = get_raw_format_caps();
        caps = gst_caps_new_simple("video/x-raw",
                                   "format", G_TYPE_STRING, raw_format,
                                   "width", G_TYPE_INT, DEFAULT_WIDTH,
                                   "height", G_TYPE_INT, DEFAULT_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, DEFAULT_FPS, 1,
                                   NULL);
    }
    g_object_set(g_hls_appsrc,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE,
                 "caps", caps,
                 NULL);
    gst_caps_unref(caps);

    if (gst_element_set_state(g_hls_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[HLS] Failed to start pipeline\n");
        gst_object_unref(g_hls_appsrc);
        g_hls_appsrc = NULL;
        gst_object_unref(g_hls_pipeline);
        g_hls_pipeline = NULL;
        return NULL;
    }

    pthread_mutex_lock(&g_frame.lock);
    if (g_frame.client_count == 0) {
        g_frame.client_count = 1;
        pthread_cond_signal(&g_frame.cond_start_cap);
    }
    pthread_mutex_unlock(&g_frame.lock);

    g_hls_loop = g_main_loop_new(NULL, FALSE);
    if (g_hls_loop) {
        printf("[HLS] Writing playlist at %s\n", kHlsPlaylist);
        g_main_loop_run(g_hls_loop);
    }

    if (g_hls_pipeline) {
        gst_element_set_state(g_hls_pipeline, GST_STATE_NULL);
        gst_object_unref(g_hls_pipeline);
        g_hls_pipeline = NULL;
    }
    if (g_hls_appsrc) {
        gst_object_unref(g_hls_appsrc);
        g_hls_appsrc = NULL;
    }
    if (g_hls_loop) {
        g_main_loop_unref(g_hls_loop);
        g_hls_loop = NULL;
    }
    return NULL;
}
