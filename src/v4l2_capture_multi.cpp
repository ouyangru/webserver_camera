#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>
#include "shared_buffer.h"
#include "h264_encoder.h"
#include "v853_isp.h"
#include "pipeline_metrics.h"
#include "cv_frame_bus.h"
#include "logger.h"
#if ENABLE_JPEG
#include <jpeglib.h>
#include <setjmp.h>
#endif

extern void* v4l2_singleplane_thread_func(void*);
namespace {
struct Plane {
    void* ptr = MAP_FAILED;
    size_t length = 0;
};
struct Mapping { Plane planes[VIDEO_MAX_PLANES]; };
struct View { const unsigned char* data; size_t size; unsigned stride; };

#if ENABLE_JPEG
struct JpegError { jpeg_error_mgr base; jmp_buf jump; };
void jpeg_failure(j_common_ptr c) {
    c->err->output_message(c);
    longjmp(reinterpret_cast<JpegError*>(c->err)->jump, 1);
}
unsigned char clamp(int v) { return static_cast<unsigned char>(std::max(0, std::min(255, v))); }
// Convert only negotiated, bounded YUV layouts. Honor stride and per-plane offsets.
bool encode_jpeg(const std::vector<View>& views, unsigned w, unsigned h,
                 unsigned format, std::vector<unsigned char>& out) {
    if (!w || !h || w > 4096 || h > 4096 || (w & 1) || (h & 1)) return false;
    bool packed = format == V4L2_PIX_FMT_YUYV;
    bool semi = format == V4L2_PIX_FMT_NV12 || format == V4L2_PIX_FMT_NV21 ||
                format == V4L2_PIX_FMT_NV12M || format == V4L2_PIX_FMT_NV21M;
    if (!packed && !semi) return false;
    unsigned ys = views[0].stride ? views[0].stride : w * (packed ? 2 : 1);
    if (ys < w * (packed ? 2 : 1) || views[0].size < size_t(ys) * h) return false;
    const unsigned char* uv = NULL;
    unsigned uvs = ys;
    if (semi) {
        if (views.size() > 1) {
            uv = views[1].data;
            uvs = views[1].stride ? views[1].stride : w;
            if (uvs < w || views[1].size < size_t(uvs) * (h / 2)) return false;
        } else {
            if (views[0].size < size_t(ys) * h + size_t(uvs) * (h / 2)) return false;
            uv = views[0].data + size_t(ys) * h;
        }
    }
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        int yy, u, v;
        if (packed) {
            const unsigned char* q = views[0].data + size_t(y) * ys + (x & ~1U) * 2;
            yy = q[(x & 1) * 2]; u = q[1]; v = q[3];
        } else {
            yy = views[0].data[size_t(y) * ys + x];
            const unsigned char* q = uv + size_t(y / 2) * uvs + (x & ~1U);
            bool vu = format == V4L2_PIX_FMT_NV21 || format == V4L2_PIX_FMT_NV21M;
            u = q[vu ? 1 : 0]; v = q[vu ? 0 : 1];
        }
        int c = std::max(0, yy - 16), d = u - 128, e = v - 128;
        unsigned char* dst = &rgb[(size_t(y) * w + x) * 3];
        dst[0] = clamp((298*c + 409*e + 128) >> 8);
        dst[1] = clamp((298*c - 100*d - 208*e + 128) >> 8);
        dst[2] = clamp((298*c + 516*d + 128) >> 8);
    }
    jpeg_compress_struct c = {};
    JpegError error;
    c.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    // Keep libjpeg-owned output in the heap object so longjmp does not invalidate locals.
    struct Destination { unsigned char* data; unsigned long size; };
    Destination* dest = new Destination{NULL, 0};
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&c); free(dest->data); delete dest; return false;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &dest->data, &dest->size);
    c.image_width = w; c.image_height = h;
    c.input_components = 3; c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c); jpeg_set_quality(&c, 75, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = &rgb[size_t(c.next_scanline) * w * 3];
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    if (dest->size <= MAX_FRAME_SIZE) out.assign(dest->data, dest->data + dest->size);
    jpeg_destroy_compress(&c); free(dest->data); delete dest;
    return !out.empty();
}
#endif
bool supported(unsigned f) {
    if (f == V4L2_PIX_FMT_MJPEG || f == V4L2_PIX_FMT_JPEG) return true;
#if ENABLE_JPEG
    return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_NV21 ||
           f == V4L2_PIX_FMT_NV12M || f == V4L2_PIX_FMT_NV21M || f == V4L2_PIX_FMT_YUYV;
#else
    return false;
#endif
}
}

static void* capture_worker(void* arg) {
    const char* device = getenv("VIDEO_DEVICE");
    if (!device || !*device) device = "/dev/video0";
    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("[V4L2] open"); return NULL; }
    v4l2_capability cap = {};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); close(fd); return NULL; }
    unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    // This Tina VIN driver advertises mplane only in capabilities, not device_caps.
    bool mplane = (caps | cap.capabilities) & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
    if (!mplane) { close(fd); return v4l2_singleplane_thread_func(arg); }
    printf("[V4L2] %s: multiplanar capture (caps=%08x device_caps=%08x)\n",
           device, cap.capabilities, cap.device_caps);
    int input = 0;
    if (ioctl(fd, VIDIOC_S_INPUT, &input) < 0) {
        perror("[V4L2] S_INPUT"); close(fd); return NULL;
    }
    const v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v4l2_streamparm parm = {};
    parm.type = type; parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 15;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) perror("[V4L2] S_PARM");
    unsigned candidates[] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV21, V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_YUYV};
    v4l2_format fmt = {};
    bool negotiated = false;
    for (unsigned candidate : candidates) {
        if (!supported(candidate)) continue;
        memset(&fmt, 0, sizeof(fmt)); fmt.type = type;
        fmt.fmt.pix_mp.width = 640; fmt.fmt.pix_mp.height = 480;
        fmt.fmt.pix_mp.pixelformat = candidate; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == 0 && supported(fmt.fmt.pix_mp.pixelformat)) {
            negotiated = true; break;
        }
        fprintf(stderr, "[V4L2] format %c%c%c%c rejected: %s\n", candidate & 255,
            (candidate >> 8) & 255, (candidate >> 16) & 255, candidate >> 24, strerror(errno));
    }
    if (negotiated && ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("[V4L2] G_FMT"); close(fd); return NULL;
    }
    auto& pix = fmt.fmt.pix_mp;
    if (!negotiated || !pix.num_planes || pix.num_planes > VIDEO_MAX_PLANES) {
        fprintf(stderr, "[V4L2] no usable multiplanar format; capture stopped\n"); close(fd); return NULL;
    }
    printf("[V4L2] negotiated %c%c%c%c %ux%u planes=%u stride=%u size=%u\n",
        pix.pixelformat & 255, (pix.pixelformat >> 8) & 255,
        (pix.pixelformat >> 16) & 255, pix.pixelformat >> 24,
        pix.width, pix.height, pix.num_planes, pix.plane_fmt[0].bytesperline,
        pix.plane_fmt[0].sizeimage);
    // Tina VIN reports aggregate Y+UV bytes per image row for contiguous NV12.
    // Restrict this workaround to sunxi-vin and a consistent 4:2:0 payload.
    if (strcmp(reinterpret_cast<const char*>(cap.driver), "sunxi-vin") == 0 &&
        pix.num_planes == 1 && (pix.pixelformat == V4L2_PIX_FMT_NV12 || pix.pixelformat == V4L2_PIX_FMT_NV21)) {
        unsigned reported = pix.plane_fmt[0].bytesperline;
        if (reported && reported % 3 == 0 && reported * 2 / 3 >= pix.width &&
            pix.plane_fmt[0].sizeimage == size_t(reported) * pix.height) {
            pix.plane_fmt[0].bytesperline = reported * 2 / 3;
            printf("[V4L2] VIN NV12 Y stride normalized to %u\n", pix.plane_fmt[0].bytesperline);
        }
    }
    v4l2_requestbuffers req = {}; req.type = type; req.memory = V4L2_MEMORY_MMAP; req.count = 4;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0 || req.count > 32) {
        perror("[V4L2] REQBUFS"); close(fd); return NULL;
    }
    std::vector<Mapping> mappings(req.count);
    bool streaming = false;
    auto cleanup = [&]() {
        if (streaming) { v4l2_buf_type t = type; ioctl(fd, VIDIOC_STREAMOFF, &t); }
        for (auto& mapping : mappings) for (auto& plane : mapping.planes)
            if (plane.ptr != MAP_FAILED) munmap(plane.ptr, plane.length);
        close(fd);
        pthread_mutex_lock(&g_frame.lock); g_frame.length = 0; g_frame.pixel_format = 0;
        pthread_mutex_unlock(&g_frame.lock);
    };
    for (unsigned i = 0; i < req.count; ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        v4l2_buffer b = {}; b.type = type; b.memory = V4L2_MEMORY_MMAP;
        b.index = i; b.length = pix.num_planes; b.m.planes = planes;
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0 || b.length != pix.num_planes) {
            perror("[V4L2] QUERYBUF"); cleanup(); return NULL;
        }
        for (unsigned p = 0; p < b.length; ++p) {
            Plane& plane = mappings[i].planes[p]; plane.length = planes[p].length;
            plane.ptr = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
            if (plane.ptr == MAP_FAILED) { perror("[V4L2] mmap"); cleanup(); return NULL; }
        }
        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("[V4L2] QBUF"); cleanup(); return NULL; }
    }
    v4l2_buf_type t = type;
    if (ioctl(fd, VIDIOC_STREAMON, &t) < 0) { perror("[V4L2] STREAMON"); cleanup(); return NULL; }
    streaming = true; printf("[V4L2] multiplanar Stream ON\n");
    V853Isp isp;
    int video_id = 0;
    if (sscanf(device, "/dev/video%d", &video_id) != 1) video_id = 0;
    if (!isp.start(video_id)) fprintf(stderr, "[ISP] initialization failed; image color/exposure may be incorrect\n");
    std::unique_ptr<H264Encoder> encoder;
#if ENABLE_CEDAR
    if (pix.pixelformat == V4L2_PIX_FMT_NV12 || pix.pixelformat == V4L2_PIX_FMT_NV21 ||
        pix.pixelformat == V4L2_PIX_FMT_NV12M || pix.pixelformat == V4L2_PIX_FMT_NV21M) {
        encoder.reset(new H264Encoder(pix.width, pix.height, 15, 1000000, getenv("H264_DUMP_PATH")));
        if (!encoder->good()) {
            fprintf(stderr, "[Cedar] %s\n", encoder->error().c_str()); encoder.reset();
        }
    }
#endif
    unsigned long count = 0;
    std::vector<unsigned char> nv12(size_t(pix.width) * pix.height * 3 / 2);
    uint64_t previous_capture_us = 0;
    while (true) {
        const uint64_t wait_begin_us = perf_now_us();
        pollfd wait = {fd, POLLIN, 0}; int ready = poll(&wait, 1, 3000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "[V4L2] capture timeout/error: revents=%x errno=%d\n", wait.revents, errno); break;
        }
        v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        v4l2_buffer b = {}; b.type = type; b.memory = V4L2_MEMORY_MMAP;
        b.length = pix.num_planes; b.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            perror("[V4L2] DQBUF"); break;
        }
        if (b.index >= mappings.size() || b.length != pix.num_planes) {
            fprintf(stderr, "[V4L2] invalid dequeued buffer\n"); break;
        }
        const uint64_t dequeued_us = perf_now_us();
        perf_observe("capture_wait", dequeued_us - wait_begin_us);
        if (previous_capture_us) perf_observe("capture_interval", dequeued_us - previous_capture_us);
        previous_capture_us = dequeued_us;
        perf_count("captured_frames");
        const uint64_t driver_us = uint64_t(b.timestamp.tv_sec)*1000000 + b.timestamp.tv_usec;
        if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
            driver_us && driver_us <= dequeued_us) perf_observe("capture_driver_age", dequeued_us-driver_us);
        pthread_mutex_lock(&g_frame.lock);
        const bool wants_jpeg = count == 0 || g_frame.client_count > 0;
        pthread_mutex_unlock(&g_frame.lock);
        std::vector<View> views;
        bool valid = !(b.flags & V4L2_BUF_FLAG_ERROR);
        for (unsigned p = 0; p < b.length; ++p) {
            const Plane& plane = mappings[b.index].planes[p];
            size_t used = planes[p].bytesused, offset = planes[p].data_offset;
            if (offset > used || used > plane.length || used == 0) { valid = false; break; }
            views.push_back({static_cast<unsigned char*>(plane.ptr) + offset,
                used - offset, pix.plane_fmt[p].bytesperline});
        }
        std::vector<unsigned char> jpeg;
        if (valid && encoder && encoder->good() && !views.empty()) {
            unsigned ys = views[0].stride ? views[0].stride : pix.width;
            unsigned cs = views.size() > 1 && views[1].stride ? views[1].stride : ys;
            size_t ysize = size_t(ys) * pix.height;
            const unsigned char* chroma = views.size() > 1 ? views[1].data : views[0].data + std::min(ysize, views[0].size);
            size_t csize = views.size() > 1 ? views[1].size : (views[0].size >= ysize ? views[0].size - ysize : 0);
            if (ys >= pix.width && cs >= pix.width && views[0].size >= ysize && csize >= size_t(cs) * (pix.height / 2)) {
                const uint64_t copy_begin_us = perf_now_us();
                for (unsigned y = 0; y < pix.height; ++y)
                    memcpy(nv12.data() + size_t(y) * pix.width, views[0].data + size_t(y) * ys, pix.width);
                unsigned char* dstc = nv12.data() + size_t(pix.width) * pix.height;
                bool vu = pix.pixelformat == V4L2_PIX_FMT_NV21 || pix.pixelformat == V4L2_PIX_FMT_NV21M;
                for (unsigned y = 0; y < pix.height / 2; ++y) {
                    memcpy(dstc + size_t(y) * pix.width, chroma + size_t(y) * cs, pix.width);
                    if (vu) for (unsigned x = 0; x < pix.width; x += 2)
                        std::swap(dstc[size_t(y) * pix.width+x], dstc[size_t(y) * pix.width+x+1]);
                }
                perf_observe("nv12_copy", perf_now_us()-copy_begin_us);
                cv_submit_nv12(nv12.data(), pix.width, pix.height, dequeued_us);
                const uint64_t encode_begin_us = perf_now_us();
                if (!encoder->encode_frame(nv12.data(), dequeued_us) && !encoder->good())
                    fprintf(stderr, "[Cedar] encoding stopped: %s\n", encoder->error().c_str());
                perf_observe("encode", perf_now_us()-encode_begin_us);
            }
        }
        if (wants_jpeg && valid && !views.empty()) {
            const uint64_t jpeg_begin_us = perf_now_us();
            if (pix.pixelformat == V4L2_PIX_FMT_MJPEG || pix.pixelformat == V4L2_PIX_FMT_JPEG) {
                if (views[0].size <= MAX_FRAME_SIZE) jpeg.assign(views[0].data, views[0].data + views[0].size);
            }
#if ENABLE_JPEG
            else encode_jpeg(views, pix.width, pix.height, pix.pixelformat, jpeg);
#endif
            perf_observe("jpeg", perf_now_us()-jpeg_begin_us);
            perf_count("jpeg_frames");
        }
        // Requeue the camera buffer before publishing an application-owned JPEG copy.
        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("[V4L2] QBUF"); break; }
        perf_observe("capture_buffer_hold", perf_now_us()-dequeued_us);
        if (jpeg.size() >= 4 && jpeg.size() <= MAX_FRAME_SIZE && jpeg[0] == 0xff && jpeg[1] == 0xd8) {
            pthread_mutex_lock(&g_frame.lock);
            memcpy(g_frame.data, jpeg.data(), jpeg.size()); g_frame.length = jpeg.size();
            g_frame.pixel_format = V4L2_PIX_FMT_MJPEG; ++g_frame.sequence;
            g_frame.capture_mono_us = dequeued_us;
            pthread_cond_broadcast(&g_frame.cond_new_frame); pthread_mutex_unlock(&g_frame.lock);
            ++count;
            if (count <= 5 || count % 100 == 0)
                printf("[V4L2] published JPEG frame=%lu driver_seq=%u bytes=%zu\n", count, b.sequence, jpeg.size());
        } else if (wants_jpeg && count == 0) {
            fprintf(stderr, "[V4L2] frame not publishable: flags=%x plane0.bytesused=%u\n", b.flags, planes[0].bytesused);
        }
    }
    cleanup(); return NULL;
}

void* v4l2_thread_func(void* arg) {
    for (;;) {
        capture_worker(arg);
        perf_count("capture_restarts");
        LOG_WARN("capture", "event=restart delay_ms=1000");
        sleep(1);
    }
}
