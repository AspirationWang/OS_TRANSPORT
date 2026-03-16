#ifndef OS_TRANSPORT_INTERNAL_H
#define OS_TRANSPORT_INTERNAL_H

#include "os_transport.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define OST_LOG_DEBUG(fmt, ...) LOG_DEBUG("[%s] " fmt, __func__, ##__VA_ARGS__)
#define OST_LOG_INFO(fmt, ...)  LOG_INFO("[%s] " fmt, __func__, ##__VA_ARGS__)
#define OST_LOG_WARN(fmt, ...)  LOG_WARN("[%s] " fmt, __func__, ##__VA_ARGS__)
#define OST_LOG_ERROR(fmt, ...) LOG_ERROR("[%s] " fmt, __func__, ##__VA_ARGS__)

typedef enum {
    NOT_SPLIT = 0,
    MIDDLE_CHUNK,
    LAST_CHUNK,
} os_transport_chunk_type_t;

struct chunk_info {
    uint64_t src;   // 源缓冲区地址
    uint64_t dst;   // 目标缓冲区地址
    uint32_t len;   // 数据长度
};

typedef enum {
    NULL_TASK = 0,
    SEND_TASK,
    RECV_TASK,
} task_type_t;

// send类型的task参数
typedef struct {
    // 与主函数的同步信息
    task_sync_t *sync;
    // chunk相关参数
    struct chunk_info *chunk_info;
    bool is_last_chunk;
    // urma发送端相关参数
    urma_write_info_t write_info;
} send_task_arg_t;

typedef struct {
    // 与主函数的同步信息
    task_sync_t *sync;
    // chunk相关参数
    struct chunk_info *chunk_info;
    bool is_last_chunk;
    // urma接收端相关参数，包括h2d相关信息
    urma_recv_info_t recv_info;
} recv_task_arg_t;

#endif // OS_TRANSPORT_INTERNAL_H