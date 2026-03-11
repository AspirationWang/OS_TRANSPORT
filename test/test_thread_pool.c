#include "os_transport_thread_pool.h"
#include <stdio.h>

// 示例任务函数
void test_task(void* arg) {
    uint32_t idx = *(uint32_t*)arg;
    printf("Worker execute task: %u\n", idx);
    free(arg); // 释放参数
}

// 任务完成回调
void task_complete_cb(uint64_t task_id, bool success, void* user_data) {
    printf("Task %lu completed: %s\n", task_id, success ? "success" : "failed");
}

int main() {
    // 初始化线程池：单个worker队列容量8，pending队列初始容量1024
    ThreadPoolHandle pool = thread_pool_init(8, 1024);
    if (!pool) {
        printf("ThreadPool init failed\n");
        return -1;
    }

    // 启动线程池
    if (thread_pool_start(pool) != 0) {
        printf("ThreadPool start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 提交单个任务
    uint32_t* arg1 = malloc(sizeof(uint32_t));
    *arg1 = 1;
    uint64_t task_id = thread_pool_submit_task(pool, test_task, arg1, task_complete_cb, NULL);
    printf("Submit single task: %lu\n", task_id);

    // 批量提交任务
    ThreadPoolTask batch_tasks[3];
    for (int i = 0; i < 3; i++) {
        uint32_t* arg = malloc(sizeof(uint32_t));
        *arg = i + 2;
        batch_tasks[i].task_func = test_task;
        batch_tasks[i].task_arg = arg;
    }
    uint64_t* batch_ids = thread_pool_submit_batch_tasks(pool, batch_tasks, 3, task_complete_cb, NULL);
    if (batch_ids) {
        printf("Submit batch tasks: ");
        for (int i = 0; i < 3; i++) {
            printf("%lu ", batch_ids[i]);
        }
        printf("\n");
        free(batch_ids);
    }

    // 等待任务完成（实际业务中可根据需求调整）
    sleep(1);

    // 销毁线程池
    thread_pool_destroy(pool);
    return 0;
}