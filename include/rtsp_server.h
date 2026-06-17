#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

// GStreamer RTSP server thread entry.
void* rtsp_server_thread(void* arg);

// GStreamer HLS streamer thread entry.
void* hls_streamer_thread(void* arg);

// 设置 HLS 输出目录（在启动 RTSP/HLS 线程之前调用）
void set_hls_resources_dir(const char* resources_dir);

#endif
