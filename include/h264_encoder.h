#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <stddef.h>
#include <shared_buffer.h>
#include <memory>
#include <string>
#include "h264_source.h"

class H264Encoder {
public:
    H264Encoder(int width, int height, int fps, int bitrate, const char* out_filename);
    ~H264Encoder();

    // Input is tightly packed NV12, width * height * 3 / 2 bytes.
    bool encode_frame(const unsigned char* yuv, uint64_t capture_mono_us = 0);
    bool good() const;
    const std::string& error() const;

    // 刷新编码器，写出剩余帧
    void flush();

private:
    struct Impl;
    Impl* p;
};

std::unique_ptr<IH264Source> create_cedar_h264_source();

#endif
