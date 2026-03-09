#ifndef OS_TRANSPORT_THREAD_POOL_H
#define OS_TRANSPORT_THREAD_POOL_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

// 线程类型枚举（仅保留两类）
typedef enum {
    THREAD_TYPE_ASYNC_POLL = 0,  // 异步轮询线程（1个）
    THREAD_TYPE_WORKER = 1,      // 工作线程（64个）
    THREAD_TYPE_MAX = 2
} ThreadType;

// 任务结构体
typedef struct {
    uint64_t task_id;                 // 唯一任务ID
    void (*task_func)(void* arg);     // 任务执行函数
    void* task_arg;                   // 任务参数
    bool is_completed;                // 任务是否完成
} ThreadPoolTask;

// 任务完成回调（asyncPoll通知外部）
typedef void (*TaskCompleteCallback)(uint64_t task_id, bool success, void* user_data);

// 线程池句柄（对外隐藏内部结构）
typedef struct _ThreadPool* ThreadPoolHandle;

/**
 * @brief 初始化线程池（1个asyncPoll + 64个worker，仅初始化不运行）
 * @param queue_cap 每个worker队列的容量
 * @param pending_cap 全局pending队列初始容量（0=默认1024）
 * @return 线程池句柄/NULL
 */
ThreadPoolHandle thread_pool_init(uint32_t queue_cap, uint32_t pending_cap);

/**
 * @brief 启动线程池（所有线程开始等待任务）
 * @param handle 线程池句柄
 * @return 0=成功，-1=失败
 */
int thread_pool_start(ThreadPoolHandle handle);

/**
 * @brief 外部提交任务（触发中断，通知asyncPoll处理）
 * @param handle 线程池句柄
 * @param task_func 任务执行函数
 * @param task_arg 任务参数
 * @param complete_cb 任务完成回调
 * @param user_data 回调用户数据
 * @return 任务ID（失败返回0）
 */
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                 void (*task_func)(void* arg),
                                 void* task_arg,
                                 TaskCompleteCallback complete_cb,
                                 void* user_data);

/**
 * @brief 通知asyncPoll线程（供其他事件调用）
 * @param handle 线程池句柄
 * @param notify_type 通知类型（自定义，如0=任务、1=其他事件）
 * @param data 通知附带数据（需自行管理内存）
 * @return 0=成功，-1=失败
 */
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data);

/**
 * @brief 销毁线程池（等待所有任务完成后退出）
 * @param handle 线程池句柄
 */
void thread_pool_destroy(ThreadPoolHandle handle);

#endif // OS_TRANSPORT_THREAD_POOL_H