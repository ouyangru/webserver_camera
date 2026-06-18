#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <vector>
#include "v4l2_capture.h"
#include "shared_buffer.h"

#define VIDEO_DEV "/dev/video0"
#define V4L2_BUFFER_COUNT 4
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

struct v4l2_buf_unit {
    void *start;
    size_t length;
};

static void fourcc_to_string(uint32_t fmt, char out[5]) {
    out[0] = static_cast<char>(fmt & 0xFF);
    out[1] = static_cast<char>((fmt >> 8) & 0xFF);
    out[2] = static_cast<char>((fmt >> 16) & 0xFF);
    out[3] = static_cast<char>((fmt >> 24) & 0xFF);
    out[4] = '\0';
}

static void print_supported_formats(int fd) {
    printf("[V4L2] Supported capture formats:\n");
    for (uint32_t index = 0;; ++index) {
        struct v4l2_fmtdesc desc;
        memset(&desc, 0, sizeof(desc));
        desc.index = index;
        desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
            break;
        }

        char fourcc[5];
        fourcc_to_string(desc.pixelformat, fourcc);
        printf("[V4L2]   %s - %s\n", fourcc, desc.description);
    }
}

static void publish_frame(const std::vector<unsigned char>& frame,
                          uint32_t pixel_format,
                          uint32_t driver_sequence) {
    if (frame.empty() || frame.size() > MAX_FRAME_SIZE) {
        return;
    }

    pthread_mutex_lock(&g_frame.lock);
    memcpy(g_frame.data, frame.data(), frame.size());
    g_frame.length = frame.size();
    g_frame.pixel_format = static_cast<int>(pixel_format);
    g_frame.sequence++;
    if (g_frame.sequence <= 5 || g_frame.sequence % 100 == 0) {
        printf("[V4L2] publish frame app_seq=%lu driver_seq=%u bytes=%zu\n",
               (unsigned long)g_frame.sequence,
               driver_sequence,
               frame.size());
    }
    pthread_cond_broadcast(&g_frame.cond_new_frame);
    pthread_mutex_unlock(&g_frame.lock);
}

static bool find_jpeg_marker(const unsigned char* data,
                             size_t len,
                             size_t from,
                             unsigned char marker,
                             size_t& marker_pos) {
    if (!data || len < 2 || from >= len) {
        return false;
    }

    for (size_t i = from; i + 1 < len; ++i) {
        if (data[i] == 0xFF && data[i + 1] == marker) {
            marker_pos = i;
            return true;
        }
    }
    return false;
}

void* v4l2_thread_func(void* arg) {
    int fd = open(VIDEO_DEV, O_RDWR);
    if (fd < 0) {
        perror("V4L2 Open");
        fprintf(stderr, "[V4L2] Capture disabled; HTTP service will continue running.\n");
        return NULL;
    }
    printf("[V4L2] 摄像头设备 %s 已打开，开始实时采集...\n", VIDEO_DEV);
    print_supported_formats(fd);

    // 设置输出为 MJPEG 优先，失败后回退 NV12/YUYV
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = DEFAULT_WIDTH;
    fmt.fmt.pix.height = DEFAULT_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT MJPEG");
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT NV12");
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
                perror("VIDIOC_S_FMT YUYV");
            }
        }
    }
    pthread_mutex_lock(&g_frame.lock);
    g_frame.pixel_format = fmt.fmt.pix.pixelformat;
    pthread_mutex_unlock(&g_frame.lock);

    {
        char fourcc[5];
        fourcc_to_string(fmt.fmt.pix.pixelformat, fourcc);
        printf("[V4L2] Actual format: %s, resolution: %dx%d\n",
               fourcc, fmt.fmt.pix.width, fmt.fmt.pix.height);
    }

    // 申请并映射缓冲区
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = V4L2_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return NULL;
    }

    struct v4l2_buf_unit buffers[V4L2_BUFFER_COUNT];
    for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            close(fd);
            return NULL;
        }
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return NULL;
        }
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        close(fd);
        return NULL;
    }
    printf("[V4L2] Stream ON - 开始实时采集\n");

    std::vector<unsigned char> mjpeg_frame;
    uint32_t current_driver_sequence = 0;
    uint64_t dqbuf_count = 0;
    uint64_t dropped_assembled_frames = 0;
    uint64_t dropped_jpeg_chunks = 0;

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
            dqbuf_count++;
            if (buf.index >= V4L2_BUFFER_COUNT) {
                fprintf(stderr, "[V4L2] invalid buffer index=%u\n", buf.index);
                ioctl(fd, VIDIOC_QBUF, &buf);
                continue;
            }

            const unsigned char* begin =
                static_cast<const unsigned char*>(buffers[buf.index].start);

            if (dqbuf_count <= 10 || dqbuf_count % 200 == 0) {
                unsigned int first0 = buf.bytesused > 0 ? begin[0] : 0;
                unsigned int first1 = buf.bytesused > 1 ? begin[1] : 0;
                unsigned int last0 = buf.bytesused > 1 ? begin[buf.bytesused - 2] : 0;
                unsigned int last1 = buf.bytesused > 0 ? begin[buf.bytesused - 1] : 0;
                printf("[V4L2] dqbuf=%lu driver_seq=%u index=%u bytes=%u flags=0x%x "
                       "first=%02X%02X last=%02X%02X\n",
                       (unsigned long)dqbuf_count,
                       buf.sequence,
                       buf.index,
                       buf.bytesused,
                       buf.flags,
                       first0,
                       first1,
                       last0,
                       last1);
            }

            if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
                if (buf.bytesused == 0) {
                    ioctl(fd, VIDIOC_QBUF, &buf);
                    continue;
                }

                size_t cursor = 0;
                while (cursor < buf.bytesused) {
                    if (mjpeg_frame.empty()) {
                        size_t soi_pos = 0;
                        if (!find_jpeg_marker(begin, buf.bytesused, cursor,
                                              0xD8, soi_pos)) {
                            dropped_jpeg_chunks++;
                            if (dropped_jpeg_chunks <= 5 ||
                                dropped_jpeg_chunks % 100 == 0) {
                                printf("[V4L2] drop MJPEG chunk without SOI "
                                       "driver_seq=%u bytes=%u dropped=%lu\n",
                                       buf.sequence,
                                       buf.bytesused,
                                       (unsigned long)dropped_jpeg_chunks);
                            }
                            break;
                        }
                        if (soi_pos > cursor) {
                            dropped_jpeg_chunks++;
                            if (dropped_jpeg_chunks <= 5 ||
                                dropped_jpeg_chunks % 100 == 0) {
                                printf("[V4L2] skip stale prefix before SOI "
                                       "driver_seq=%u bytes=%zu dropped=%lu\n",
                                       buf.sequence,
                                       soi_pos - cursor,
                                       (unsigned long)dropped_jpeg_chunks);
                            }
                        }
                        current_driver_sequence = buf.sequence;
                        cursor = soi_pos;
                    }

                    size_t eoi_pos = 0;
                    bool found_eoi = find_jpeg_marker(begin, buf.bytesused,
                                                      cursor, 0xD9, eoi_pos);
                    size_t copy_end = found_eoi ? eoi_pos + 2 : buf.bytesused;
                    size_t copy_len = copy_end - cursor;

                    if (mjpeg_frame.size() + copy_len > MAX_FRAME_SIZE) {
                        dropped_assembled_frames++;
                        printf("[V4L2] drop oversized assembled MJPEG "
                               "driver_seq=%u current=%zu next=%zu dropped=%lu\n",
                               current_driver_sequence,
                               mjpeg_frame.size(),
                               copy_len,
                               (unsigned long)dropped_assembled_frames);
                        mjpeg_frame.clear();
                        break;
                    }

                    mjpeg_frame.insert(mjpeg_frame.end(),
                                       begin + cursor,
                                       begin + copy_end);
                    cursor = copy_end;

                    if (!found_eoi) {
                        if (buf.flags & V4L2_BUF_FLAG_LAST) {
                            dropped_jpeg_chunks++;
                            printf("[V4L2] drop incomplete MJPEG at LAST "
                                   "driver_seq=%u assembled=%zu dropped=%lu\n",
                                   current_driver_sequence,
                                   mjpeg_frame.size(),
                                   (unsigned long)dropped_jpeg_chunks);
                            mjpeg_frame.clear();
                        }
                        break;
                    }

                    publish_frame(mjpeg_frame,
                                  fmt.fmt.pix.pixelformat,
                                  current_driver_sequence);
                    mjpeg_frame.clear();
                }
            } else if (buf.bytesused <= MAX_FRAME_SIZE) {
                std::vector<unsigned char> frame(begin, begin + buf.bytesused);
                publish_frame(frame, fmt.fmt.pix.pixelformat, buf.sequence);
            }

            ioctl(fd, VIDIOC_QBUF, &buf);
        } else {
            perror("[V4L2] VIDIOC_DQBUF");
        }
    }

    // 永不达成的清理（如果改变为可退出循环，请释放资源）
    for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }
    close(fd);
    return NULL;
}
