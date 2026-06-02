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

void* v4l2_thread_func(void* arg) {
    int fd = open(VIDEO_DEV, O_RDWR);
    if (fd < 0) { perror("V4L2 Open"); exit(1); }
    printf("[V4L2] 摄像头设备 %s 已打开，等待客户端连接...\n", VIDEO_DEV);

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
    int streaming_on = 0;

    const int width = fmt.fmt.pix.width;
    const int height = fmt.fmt.pix.height;
    (void)width;
    (void)height;

    while (1) {
        pthread_mutex_lock(&g_frame.lock);
        while (g_frame.client_count == 0) {
            if (streaming_on) {
                ioctl(fd, VIDIOC_STREAMOFF, &type);
                streaming_on = 0;
                printf("[V4L2] Stream OFF - 进入节能模式\n");
            }
            pthread_cond_wait(&g_frame.cond_start_cap, &g_frame.lock);
        }

        if (!streaming_on) {
            ioctl(fd, VIDIOC_STREAMON, &type);
            streaming_on = 1;
            printf("[V4L2] Stream ON - 开始实时采集\n");
        }
        pthread_mutex_unlock(&g_frame.lock);

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == 0) {
            pthread_mutex_lock(&g_frame.lock);
            if (buf.bytesused <= MAX_FRAME_SIZE) {
                memcpy(g_frame.data, buffers[buf.index].start, buf.bytesused);
                g_frame.length = buf.bytesused;
                pthread_cond_broadcast(&g_frame.cond_new_frame);
            }
            pthread_mutex_unlock(&g_frame.lock);

            ioctl(fd, VIDIOC_QBUF, &buf);
        }
    }

    // 永不达成的清理（如果改变为可退出循环，请释放资源）
    for (int i = 0; i < V4L2_BUFFER_COUNT; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }
    close(fd);
    return NULL;
}
