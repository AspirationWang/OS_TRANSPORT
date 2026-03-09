#ifndef OS_TRANSPORT_THREAD_POOL_INTERNAL_H
#define OS_TRANSPORT_THREAD_POOL_INTERNAL_H

#include "os_transport_thread_pool.h"
#include "os_transport_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>

// Fix: Explicitly define the TRANSPORT_LOG macro (thread-safe, compatible with all source files)
#ifndef TRANSPORT_LOG
#define TRANSPORT_LOG(level, fmt, ...) \
    do { \
        fprintf(stderr, "[%s][%s:%d] " fmt "\n", level, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#endif // OS_TRANSPORT_INTERNAL_H

// Worker线程结构体（包含状态+队列）
typedef struct {
    pthread_t tid;                  // 线程ID
    bool is_idle;                   // 是否空闲
    bool is_running;                // 是否运行
    ThreadPoolTask* task_queue;     // 任务队列
    uint32_t queue_cap;             // 队列容量
    uint32_t queue_size;            // 队列当前大小
    pthread_mutex_t mutex;          // 队列/状态锁
    pthread_cond_t cond_task;       // 任务通知条件变量
} WorkerThread;

// 线程池内部结构
struct _ThreadPool {
    // 线程基础配置
    pthread_t async_poll_tid;       // asyncPoll线程ID
    WorkerThread workers[64];       // 64个worker线程
    bool is_initialized;            // 初始化完成标记
    bool is_running;                // 线程池是否启动
    bool is_destroying;             // 销毁标记

    // 任务&通知相关
    atomic_uint64_t next_task_id;   // 下一个任务ID（原子类型）
    pthread_mutex_t global_mutex;   // 全局锁
    pthread_cond_t cond_interrupt;  // 外部中断条件（任务提交/其他通知）
    pthread_cond_t cond_all_done;   // 所有任务完成条件

    // 通知缓存（其他事件通知asyncPoll）
    uint32_t notify_type;           // 通知类型
    void* notify_data;              // 通知数据
    bool has_notify;                // 是否有未处理的通知

    // 任务完成回调
    TaskCompleteCallback complete_cb;
    void* complete_user_data;

    // 统计
    atomic_uint32_t running_tasks;  // 运行中任务数
    atomic_uint32_t completed_tasks;// 已完成任务数
};

#endif // OS_TRANSPORT_THREAD_POOL_INTERNAL_H