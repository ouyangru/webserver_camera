#include "shared_buffer.h"

// 静态初始化全局变量
SharedFrame g_frame = {
    .length = 0,
    .pixel_format = 0,
    .client_count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond_new_frame = PTHREAD_COND_INITIALIZER,
    .cond_start_cap = PTHREAD_COND_INITIALIZER
};