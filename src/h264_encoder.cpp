#include "h264_encoder.h"
#include <stdio.h>

struct H264Encoder::Impl {
    int placeholder;
};

H264Encoder::H264Encoder(int width, int height, int fps, int bitrate, const char* out_filename)
{
    (void)width;
    (void)height;
    (void)fps;
    (void)bitrate;
    (void)out_filename;
    p = new Impl();
}

H264Encoder::~H264Encoder()
{
    delete p;
    p = nullptr;
}

bool H264Encoder::encode_frame(const unsigned char* yuv)
{
    (void)yuv;
    fprintf(stderr, "H264Encoder is disabled: use hardware encoder via GStreamer.\n");
    return false;
}

void H264Encoder::flush()
{
}
