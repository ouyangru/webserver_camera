#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
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

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
            pthread_mutex_lock(&g_frame.lock);
            if (buf.bytesused <= MAX_FRAME_SIZE) {
                memcpy(g_frame.data, buffers[buf.index].start, buf.bytesused);
                g_frame.length = buf.bytesused;
                g_frame.sequence++;
                if (g_frame.sequence <= 3) {
                    printf("[V4L2] captured frame #%lu, bytes=%d\n",
                           (unsigned long)g_frame.sequence, buf.bytesused);
                }
                pthread_cond_broadcast(&g_frame.cond_new_frame);
            }
            pthread_mutex_unlock(&g_frame.lock);

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
