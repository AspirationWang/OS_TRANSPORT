#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

// 外部任务执行函数
void test_worker_task(void* arg) {
    int task_data = *(int*)arg;
    TRANSPORT_LOG("INFO", "worker execute task: data=%d", task_data);
    // 模拟任务执行（1秒）
    sleep(1);
    // 外部参数自行释放
    free(arg);
}

// 任务完成回调（asyncPoll通知外部）
void test_complete_callback(void* arg, bool success) {
    char* msg = (char*)arg;
    TRANSPORT_LOG("INFO", "external callback: %s, success=%d", msg, success);
}

int main() {
    // 1. 初始化线程池（队列容量1024）
    ThreadPoolHandle pool = thread_pool_init(1024);
    if (pool == NULL) {
        printf("thread pool init failed\n");
        return -1;
    }

    // 2. 启动线程池（线程开始等待任务）
    if (thread_pool_start(pool) != 0) {
        printf("thread pool start failed\n");
        thread_pool_destroy(pool);
        return -1;
    }

    // 3. 构造任务
    ThreadPoolTask task;
    memset(&task, 0, sizeof(task));
    task.task_func = test_worker_task;
    int* task_data = (int*)malloc(sizeof(int));
    *task_data = 12345;
    task.task_arg = task_data;
    task.complete_cb = test_complete_callback;
    task.complete_arg = "task finish notify";

    // 4. 外部触发中断：通知asyncPoll处理任务
    if (thread_pool_trigger_interrupt(pool, &task) != 0) {
        printf("trigger interrupt failed\n");
        free(task_data);
        thread_pool_destroy(pool);
        return -1;
    }

    // 5. 等待任务完成
    sleep(2);

    // 6. 销毁线程池
    thread_pool_destroy(pool);
    return 0;
}