#ifndef OS_TRANSPORT_THREAD_POOL_H
#define OS_TRANSPORT_THREAD_POOL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 任务结构体
 */
typedef struct {
    uint64_t task_id;                 // 唯一任务ID
    void (*task_func)(void* arg);     // 任务执行函数
    void* task_arg;                   // 任务参数
    bool is_completed;                // 任务完成标记
} OSThreadPoolTask;

/**
 * @brief 任务完成回调函数（通知外部任务结果）
 * @param task_id 任务ID
 * @param success 任务是否执行成功
 * @param user_data 外部透传数据
 */
typedef void (*OSTaskCompleteCb)(uint64_t task_id, bool success, void* user_data);

/**
 * @brief 线程池句柄（对外隐藏内部结构）
 */
typedef struct _OSThreadPool* OSThreadPoolHandle;

/**
 * @brief 初始化线程池（1个asyncPoll + 64个worker，仅初始化不运行）
 * @param worker_queue_cap 每个worker队列容量
 * @param pending_queue_cap 全局pending队列初始容量（0=默认1024）
 * @return 线程池句柄（NULL=失败）
 */
OSThreadPoolHandle os_thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap);

/**
 * @brief 启动线程池（所有线程开始等待任务）
 * @param handle 线程池句柄
 * @return 0=成功，-1=失败
 */
int os_thread_pool_start(OSThreadPoolHandle handle);

/**
 * @brief 外部提交任务（触发中断，通知asyncPoll处理）
 * @param handle 线程池句柄
 * @param task_func 任务执行函数
 * @param task_arg 任务参数（需用户自行管理内存）
 * @param complete_cb 任务完成回调
 * @param user_data 回调透传数据
 * @return 任务ID（0=失败）
 */
uint64_t os_thread_pool_submit_task(OSThreadPoolHandle handle,
                                    void (*task_func)(void* arg),
                                    void* task_arg,
                                    OSTaskCompleteCb complete_cb,
                                    void* user_data);

/**
 * @brief 通知asyncPoll线程（通用接口，支持自定义事件）
 * @param handle 线程池句柄
 * @param notify_type 通知类型（0=任务提交，1+自定义）
 * @param data 通知附带数据（需用户自行管理内存）
 * @return 0=成功，-1=失败
 */
int os_async_poll_notify(OSThreadPoolHandle handle, uint32_t notify_type, void* data);

/**
 * @brief 销毁线程池（等待所有任务完成后释放资源）
 * @param handle 线程池句柄
 */
void os_thread_pool_destroy(OSThreadPoolHandle handle);

#endif // OS_THREAD_POOL_H