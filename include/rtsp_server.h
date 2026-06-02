#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

// GStreamer RTSP server thread entry.
void* rtsp_server_thread(void* arg);

// GStreamer HLS streamer thread entry.
void* hls_streamer_thread(void* arg);

#endif
