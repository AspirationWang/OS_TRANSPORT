#ifndef OS_TRANSPORT_THREAD_POOL_INTERNAL_H
#define OS_TRANSPORT_THREAD_POOL_INTERNAL_H

#include "os_transport_thread_pool.h"
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

// Fix: Explicitly define the TRANSPORT_LOG macro (thread-safe, compatible with all source files)
#ifndef TRANSPORT_LOG
#define TRANSPORT_LOG(level, fmt, ...) \
    do { \
        fprintf(stderr, "[%s][%s:%d] " fmt "\n", level, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#endif // OS_TRANSPORT_INTERNAL_H

// Worker线程参数
typedef struct {
    struct _ThreadPool* pool;
    int worker_idx;
} WorkerThreadArg;

// Worker线程结构体（包含状态+环形队列）
typedef struct {
    pthread_t tid;                  // 线程ID
    bool is_idle;                   // 是否空闲
    bool is_running;                // 是否运行
    ThreadPoolTask* task_queue;     // 环形任务队列
    uint32_t queue_cap;             // 队列容量
    uint32_t queue_head;            // 队列头指针
    uint32_t queue_tail;            // 队列尾指针
    uint32_t queue_size;            // 队列当前大小
    pthread_mutex_t mutex;          // 队列/状态锁
    pthread_cond_t cond_task;       // 任务通知条件变量
} WorkerThread;

// 全局pending任务队列（缓存队列满的任务）
typedef struct {
    ThreadPoolTask** tasks;         // 任务指针数组
    uint32_t cap;                   // 队列容量
    uint32_t size;                  // 当前任务数
    uint32_t head;                  // 头指针
    uint32_t tail;                  // 尾指针
    pthread_mutex_t mutex;          // 队列锁
    pthread_cond_t cond_pending;    // pending队列有任务的条件
} PendingTaskQueue;

// 线程池内部结构（新增pending队列）
struct _ThreadPool {
    // 线程基础配置
    pthread_t async_poll_tid;       // asyncPoll线程ID
    WorkerThread workers[64];       // 64个worker线程
    bool is_initialized;            // 初始化完成标记
    bool is_running;                // 线程池是否启动
    bool is_destroying;             // 销毁标记

    // 任务ID生成（互斥锁保护）
    uint64_t next_task_id;          // 下一个任务ID
    pthread_mutex_t task_id_mutex;  // 任务ID锁

    // 同步控制
    pthread_mutex_t global_mutex;   // 全局锁
    pthread_cond_t cond_interrupt;  // 外部中断条件
    pthread_cond_t cond_all_done;   // 所有任务完成条件

    // 通知缓存
    uint32_t notify_type;           // 通知类型
    void* notify_data;              // 通知数据
    bool has_notify;                // 是否有未处理的通知

    // 任务完成回调
    TaskCompleteCallback complete_cb;
    void* complete_user_data;

    // 任务统计（互斥锁保护）
    uint32_t running_tasks;         // 运行中任务数
    uint32_t completed_tasks;       // 已完成任务数
    pthread_mutex_t stats_mutex;    // 统计锁

    // 全局pending队列（核心补充）
    PendingTaskQueue pending_queue; // 缓存队列满的任务
};

#endif // OS_TRANSPORT_THREAD_POOL_INTERNAL_H