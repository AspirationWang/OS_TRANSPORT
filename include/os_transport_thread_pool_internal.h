#ifndef OS_TRANSPORT_THREAD_POOL_INTERNAL_H
#define OS_TRANSPORT_THREAD_POOL_INTERNAL_H

#include "os_transport_thread_pool.h"
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

// Fix: Explicitly define the TRANSPORT_LOG macro (thread-safe, compatible with all source files)
#define TRANSPORT_LOG(level, fmt, ...) \
    do { \
        fprintf(stderr, "[%s][%s:%d] " fmt "\n", level, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while(0)

#endif // OS_TRANSPORT_INTERNAL_H

// 任务队列结构体（每个线程类型对应1个队列）
typedef struct {
    ThreadPoolTask* tasks;          // 任务数组
    uint32_t cap;                   // 队列容量（最大任务数）
    uint32_t size;                  // 当前队列中的任务数
    uint32_t head;                  // 队列头指针（取任务）
    uint32_t tail;                  // 队列尾指针（加任务）
    pthread_mutex_t mutex;          // 队列互斥锁
    pthread_cond_t cond_not_empty;  // 队列非空条件变量（线程等待任务）
    pthread_cond_t cond_not_full;   // 队列非满条件变量（提交任务等待）
} TaskQueue;

// 线程池内部结构
struct _ThreadPool {
    // 1. 线程相关
    pthread_t threads[THREAD_TYPE_MAX][32];  // 线程句柄（每种线程最多32个，可扩展）
    uint32_t thread_nums[THREAD_TYPE_MAX];   // 每种线程的实际数量
    bool is_running;                         // 线程池运行状态

    // 2. 任务队列（每种线程1个正式队列 + 1个缓存队列）
    TaskQueue task_queues[THREAD_TYPE_MAX];          // 正式任务队列（线程消费）
    TaskQueue task_cache_queues[THREAD_TYPE_MAX];    // 缓存队列（队列满时暂存）

    // 3. 全局锁（保护线程池整体状态）
    pthread_mutex_t global_mutex;
};

// 内部函数：处理缓存队列（将缓存任务移入正式队列）
void process_cache_queue(ThreadPoolHandle handle, ThreadType type);

#endif // OS_TRANSPORT_THREAD_POOL_INTERNAL_H