#include "os_transport_thread_pool_internal.h"
#include "os_transport_thread_pool.h"
#include <stdio.h>
#include <stdlib.h>

// 示例任务函数
void demo_task(void* arg) {
    uint32_t task_no = *(uint32_t*)arg;
    LOG_INFO("demo task %u executing", task_no);
    free(arg); // 释放参数内存
}

// 任务完成回调
void demo_complete_cb(uint64_t task_id, bool success, void* user_data) {
    LOG_INFO("task[%lu] completed (success:%s, user_data:%s)",
            task_id, success ? "yes" : "no", (char*)user_data);
}

int main() {
    // 1. 初始化线程池（worker队列容量8，pending队列容量1024）
    ThreadPoolHandle pool = thread_pool_init(8, 1024);
    if (!pool) {
        printf("thread pool init failed\n");
        return -1;
    }

    // 2. 启动线程池
    if (thread_pool_start(pool) != 0) {
        printf("thread pool start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 3. 提交单个任务
    uint32_t* arg1 = malloc(sizeof(uint32_t));
    *arg1 = 1;
    uint64_t task1_id = thread_pool_submit_task(pool, demo_task, arg1, demo_complete_cb, "single task");
    printf("submit single task, id:%lu\n", task1_id);

    // 4. 提交批量任务
    uint32_t batch_count = 3;
    ThreadPoolTask* batch_tasks = malloc(batch_count * sizeof(ThreadPoolTask));
    for (uint32_t i = 0; i < batch_count; i++) {
        uint32_t* arg = malloc(sizeof(uint32_t));
        *arg = i + 2;
        batch_tasks[i].task_func = demo_task;
        batch_tasks[i].task_arg = arg;
    }
    uint64_t* batch_ids = thread_pool_submit_batch_tasks(pool, batch_tasks, batch_count, demo_complete_cb, "batch task");
    printf("submit batch tasks, ids:");
    for (uint32_t i = 0; i < batch_count; i++) {
        printf(" %lu", batch_ids[i]);
    }
    printf("\n");

    // 5. 自定义通知（示例）
    async_poll_notify(pool, 1, "custom notify data");

    // 6. 等待任务完成（实际业务中可根据需求调整）
    sleep(2);

    // 7. 销毁线程池
    thread_pool_destroy(pool);
    free(batch_tasks);
    free(batch_ids);

    return 0;
}