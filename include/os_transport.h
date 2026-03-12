#ifndef OS_TRANSPORT_H
#define OS_TRANSPORT_H

#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <urma/urma_api.h>
#ifdef URMA_OVER_UB
#    include <urma/urma_ubagg.h>
#endif

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
        uint64_t requeset_id : 32;
    } bs;
    uint64_t user_ctx;
} os_transport_user_data_t;

struct buffer_info {
    uint64_t  addr;   // 数据缓冲区地址
    urma_target_seg_t *tseg; // 目标分段信息
};

struct chunk_info {
    uint64_t src;   // 源缓冲区地址
    uint64_t dst;   // 目标缓冲区地址
    uint32_t len;      // 数据长度
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
    urma_jfs_t *jfs;
    urma_target_jetty_t *target_jfr;
    urma_target_seg_t *dst_tseg;
    urma_target_seg_t *src_tseg;
    urma_jfs_wr_flag_t flag;
    uint64_t user_ctx;
} urma_write_info_t;

typedef struct {

} urma_recv_info_t;

typedef union {
    urma_write_info_t write_info;
    urma_recv_info_t recv_info;
} urma_info_t;


typedef struct {

} recv_info_t;

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
    recv_info_t recv_info;
} recv_task_arg_t;

typedef enum jetty_mode {
    JETTY_MODE_SIMPLEX = 0,
    JETTY_MODE_DUPLEX
} jetty_mode_t;

typedef struct urma_jetty_info {
    urma_jfs_t *jfs; /* [Public] see urma_jetty_info. */
    urma_jetty_t *jetty; /* [Public] see urma_jetty_info. */
    urma_target_jetty_t *tjetty; /* [Public] see urma_jetty_info. */
    jetty_mode_t jetty_mode; /* [Public] see urma_jetty_info. */
} urma_jetty_info_t; 

typedef struct os_transport_handle {
    urma_context_t *urma_ctx; /* [Private] point to urma context. */
    os_transport_cfg_t *ost_cfg; /* [Private] point to os transport config. */
    ThreadPoolHandle *thread_pool; /* [Private] 线程池句柄 */
} os_transport_handle_t;

uint32_t os_transport_init(urma_context_t *urma_ctx, os_transport_cfg_t *ost_cfg, void **handle);

uint32_t os_transport_reg_jfc(urma_jfce_t *jfce, urma_jfc_t *jfc, void *handle);

uint32_t os_transport_send(void *handle, struct urma_jetty_info *jetty_info,
                           struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t buffer_num, uint32_t server_key, uint32_t client_key);

uint32_t os_transport_recv(void *handle, struct buffer_info *host_src,
                           struct buffer_info *device_dst, uint32_t buffer_num, uint32_t client_key);

uint32_t os_transport_destroy(void *handle);

#endif // OS_TRANSPORT_H