#include "os_transport_thread_pool_internal.h"
#include "os_transport_thread_pool.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

// 任务执行函数
void test_task_func(void* arg) {
    int task_data = *(int*)arg;
    TRANSPORT_LOG("INFO", "task execute: data=%d", task_data);
    sleep(1); // 模拟任务执行
    free(arg); // 释放参数
}

// 任务完成回调（asyncPoll通知外部）
void test_complete_cb(uint64_t task_id, bool success, void* user_data) {
    char* msg = (char*)user_data;
    TRANSPORT_LOG("INFO", "external notify: task %lu %s, msg=%s",
                 task_id, success ? "success" : "failed", msg);
}

// 其他事件通知的回调（示例）
void other_event_notify(ThreadPoolHandle pool) {
    char* data = "other event data";
    async_poll_notify(pool, 1, data); // 类型1：其他事件
}

int main() {
    // 1. 初始化线程池（每个worker队列容量16）
    ThreadPoolHandle pool = thread_pool_init(16);
    if (pool == NULL) {
        printf("init failed\n");
        return -1;
    }

    // 2. 启动线程池
    if (thread_pool_start(pool) != 0) {
        printf("start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 3. 提交任务
    int* task_arg = (int*)malloc(sizeof(int));
    *task_arg = 12345;
    uint64_t task_id = thread_pool_submit_task(pool,
                                               test_task_func,
                                               task_arg,
                                               test_complete_cb,
                                               "task finish notify");
    if (task_id == 0) {
        printf("submit task failed\n");
        free(task_arg);
        thread_pool_destroy(pool);
        return -1;
    }

    // 4. 触发其他事件通知asyncPoll
    other_event_notify(pool);

    // 5. 等待任务完成
    sleep(2);

    // 6. 销毁线程池
    thread_pool_destroy(pool);
    return 0;
}