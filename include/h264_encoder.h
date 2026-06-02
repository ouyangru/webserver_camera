#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <stddef.h>
#include <shared_buffer.h>

class H264Encoder {
public:
    H264Encoder(int width, int height, int fps, int bitrate, const char* out_filename);
    ~H264Encoder();

    // Deprecated: use hardware encoder via GStreamer.
    bool encode_frame(const unsigned char* yuv);

    // 刷新编码器，写出剩余帧
    void flush();

private:
    struct Impl;
    Impl* p;
};

#endif
