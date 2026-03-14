#ifndef OS_TRANSPORT_URMA_H
#define OS_TRANSPORT_URMA_H


#include <urma/urma_api.h>
#ifdef URMA_OVER_UB
#    include <urma/urma_ubagg.h>
#endif

struct chunk_info;

// #include <urma/cuda_runtime.h>
// 这里简单定义一个cudaStream_t结构体，实际使用时删除这个定义，直接包含cuda_runtime.h头文件
#include <stddef.h>
#include <stdint.h>
typedef struct {
    int i;
} cudaStream_t;

typedef struct {
    urma_jfs_t *jfs;
    urma_jetty_t *jetty;
    urma_target_jetty_t *target_jfr;
    urma_target_seg_t *dst_tseg;
    urma_target_seg_t *src_tseg;
    urma_jfs_wr_flag_t flag;
    uint32_t user_ctx_server;
    uint32_t user_ctx_client;
} urma_write_info_t;

typedef struct {
    void *dst;             // 设备地址
    cudaStream_t stream;   // CUDA流
} device_info_t;

typedef struct {
    device_info_t device_info; // 设备信息
    uint32_t request_id;       // 请求ID
} urma_recv_info_t;

typedef union {
    urma_write_info_t write_info;
    urma_recv_info_t recv_info;
} urma_info_t;

typedef enum jetty_mode { JETTY_MODE_SIMPLEX = 0, JETTY_MODE_DUPLEX } jetty_mode_t;

typedef struct urma_jetty_info {
    urma_jfs_t *jfs;             /* [Public] see urma_jetty_info. */
    urma_jetty_t *jetty;         /* [Public] see urma_jetty_info. */
    urma_target_jetty_t *tjetty; /* [Public] see urma_jetty_info. */
    jetty_mode_t jetty_mode;     /* [Public] see urma_jetty_info. */
} urma_jetty_info_t;

urma_status_t urma_write_with_notify(urma_write_info_t write_info, struct chunk_info *chunk_info);

enum cudaMemcpyKind {
    cudaMemcpyHostToHost = 0,
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2,
    cudaMemcpyDeviceToDevice = 3,
    cudaMemcpyDefault = 4
};

int cudaMemcpyAsync(void *dst, const void *src, size_t count, enum cudaMemcpyKind kind, cudaStream_t stream);
#endif   // OS_TRANSPORT_URMA_H
