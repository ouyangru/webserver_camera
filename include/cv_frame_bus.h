#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
// Fixed-width, same-host IPC; payload is tightly packed NV12 owned by this file.
struct CvFrameHeader {
    uint32_t magic, width, height, bytes;
    uint64_t sequence, mono_us;
};
static const uint32_t CV_FRAME_MAGIC = 0x43564631;
static const size_t CV_MAX_BYTES = 1920 * 1080 * 3 / 2;
void cv_submit_nv12(const unsigned char* data, unsigned width, unsigned height, uint64_t mono_us);
std::string cv_status_json();
bool cv_set_mode(unsigned mode);
