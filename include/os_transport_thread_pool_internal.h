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

// 任务队列结构体
typedef struct {
    ThreadPoolTask* tasks;
    uint32_t cap;
    uint32_t size;
    uint32_t head;
    uint32_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;  // 队列非空（线程等待任务）
    pthread_cond_t cond_not_full;   // 队列非满（入队等待）
    pthread_cond_t cond_task_done;  // 任务完成（asyncPoll等待）
} TaskQueue;

// 线程池内部结构
struct _ThreadPool {
    // 线程基础配置
    pthread_t threads[THREAD_TYPE_MAX][64];  // 线程句柄（worker最多64）
    uint32_t thread_nums[THREAD_TYPE_MAX];   // 固定：[1,1,64]
    bool is_initialized;                     // 初始化完成标记
    bool is_running;                         // 线程是否启动（false=阻塞）
    bool is_destroying;                      // 销毁标记

    // 队列：asyncPoll→notifier→worker
    TaskQueue async_poll_queue;    // asyncPoll接收外部中断的队列
    TaskQueue notifier_queue;      // notifier转发给worker的队列
    TaskQueue worker_queue;        // worker执行的任务队列

    // 同步控制
    pthread_mutex_t global_mutex;          // 全局锁
    pthread_cond_t cond_thread_start;      // 线程启动信号（初始化后触发）
    pthread_cond_t cond_interrupt;         // 外部中断信号（通知asyncPoll）
    uint64_t next_task_id;                 // 下一个任务ID（自增）
};

// 内部函数：任务完成通知
void notify_task_complete(ThreadPoolHandle handle, ThreadPoolTask* task, bool success);

#endif // OS_TRANSPORT_THREAD_POOL_INTERNAL_H