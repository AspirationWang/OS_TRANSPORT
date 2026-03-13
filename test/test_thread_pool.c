#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>

// 模拟外部事件队列，用于 async_poll_routine_wait_poll
typedef struct {
    uint64_t* requests;
    int cap;
    int head;
    int tail;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} MockEventQueue;

static MockEventQueue g_event_queue = {0};

// 初始化模拟事件队列
void mock_event_queue_init(int cap) {
    g_event_queue.requests = malloc(cap * sizeof(uint64_t));
    g_event_queue.cap = cap;
    g_event_queue.head = g_event_queue.tail = g_event_queue.size = 0;
    pthread_mutex_init(&g_event_queue.mutex, NULL);
    pthread_cond_init(&g_event_queue.cond, NULL);
}

// 向事件队列添加一个 request_id（模拟外部事件）
void mock_event_queue_push(uint32_t req_id) {
    pthread_mutex_lock(&g_event_queue.mutex);
    if (g_event_queue.size >= g_event_queue.cap) {
        // 简单扩容
        int new_cap = g_event_queue.cap * 2;
        uint64_t* new_reqs = malloc(new_cap * sizeof(uint64_t));
        for (int i = 0; i < g_event_queue.size; i++) {
            new_reqs[i] = g_event_queue.requests[(g_event_queue.head + i) % g_event_queue.cap];
        }
        free(g_event_queue.requests);
        g_event_queue.requests = new_reqs;
        g_event_queue.cap = new_cap;
        g_event_queue.head = 0;
        g_event_queue.tail = g_event_queue.size;
    }
    g_event_queue.requests[g_event_queue.tail] = req_id;
    g_event_queue.tail = (g_event_queue.tail + 1) % g_event_queue.cap;
    g_event_queue.size++;
    pthread_cond_signal(&g_event_queue.cond);
    pthread_mutex_unlock(&g_event_queue.mutex);
}

// 模拟的 async_poll_routine_wait_poll 函数（由线程池调用）
int async_poll_routine_wait_poll(ThreadPoolHandle pool, uint64_t* request_id) {
    (void)pool; // 未使用
    pthread_mutex_lock(&g_event_queue.mutex);
    while (g_event_queue.size == 0) {
        pthread_cond_wait(&g_event_queue.cond, &g_event_queue.mutex);
    }
    *request_id = g_event_queue.requests[g_event_queue.head];
    g_event_queue.head = (g_event_queue.head + 1) % g_event_queue.cap;
    g_event_queue.size--;
    pthread_mutex_unlock(&g_event_queue.mutex);
    return 0;
}

// 测试全局状态
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int completed_count;           // 单个任务完成计数
    int batch_completed_count;      // 批次完成计数
    int* exec_order;                // 记录每个任务执行的序号（按完成顺序）
    int exec_index;
    int total_tasks;                // 预期总任务数
} TestState;

static TestState g_state;

// 初始化测试状态
static void test_state_init(int total) {
    pthread_mutex_init(&g_state.lock, NULL);
    pthread_cond_init(&g_state.cond, NULL);
    g_state.completed_count = 0;
    g_state.batch_completed_count = 0;
    if (g_state.exec_order) free(g_state.exec_order);
    g_state.exec_order = calloc(total, sizeof(int));
    g_state.exec_index = 0;
    g_state.total_tasks = total;
}

// 等待所有单个任务完成
static void test_state_wait_completion(void) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.completed_count < g_state.total_tasks) {
        pthread_cond_wait(&g_state.cond, &g_state.lock);
    }
    pthread_mutex_unlock(&g_state.lock);
}

// 等待批次完成（根据批次计数）
static void test_state_wait_batch(int expected_batches) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.batch_completed_count < expected_batches) {
        pthread_cond_wait(&g_state.cond, &g_state.lock);
    }
    pthread_mutex_unlock(&g_state.lock);
}

// 任务函数：记录执行顺序并释放参数
static void test_task(void* arg) {
    int seq = *(int*)arg;
    printf("Executing task seq %d in thread %lu\n", seq, (unsigned long)pthread_self());

    pthread_mutex_lock(&g_state.lock);
    g_state.exec_order[g_state.exec_index++] = seq;
    pthread_mutex_unlock(&g_state.lock);

    free(arg); // 释放序号内存
}

// 单个任务完成回调
static void test_complete_cb(uint64_t task_id, bool success, void* user_data) {
    (void)user_data;
    pthread_mutex_lock(&g_state.lock);
    g_state.completed_count++;
    pthread_cond_signal(&g_state.cond);
    pthread_mutex_unlock(&g_state.lock);
    printf("Task %lu completed, success=%d\n", task_id, success);
}

// 批次完成回调
static void batch_complete_cb(uint64_t task_id, bool success, void* user_data) {
    uint32_t req_id = (uint64_t)(uintptr_t)user_data;
    printf("Batch complete for task_id %lu request_id %u, success=%d\n", task_id, req_id, success);

    pthread_mutex_lock(&g_state.lock);
    g_state.batch_completed_count++;
    pthread_cond_signal(&g_state.cond);
    pthread_mutex_unlock(&g_state.lock);
}

int main() {
    printf("Starting thread pool v3 tests...\n");

    // 初始化模拟事件队列
    mock_event_queue_init(64);

    // 初始化线程池，worker队列容量设为2以测试扩容
    ThreadPoolHandle pool = thread_pool_init(2, 0);
    assert(pool != NULL);

    // 启动线程池
    int ret = thread_pool_start(pool);
    assert(ret == 0);
    printf("Thread pool started.\n");

    // 测试1：提交两个不同 request_id 的单个任务（使用批量提交 count=1 来模拟）
    printf("\n=== Test 1: Single tasks with different request_ids ===\n");
    test_state_init(2);
    ThreadPoolTask tasks1[1];
    uint32_t req1 = 1001;
    uint32_t req2 = 1002;

    // 任务1
    int* seq1 = malloc(sizeof(int));
    *seq1 = 1;
    tasks1[0].request_id = req1;
    tasks1[0].task_func = test_task;
    tasks1[0].task_arg = seq1;
    uint64_t* ids1 = thread_pool_submit_batch_tasks(pool, tasks1, 1,
                                                     test_complete_cb, NULL,
                                                     NULL, NULL); // 无批次回调
    assert(ids1 != NULL);
    free(ids1);

    // 任务2
    int* seq2 = malloc(sizeof(int));
    *seq2 = 2;
    tasks1[0].request_id = req2;
    tasks1[0].task_arg = seq2;
    uint64_t* ids2 = thread_pool_submit_batch_tasks(pool, tasks1, 1,
                                                     test_complete_cb, NULL,
                                                     NULL, NULL);
    assert(ids2 != NULL);
    free(ids2);

    // 发送通知
    printf("Sending notify for req %u\n", req1);
    mock_event_queue_push(req1);
    usleep(50000);
    printf("Sending notify for req %lu\n", req2);
    mock_event_queue_push(req2);

    test_state_wait_completion();
    printf("Test 1 passed.\n");

    // 测试2：批量提交5个相同 request_id 的任务，验证顺序和批次回调
    printf("\n=== Test 2: Batch tasks with same request_id ===\n");
    const int BATCH_COUNT = 5;
    uint64_t batch_req = 2001;
    ThreadPoolTask batch_tasks[BATCH_COUNT];
    int* seqs[BATCH_COUNT];

    test_state_init(BATCH_COUNT);
    for (int i = 0; i < BATCH_COUNT; i++) {
        seqs[i] = malloc(sizeof(int));
        *seqs[i] = i + 10; // 序号从10开始
        batch_tasks[i].request_id = batch_req;
        batch_tasks[i].task_func = test_task;
        batch_tasks[i].task_arg = seqs[i];
    }

    uint64_t* batch_ids = thread_pool_submit_batch_tasks(pool, batch_tasks, BATCH_COUNT,
                                                         test_complete_cb, NULL,
                                                         batch_complete_cb, (void*)(uintptr_t)batch_req);
    assert(batch_ids != NULL);
    free(batch_ids);

    // 逐个发送通知，每个通知只执行一个任务
    for (int i = 0; i < BATCH_COUNT; i++) {
        printf("Sending notify %d for req %lu\n", i+1, batch_req);
        mock_event_queue_push(batch_req);
        usleep(30000); // 等待一个任务执行
    }

    test_state_wait_completion();
    test_state_wait_batch(1);

    // 验证执行顺序
    printf("Execution order: ");
    for (int i = 0; i < BATCH_COUNT; i++) {
        printf("%d ", g_state.exec_order[i]);
    }
    printf("\n");
    for (int i = 0; i < BATCH_COUNT; i++) {
        assert(g_state.exec_order[i] == 10 + i);
    }
    printf("Test 2 passed.\n");

    // 测试3：多个不同request_id交错通知
    printf("\n=== Test 3: Interleaved notifications for different request_ids ===\n");
    const int TASKS_PER_REQ = 3;
    uint32_t req_a = 3001;
    uint32_t req_b = 3002;
    ThreadPoolTask tasks_a[TASKS_PER_REQ];
    ThreadPoolTask tasks_b[TASKS_PER_REQ];
    int* seqs_a[TASKS_PER_REQ];
    int* seqs_b[TASKS_PER_REQ];

    test_state_init(TASKS_PER_REQ * 2);
    for (int i = 0; i < TASKS_PER_REQ; i++) {
        seqs_a[i] = malloc(sizeof(int));
        *seqs_a[i] = 100 + i;
        tasks_a[i].request_id = req_a;
        tasks_a[i].task_func = test_task;
        tasks_a[i].task_arg = seqs_a[i];

        seqs_b[i] = malloc(sizeof(int));
        *seqs_b[i] = 200 + i;
        tasks_b[i].request_id = req_b;
        tasks_b[i].task_func = test_task;
        tasks_b[i].task_arg = seqs_b[i];
    }

    uint64_t* ids_a = thread_pool_submit_batch_tasks(pool, tasks_a, TASKS_PER_REQ,
                                                      test_complete_cb, NULL,
                                                      batch_complete_cb, (void*)(uintptr_t)req_a);
    uint64_t* ids_b = thread_pool_submit_batch_tasks(pool, tasks_b, TASKS_PER_REQ,
                                                      test_complete_cb, NULL,
                                                      batch_complete_cb, (void*)(uintptr_t)req_b);
    assert(ids_a && ids_b);
    free(ids_a);
    free(ids_b);

    // 交错发送通知：A, B, A, B, A, B
    mock_event_queue_push(req_a);
    usleep(20000);
    mock_event_queue_push(req_b);
    usleep(20000);
    mock_event_queue_push(req_a);
    usleep(20000);
    mock_event_queue_push(req_b);
    usleep(20000);
    mock_event_queue_push(req_a);
    usleep(20000);
    mock_event_queue_push(req_b);
    usleep(20000);

    test_state_wait_completion();
    test_state_wait_batch(2); // 两个批次

    // 验证每个request_id内部顺序（由于并发，全局顺序可能交错，但每个req内部应有序）
    // 我们可以分别统计两个req的执行顺序
    int exec_a[TASKS_PER_REQ];
    memset(exec_a, 0, sizeof(exec_a));
    int exec_b[TASKS_PER_REQ];
    memset(exec_b, 0, sizeof(exec_b));
    int count_a = 0, count_b = 0;
    for (int i = 0; i < g_state.exec_index; i++) {
        int val = g_state.exec_order[i];
        if (val >= 100 && val < 200) {
            exec_a[count_a++] = val;
        } else if (val >= 200 && val < 300) {
            exec_b[count_b++] = val;
        }
    }
    assert(count_a == TASKS_PER_REQ && count_b == TASKS_PER_REQ);
    for (int i = 0; i < TASKS_PER_REQ; i++) {
        assert(exec_a[i] == 100 + i);
        assert(exec_b[i] == 200 + i);
    }
    printf("Test 3 passed.\n");

    // 测试4：队列扩容（提交大量任务）
    printf("\n=== Test 4: Queue expansion ===\n");
    const int LARGE_COUNT = 100;
    uint64_t large_req = 4001;
    ThreadPoolTask large_tasks[LARGE_COUNT];
    int* large_seqs[LARGE_COUNT];

    test_state_init(LARGE_COUNT);
    for (int i = 0; i < LARGE_COUNT; i++) {
        large_seqs[i] = malloc(sizeof(int));
        *large_seqs[i] = i;
        large_tasks[i].request_id = large_req;
        large_tasks[i].task_func = test_task;
        large_tasks[i].task_arg = large_seqs[i];
    }

    uint64_t* large_ids = thread_pool_submit_batch_tasks(pool, large_tasks, LARGE_COUNT,
                                                          test_complete_cb, NULL,
                                                          batch_complete_cb, (void*)(uintptr_t)large_req);
    assert(large_ids != NULL);
    free(large_ids);

    // 发送所有通知
    for (int i = 0; i < LARGE_COUNT; i++) {
        mock_event_queue_push(large_req);
    }

    test_state_wait_completion();
    test_state_wait_batch(1);
    printf("Test 4 passed (all %d tasks completed).\n", LARGE_COUNT);

    // 测试5：销毁线程池
    printf("\n=== Test 5: Destroy thread pool ===\n");
    thread_pool_destroy(pool);
    printf("Thread pool destroyed.\n");

    // 清理模拟事件队列
    free(g_event_queue.requests);
    pthread_mutex_destroy(&g_event_queue.mutex);
    pthread_cond_destroy(&g_event_queue.cond);

    // 清理测试状态
    free(g_state.exec_order);
    pthread_mutex_destroy(&g_state.lock);
    pthread_cond_destroy(&g_state.cond);

    printf("\nAll tests passed successfully!\n");
    return 0;
}