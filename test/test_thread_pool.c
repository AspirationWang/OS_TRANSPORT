#include "../include/os_transport_thread_pool.h"
#include "../include/os_transport_internal.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

// 任务执行函数（模拟耗时任务）
void test_task_func(void* arg) {
    int task_data = *(int*)arg;
    TRANSPORT_LOG("INFO", "task %d execute: processing data=%d", task_data, task_data);
    sleep(2); // 模拟任务执行（2秒，让worker长时间忙碌）
    free(arg);
}

// 任务完成回调
void test_complete_cb(uint64_t task_id, bool success, void* user_data) {
    char* msg = (char*)user_data;
    TRANSPORT_LOG("INFO", "external notify: task %lu %s → %s",
                 task_id, success ? "success" : "failed", msg);
}

int main() {
    // 1. 初始化线程池（每个worker队列容量2，pending队列初始容量10）
    ThreadPoolHandle pool = thread_pool_init(2, 10);
    if (pool == NULL) {
        printf("thread pool init failed\n");
        return -1;
    }

    // 2. 启动线程池
    if (thread_pool_start(pool) != 0) {
        printf("thread pool start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 3. 提交大量任务（超过worker队列总容量，验证pending缓存）
    int task_count = 200; // 64*2=128 → 超过的72个任务进入pending
    for (int i = 0; i < task_count; i++) {
        int* task_arg = (int*)malloc(sizeof(int));
        *task_arg = i + 1;
        uint64_t task_id = thread_pool_submit_task(pool,
                                                   test_task_func,
                                                   task_arg,
                                                   test_complete_cb,
                                                   "task finish notify");
        if (task_id == 0) {
            printf("submit task %d failed\n", i+1);
            free(task_arg);
            continue;
        }
        if ((i+1) % 20 == 0) {
            printf("submit %d tasks, last task_id=%lu\n", i+1, task_id);
        }
    }

    // 4. 等待所有任务完成（pending任务会被逐步分发）
    printf("wait all %d tasks completed...\n", task_count);
    sleep(60); // 根据任务数调整等待时间

    // 5. 销毁线程池
    thread_pool_destroy(pool);
    return 0;
}