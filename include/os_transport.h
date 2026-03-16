#ifndef OS_TRANSPORT_H
#define OS_TRANSPORT_H

#include "os_transport_thread_pool.h"
#include "os_transport_urma.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DEFAULT_CHUNK_SIZE (2 * 1024 * 1024)   // 2MB

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

typedef struct os_transport_cfg {
    bool urma_event_mode;
    uint8_t reserved1[3];         // 保留字节，保持结构体对齐
    uint32_t worker_thread_num;   // 线程池中工作线程数量
    urma_jfce_t *jfce;            // 关联的JFCE对象
    urma_jfc_t *jfc;              // 关联的JFC对象
    uint32_t reserved2[10];
} os_transport_cfg_t;

typedef struct {
    ThreadPoolTask *tasks;
    void *task_args;
    uint32_t task_num;
} task_group_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int request_completed;       // 该请求的所有task是否都已完成
    uint64_t total_tasks;        // 任务组总任务数
    uint64_t completed_tasks;    // 任务组已完成任务数
    task_group_t *task_group;    // 任务组，由主线程统一释放
    struct chunk_info *chunks;   // 本次请求关联的chunk数组，由主线程统一释放
} task_sync_t;

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

uint32_t wait_and_free_sync(void *handle, task_sync_t *sync_handle);

uint32_t os_transport_destroy(void *handle);

#endif   // OS_TRANSPORT_H
