#ifndef OS_TRANSPORT_THREAD_POOL_INTERNAL_H
#define OS_TRANSPORT_THREAD_POOL_INTERNAL_H

#include "os_transport_thread_pool.h"
#include "urma_api.h"
#include "urma_cmd.h"
#include "urma_opcode.h"
#include "urma_provider.h"
#include "urma_types_str.h"
#include "urma_types.h"
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>

struct union {
    struct {
        uint64_t chunk_type   : 2;
        uint64_t chunk_id     : 6;
        uint64_t chunk_size   : 24;
        unint64_t request_id  : 32;
    } bs;
    uint64_t user_ctx;
} TransportData;

/**
 * @brief Worker线程状态枚举
 */
typedef enum {
    WORKER_STATE_INIT = 0,    // 初始化未运行
    WORKER_STATE_IDLE = 1,    // 空闲（已启动，等待任务）
    WORKER_STATE_BUSY = 2,    // 忙碌（执行任务）
    WORKER_STATE_EXIT = 3     // 退出
} WorkerState;

/**
 * @brief Worker线程结构体
 */
typedef struct {
    pthread_t tid;                  // 线程ID
    pthread_mutex_t mutex;          // 线程锁
    pthread_cond_t cond_task;       // 任务通知条件变量
    WorkerState state;              // 线程状态（IDLE/BUSY/EXIT等）
    int worker_idx;                 // 线程索引
    ThreadPoolHandle pool;          // 所属线程池句柄
    ThreadPoolTask** task_queue;    // 任务指针数组（循环队列）
    uint32_t queue_cap;             // 队列容量
    uint32_t queue_head;            // 队列头指针
    uint32_t queue_tail;            // 队列尾指针
    uint32_t queue_size;            // 当前队列任务数
    uint32_t pending_req;           // 等待执行的 request_id（0表示无待执行）
} WorkerThread;

/**
 * @brief 请求上下文（用于记录request_id与worker的绑定及批次信息）
 */
typedef struct RequestContext {
    uint32_t request_id;
    int worker_idx;                    // 绑定的 worker 索引
    int pending_count;                  // 剩余任务数
    TaskCompleteCb batch_cb;            // 批次完成回调
    void* batch_user_data;
    struct RequestContext* next;        // 哈希冲突链表
} RequestContext;

/**
 * @brief 通知项（用于通用通知）
 */
typedef struct {
    uint32_t type;
    void* data;
} NotifyItem;

/**
 * @brief urma相关信息，用于与asyncPool线程绑定
 */
typedef struct {
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    bool urma_event_mode;
} ThreadPoolUrmaInfo;

#define REQ_HASH_SIZE 1024
#define EPOLL_TIME 100
#define POLL_SIZE  10
#define POLL_TRY_CNT 10

/**
 * @brief 线程池内部结构
 */
struct _ThreadPool {
    // 线程基础配置
    pthread_t async_poll_tid;       // asyncPoll线程ID
    WorkerThread workers[64];        // 64个Worker线程
    bool is_initialized;            // 初始化完成标记
    bool is_running;                // 线程池运行标记
    bool is_destroying;             // 销毁标记

    // 任务ID生成
    uint64_t next_task_id;          // 下一个任务ID
    pthread_mutex_t task_id_mutex;  // 任务ID锁

    // 中断&同步控制
    pthread_mutex_t global_mutex;   // 全局锁（用于通知队列等）
    pthread_cond_t cond_interrupt;  // 外部中断条件（保留，当前未使用）

    // 线程启动同步
    pthread_cond_t cond_start;      // 线程启动信号
    pthread_mutex_t start_mutex;    // 启动控制锁
    bool is_started;                // asyncPoll是否已启动

    // 通用通知队列（保留以备扩展）
    NotifyItem* notify_queue;       // 通知队列数组
    uint32_t notify_queue_cap;      // 队列容量
    uint32_t notify_queue_head;     // 队列头（取通知）
    uint32_t notify_queue_tail;     // 队列尾（存通知）
    uint32_t notify_queue_size;     // 队列当前通知数

    // URMA 相关信息
    ThreadPoolUrmaInfo urmaInfo;

    // request_id 哈希表
    RequestContext* req_hash[REQ_HASH_SIZE];
    pthread_mutex_t req_hash_mutex; // 保护哈希表
};

// 日志级别
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

// 全局日志级别控制（可通过编译宏/配置修改）
#ifndef GLOBAL_LOG_LEVEL
#define GLOBAL_LOG_LEVEL LOG_LEVEL_DEBUG
#endif

// 获取当前时间字符串
static inline const char* get_log_time() {
    static char time_buf[32];
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return time_buf;
}

// 日志格式化输出宏
#define LOG(level, fmt, ...) do { \
    if (level >= GLOBAL_LOG_LEVEL) { \
        const char* level_str = NULL; \
        switch(level) { \
            case LOG_LEVEL_DEBUG: level_str = "DEBUG"; break; \
            case LOG_LEVEL_INFO:  level_str = "INFO";  break; \
            case LOG_LEVEL_WARN:  level_str = "WARN";  break; \
            case LOG_LEVEL_ERROR: level_str = "ERROR"; break; \
        } \
        fprintf(stderr, "[%s][%s][%s:%d] " fmt "\n", \
            get_log_time(), level_str, __FILE__, __LINE__, ##__VA_ARGS__); \
    } \
} while(0)

// 快捷日志宏
#define LOG_DEBUG(fmt, ...) LOG(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

#endif // OS_TRANSPORT_THREAD_POOL_INTERNAL_H