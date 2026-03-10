#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "os_transport_thread_pool.h"

/**
 * @brief 测试任务执行函数（模拟耗时）
 */
void test_task_func(void* arg) {
    int task_data = *(int*)arg;
    printf("[TASK] Task %d executing (data=%d)\n", task_data, task_data);
    sleep(1); // 模拟1秒耗时
    free(arg);
}

/**
 * @brief 任务完成回调
 */
void test_complete_cb(uint64_t task_id, bool success, void* user_data) {
    char* msg = (char*)user_data;
    printf("[CALLBACK] Task %lu %s → %s\n", task_id, success ? "success" : "failed", msg);
}

/**
 * @brief 自定义事件通知示例
 */
void test_custom_notify(ThreadPoolHandle pool) {
    char* custom_data = "System: Resource check completed";
    async_poll_notify(pool, 1, custom_data);
}

int main() {
    // 1. 初始化线程池（Worker队列容量2，Pending队列初始容量10）
    ThreadPoolHandle pool = thread_pool_init(2, 10);
    if (pool == NULL) {
        printf("ThreadPool init failed\n");
        return -1;
    }

    // 2. 启动线程池
    if (thread_pool_start(pool) != 0) {
        printf("ThreadPool start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 3. 提交大量任务（验证Pending缓存+Worker按需唤醒）
    int task_count = 200;
    for (int i = 0; i < task_count; i++) {
        int* task_arg = (int*)malloc(sizeof(int));
        *task_arg = i + 1;
        uint64_t task_id = thread_pool_submit_task(pool,
                                                      test_task_func,
                                                      task_arg,
                                                      test_complete_cb,
                                                      "Task finish notify");
        if (task_id == 0) {
            printf("Submit task %d failed\n", i+1);
            free(task_arg);
            continue;
        }
        if ((i+1) % 50 == 0) {
            printf("Submitted %d tasks, last task ID: %lu\n", i+1, task_id);
        }
    }

    // 4. 触发自定义事件
    test_custom_notify(pool);

    // 5. 等待所有任务完成
    printf("Waiting all %d tasks completed...\n", task_count);
    sleep(30);

    // 6. 销毁线程池
    thread_pool_destroy(pool);
    return 0;
}