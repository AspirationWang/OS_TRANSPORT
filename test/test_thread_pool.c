// test_thread_pool.c (修正版)
#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>

// 测试全局状态
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int completed_count;
    int total_tasks;
    int expected_completions;
    int task_counter;          // 用于生成任务序号
    int* execution_order;       // 记录每个任务执行的序号（按完成顺序）
    pthread_t* thread_ids;       // 记录每个任务执行的线程ID
    int write_pos;               // 当前写入位置（在锁保护下使用）
    int notify_received[10];     // 记录接收到的通知类型
    int notify_count;
} TestState;

static TestState g_state;

// 初始化测试状态（每次测试前调用）
static void test_state_init(int total) {
    // 释放之前可能分配的内存
    if (g_state.execution_order) {
        free(g_state.execution_order);
        g_state.execution_order = NULL;
    }
    if (g_state.thread_ids) {
        free(g_state.thread_ids);
        g_state.thread_ids = NULL;
    }

    pthread_mutex_init(&g_state.lock, NULL);
    pthread_cond_init(&g_state.cond, NULL);
    g_state.completed_count = 0;
    g_state.total_tasks = total;
    g_state.expected_completions = total;
    g_state.task_counter = 0;
    g_state.execution_order = calloc(total, sizeof(int));
    g_state.thread_ids = calloc(total, sizeof(pthread_t));
    g_state.write_pos = 0;  // 重置写入位置
    g_state.notify_count = 0;
    memset(g_state.notify_received, 0, sizeof(g_state.notify_received));
}

// 等待所有任务完成
static void test_state_wait_completion(void) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.completed_count < g_state.expected_completions) {
        pthread_cond_wait(&g_state.cond, &g_state.lock);
    }
    pthread_mutex_unlock(&g_state.lock);
}

// 任务完成回调
static void task_complete_cb(uint64_t task_id, bool success, void* user_data) {
    printf("Callback: task %lu completed, success=%d\n", task_id, success);
    pthread_mutex_lock(&g_state.lock);
    g_state.completed_count++;
    pthread_cond_signal(&g_state.cond);
    pthread_mutex_unlock(&g_state.lock);
}

// 通用任务函数：记录执行线程ID和序号
static void sample_task(void* arg) {
    int* seq_ptr = (int*)arg;
    int seq = *seq_ptr;
    pthread_t self = pthread_self();

    pthread_mutex_lock(&g_state.lock);
    int pos = g_state.write_pos;
    // 确保不越界（理论上不会，但加保护）
    if (pos < g_state.total_tasks) {
        g_state.execution_order[pos] = seq;
        g_state.thread_ids[pos] = self;
        g_state.write_pos++;
    } else {
        fprintf(stderr, "Error: write_pos %d exceeded total_tasks %d\n", pos, g_state.total_tasks);
    }
    pthread_mutex_unlock(&g_state.lock);

    printf("Task %d (seq %d) executed by thread %lu\n", seq, seq, (unsigned long)self);
    usleep(10000); // 模拟工作
    free(arg);     // 释放传入的参数
}

// 批量任务专用函数：用于测试顺序
static void batch_task(void* arg) {
    int seq = *(int*)arg;
    pthread_t self = pthread_self();

    pthread_mutex_lock(&g_state.lock);
    int pos = g_state.write_pos;
    if (pos < g_state.total_tasks) {
        g_state.execution_order[pos] = seq;
        g_state.thread_ids[pos] = self;
        g_state.write_pos++;
    }
    pthread_mutex_unlock(&g_state.lock);

    printf("Batch task %d executed by thread %lu\n", seq, (unsigned long)self);
    usleep(5000);
    free(arg);
}

// 测试单个任务提交
static void test_single_task(ThreadPoolHandle pool) {
    printf("\n=== Test single task submission ===\n");
    int* arg = malloc(sizeof(int));
    *arg = 999;
    uint64_t task_id = thread_pool_submit_task(pool, sample_task, arg, task_complete_cb, NULL);
    assert(task_id != 0);
    printf("Submitted single task, id=%lu\n", task_id);
    test_state_wait_completion();
    printf("Single task done.\n");
}

// 测试批量任务提交（顺序保证）
static void test_batch_tasks(ThreadPoolHandle pool) {
    printf("\n=== Test batch tasks submission (order guarantee) ===\n");
    const int BATCH_COUNT = 5;
    ThreadPoolTask tasks[BATCH_COUNT];
    int* args[BATCH_COUNT];

    for (int i = 0; i < BATCH_COUNT; i++) {
        args[i] = malloc(sizeof(int));
        *args[i] = i;
        tasks[i].task_func = batch_task;
        tasks[i].task_arg = args[i];
        // task_id 和 is_completed 由内部填充，无需设置
    }

    uint64_t* task_ids = thread_pool_submit_batch_tasks(pool, tasks, BATCH_COUNT, task_complete_cb, NULL);
    assert(task_ids != NULL);

    printf("Submitted batch tasks, ids: ");
    for (int i = 0; i < BATCH_COUNT; i++) {
        printf("%lu ", task_ids[i]);
    }
    printf("\n");

    test_state_wait_completion();

    // 验证执行顺序：所有任务应该在同一线程中按序号递增执行
    pthread_t first_thread = g_state.thread_ids[0];
    for (int i = 0; i < BATCH_COUNT; i++) {
        assert(pthread_equal(g_state.thread_ids[i], first_thread));
        assert(g_state.execution_order[i] == i);
    }
    printf("Batch tasks order and thread affinity verified.\n");

    free(task_ids);
    // args 已在任务中释放
}

// 测试队列扩展（提交大量任务）
static void test_queue_expansion(ThreadPoolHandle pool) {
    printf("\n=== Test queue expansion (many tasks) ===\n");
    const int TASK_COUNT = 200;
    int* args[TASK_COUNT];

    for (int i = 0; i < TASK_COUNT; i++) {
        args[i] = malloc(sizeof(int));
        *args[i] = i;
        uint64_t tid = thread_pool_submit_task(pool, sample_task, args[i], task_complete_cb, NULL);
        assert(tid != 0);
    }
    printf("Submitted %d tasks, waiting for completion...\n", TASK_COUNT);
    test_state_wait_completion();
    printf("All %d tasks completed.\n", TASK_COUNT);
}

// 测试通用通知机制
static void test_notify(ThreadPoolHandle pool) {
    printf("\n=== Test async_poll_notify ===\n");
    int ret;
    ret = async_poll_notify(pool, 1, NULL);
    assert(ret == 0);
    ret = async_poll_notify(pool, 2, (void*)0x1234);
    assert(ret == 0);
    ret = async_poll_notify(pool, 3, NULL);
    assert(ret == 0);
    printf("Sent 3 notifications.\n");
    usleep(100000); // 等待通知被处理（日志可见）
}

// 测试销毁
static void test_destroy(ThreadPoolHandle pool) {
    printf("\n=== Test thread pool destroy ===\n");
    thread_pool_destroy(pool);
    printf("Destroy completed.\n");
}

int main() {
    printf("Starting thread pool tests...\n");

    // 初始化测试状态（初始为0，分配会在每个测试前进行）
    memset(&g_state, 0, sizeof(g_state));

    // 初始化线程池
    ThreadPoolHandle pool = thread_pool_init(2, 4);
    assert(pool != NULL);

    int ret = thread_pool_start(pool);
    assert(ret == 0);
    printf("Thread pool started.\n");

    // 测试1：单个任务
    test_state_init(1);
    test_single_task(pool);

    // 测试2：批量任务（5个）
    test_state_init(5);
    test_batch_tasks(pool);

    // 测试3：大量任务（200个）
    test_state_init(200);
    test_queue_expansion(pool);

    // 测试4：通知
    test_notify(pool);

    // 测试5：销毁
    test_destroy(pool);

    // 释放测试状态资源
    if (g_state.execution_order) free(g_state.execution_order);
    if (g_state.thread_ids) free(g_state.thread_ids);
    pthread_mutex_destroy(&g_state.lock);
    pthread_cond_destroy(&g_state.cond);

    printf("\nAll tests passed successfully!\n");
    return 0;
}