#include "os_transport_thread_pool.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/**
 * @brief 测试任务执行函数（模拟耗时任务）
 */
void test_task_func(void* arg) {
    int task_data = *(int*)arg;
    printf("[TASK] task %d executing (data=%d)\n", task_data, task_data);
    sleep(1); // 模拟1秒耗时
    free(arg); // 释放用户参数
}

/**
 * @brief 任务完成回调
 */
void test_complete_cb(uint64_t task_id, bool success, void* user_data) {
    char* msg = (char*)user_data;
    printf("[CALLBACK] task %lu %s → %s\n", task_id, success ? "success" : "failed", msg);
}

/**
 * @brief 自定义事件通知示例
 */
void test_custom_notify(OSThreadPoolHandle pool) {
    char* custom_data = "system: resource check completed";
    os_async_poll_notify(pool, 1, custom_data); // 类型1：自定义事件
}

int main() {
    // 1. 初始化线程池（每个worker队列容量2，pending队列初始容量10）
    OSThreadPoolHandle pool = os_thread_pool_init(2, 10);
    if (pool == NULL) {
        printf("thread pool init failed\n");
        return -1;
    }

    // 2. 启动线程池
    if (os_thread_pool_start(pool) != 0) {
        printf("thread pool start failed\n");
        os_thread_pool_destroy(pool);
        return -1;
    }

    // 3. 提交大量任务（验证pending缓存）
    int task_count = 200; // 64*2=128 → 72个进入pending
    for (int i = 0; i < task_count; i++) {
        int* task_arg = (int*)malloc(sizeof(int));
        *task_arg = i + 1;
        uint64_t task_id = os_thread_pool_submit_task(pool,
                                                      test_task_func,
                                                      task_arg,
                                                      test_complete_cb,
                                                      "task finish notify");
        if (task_id == 0) {
            printf("submit task %d failed\n", i+1);
            free(task_arg);
            continue;
        }
        if ((i+1) % 50 == 0) {
            printf("submitted %d tasks, last task_id=%lu\n", i+1, task_id);
        }
    }

    // 4. 触发自定义事件通知
    test_custom_notify(pool);

    // 5. 等待所有任务完成（pending任务会被逐步分发）
    printf("waiting all %d tasks completed...\n", task_count);
    sleep(30); // 根据任务数调整等待时间

    // 6. 销毁线程池
    os_thread_pool_destroy(pool);
    return 0;
}