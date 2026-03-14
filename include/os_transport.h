#ifndef OS_TRANSPORT_H
#define OS_TRANSPORT_H

#include "os_transport_thread_pool.h"
#include "os_transport_urma.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DEFAULT_CHUNK_SIZE (2 * 1024 * 1024)   // 2MB

typedef enum {
    NOT_SPLIT = 0,
    MIDDLE_CHUNK,
    LAST_CHUNK,
} os_transport_chunk_type_t;

typedef union {
    struct {
        uint64_t chunk_type : 2;
        uint64_t chunk_id : 6;
        uint64_t chunk_size : 24;
        uint64_t request_id : 32;
    } bs;
    uint64_t user_ctx;
} os_transport_user_data_t;

struct buffer_info {
    uint64_t addr;             // 数据缓冲区地址
    urma_target_seg_t *tseg;   // 目标分段信息
};

struct chunk_info {
    uint64_t src;   // 源缓冲区地址
    uint64_t dst;   // 目标缓冲区地址
    uint32_t len;   // 数据长度
};

typedef struct os_transport_cfg {
    bool urma_event_mode;
    uint8_t reserved1[3];         // 保留字节，保持结构体对齐
    uint32_t worker_thread_num;   // 线程池中工作线程数量
    urma_jfce_t *jfce;            // 关联的JFCE对象
    urma_jfc_t *jfc;              // 关联的JFC对象
    uint32_t reserved2[10];
} os_transport_cfg_t;

typedef enum {
    NULL_TASK = 0,
    SEND_TASK,
    RECV_TASK,
} task_type_t;

typedef struct {
    ThreadPoolTask *tasks;
    void *task_args;
    uint32_t task_num;
} task_group_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int request_completed;           // 该请求的所有task是否都已完成
    uint64_t total_tasks;            // 任务组总任务数
    uint64_t completed_tasks;        // 任务组已完成任务数
    task_group_t *group_task_args;   // 任务组，由主线程统一释放
    struct chunk_info *chunks;       // 本次请求关联的chunk数组，由主线程统一释放
} task_sync_t;

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

typedef struct os_transport_handle {
    urma_context_t *urma_ctx;
    uint32_t worker_thread_num;
    bool urma_event_mode;
    ThreadPoolHandle thread_pool;
} os_transport_handle_t;

uint32_t os_transport_init(urma_context_t *urma_ctx, os_transport_cfg_t *ost_cfg, void **handle);

uint32_t os_transport_reg_jfc(urma_jfce_t *jfce, urma_jfc_t *jfc, void *handle);

uint32_t os_transport_send(void *handle, struct urma_jetty_info *jetty_info,
                           struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t len, uint32_t server_key, uint32_t client_key,
                           task_sync_t **ret_sync_handle);

uint32_t os_transport_recv(void *handle, struct buffer_info *host_src, device_info_t *device_dst,
                           uint32_t len, uint32_t client_key, task_sync_t **ret_sync_handle);

uint32_t wait_and_free_sync(task_sync_t *sync_handle);

uint32_t os_transport_destroy(void *handle);

#endif   // OS_TRANSPORT_H
