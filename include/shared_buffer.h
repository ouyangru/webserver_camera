#ifndef SHARED_BUFFER_H
#define SHARED_BUFFER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_FRAME_SIZE (2 * 1024 * 1024)

// typedef unsigned char uint8_t;
// typedef unsigned short int uint16_t;
// typedef unsigned int uint32_t;


typedef struct {
    unsigned char data[MAX_FRAME_SIZE];
    size_t length;
    int pixel_format;
    uint64_t sequence;            // 每采集到一帧递增，用于识别真正的新帧
    int client_count;             // 在线客户端计数
    pthread_mutex_t lock;         // 互斥锁
    pthread_cond_t cond_new_frame; // 新帧到达通知
    pthread_cond_t cond_start_cap; // 开启采集通知
} SharedFrame;

// 全局唯一的共享缓冲区对象
extern SharedFrame g_frame;

#endif
