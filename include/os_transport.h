#ifndef OS_TRANSPORT_H
#define OS_TRANSPORT_H

#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DEFAULT_CHUNK_SIZE (2 * 1024 * 1024)   // 2MB

struct buffer_info {
    void*  addr;   // 数据缓冲区地址
    size_t len;    // 数据长度
};

typedef struct os_transport_cfg {
    bool         urma_event_mode;
    uint8_t      reserved1[3];        // 保留字节，保持结构体对齐
    uint32_t     worker_thread_num;   // 线程池中工作线程数量
    urma_jfce_t* jfce;                // 关联的JFCE对象
    urma_jfc_t*  jfc;                 // 关联的JFC对象
    uint32_t     reserved2[10];
} os_transport_cfg_t;

typedef enum {
    NULL_TASK = 0,
    SEND_TASK,
    RECV_TASK,
} task_type_t;

typedef struct {

} urma_write_info_t;

typedef struct {

} h2d_info_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int request_completed; // 该请求的所有task是否都已完成
} task_sync_t;

// send类型的task参数，包括：
// 1. urma_write相关参数
// 2. 与主函数的同步信息
// 3. 发送chunk的相关参数，例如：chunk_id，是否为最后一个chunk等
typedef struct {
    // urma_write相关参数
    urma_write_info_t write_info;
    // 与主函数的同步信息
    task_sync_t *sync;
    // chunk相关参数
    uint32_t chunk_id;
    bool is_last_chunk;
} send_task_arg_t;

typedef struct {
    // urma_read相关参数
    h2d_info_t h2d_info;
    // 与主函数的同步信息
    task_sync_t *sync;
    // chunk相关参数
    uint32_t chunk_id;
    bool is_last_chunk;
} recv_task_arg_t;



/**
 * @brief 传输层初始化
 * @param config 配置参数（0=默认）
 * @return 0=成功，非0=失败
 */
uint32_t os_transport_init(urma_context_t *urma_ctx, os_transport_cfg_t *ost_cfg, void **handle);

uint32_t os_transport_reg_jfc(urma_jfce_t *jfce, urma_jfc_t *jfc, void *handle);

uint32_t os_transport_send(void *handle, struct urma_jetty_info *jetty_info,
                           struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t buffer_num, uint32_t server_key, uint32_t client_key);

uint32_t os_transport_recv(void *handle, struct buffer_info *host_src,
                           struct buffer_info *device_dst, uint32_t buffer_num, uint32_t client_key);

uint32_t os_transport_destroy(void *handle);

#endif // OS_TRANSPORT_H