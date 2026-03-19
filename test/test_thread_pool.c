/*
* test_thread_pool.c - 线程池单元测试（利用 TEST_MODE 模拟事件）
*
* 编译命令：
*   gcc -o test_thread_pool os_transport_thread_pool.c test_thread_pool.c -lpthread -I.
*/

#include "os_transport_thread_pool.h"
#include "os_transport_thread_pool_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

/* ---------- 定义 URMA 类型和常量（仅用于编译，测试模式不实际调用） ---------- */
typedef int urma_status_t;
#define URMA_SUCCESS 0
#define URMA_CR_OPC_SEND 1
#define URMA_CR_OPC_WRITE_WITH_IMM 2

/* ---------- 声明测试模式下的模拟队列函数（实现在 .c 中） ---------- */
void mock_event_queue_init(uint32_t cap);
void mock_event_queue_push(uint64_t req_id);
void mock_event_queue_destroy(void);

/* ---------- 测试全局状态 ---------- */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int completed_count;           // 单个任务完成计数
    int batch_completed_count;      // 批次完成计数
    int *exec_order;                // 记录每个任务执行的序号（按完成顺序）
    int exec_index;
    int total_tasks;                // 预期总任务数
} TestState;

static TestState g_state = {0};

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

static void test_state_wait_completion(void) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.completed_count < g_state.total_tasks) {
        pthread_cond_wait(&g_state.cond, &g_state.lock);
    }
    pthread_mutex_unlock(&g_state.lock);
}

static void test_state_wait_batch(int expected_batches) {
    pthread_mutex_lock(&g_state.lock);
    while (g_state.batch_completed_count < expected_batches) {
        pthread_cond_wait(&g_state.cond, &g_state.lock);
    }
    pthread_mutex_unlock(&g_state.lock);
}

/* ---------- 任务函数 ---------- */
static int test_task(void *arg) {
    int seq = *(int *)arg;
    printf("Executing task seq %d in thread %lu\n", seq, (unsigned long)pthread_self());

    pthread_mutex_lock(&g_state.lock);
    g_state.exec_order[g_state.exec_index++] = seq;
    pthread_mutex_unlock(&g_state.lock);

    free(arg); // 释放序号内存
    return 0;
}

/* ---------- 回调函数 ---------- */
static void test_complete_cb(uint64_t task_id, bool success, void *user_data) {
    (void)user_data;
    pthread_mutex_lock(&g_state.lock);
    g_state.completed_count++;
    pthread_cond_signal(&g_state.cond);
    pthread_mutex_unlock(&g_state.lock);
    printf("Task %lu completed, success=%d\n", task_id, success);
}

static void batch_complete_cb(uint64_t task_id, bool success, void *user_data) {
    uint32_t req_id = (uint32_t)(uintptr_t)user_data;
    printf("Batch complete for request_id %u, success=%d\n", req_id, success);

    pthread_mutex_lock(&g_state.lock);
    g_state.batch_completed_count++;
    pthread_cond_signal(&g_state.cond);
    pthread_mutex_unlock(&g_state.lock);
}

/* ---------- 测试用例1：单个任务 ---------- */
static void test_single_tasks(ThreadPoolHandle pool) {
    printf("\n=== Test 1: Single tasks ===\n");
    test_state_init(2);
    uint32_t req1 = 1001, req2 = 1002;
    int *arg1 = malloc(sizeof(int)); *arg1 = 1;
    int *arg2 = malloc(sizeof(int)); *arg2 = 2;

    uint64_t id1 = thread_pool_submit_task(pool, req1, test_task, arg1, test_complete_cb, NULL);
    uint64_t id2 = thread_pool_submit_task(pool, req2, test_task, arg2, test_complete_cb, NULL);
    assert(id1 != 0 && id2 != 0);
    printf("Submitted tasks: %lu, %lu\n", id1, id2);

    // 发送模拟事件
    mock_event_queue_push(req1);
    mock_event_queue_push(req2);

    test_state_wait_completion();
    assert(g_state.exec_order[0] == 1 && g_state.exec_order[1] == 2);
    printf("Test 1 passed.\n");
}

/* ---------- 测试用例2：批量任务，验证顺序和批次回调 ---------- */
static void test_batch_tasks(ThreadPoolHandle pool) {
    printf("\n=== Test 2: Batch tasks ===\n");
    const int BATCH_COUNT = 5;
    uint32_t batch_req = 2001;
    ThreadPoolTask tasks[BATCH_COUNT];
    int *args[BATCH_COUNT];

    test_state_init(BATCH_COUNT);
    for (int i = 0; i < BATCH_COUNT; i++) {
        args[i] = malloc(sizeof(int));
        *args[i] = i + 10; // 序号从10开始
        tasks[i].request_id = batch_req;
        tasks[i].task_func = test_task;
        tasks[i].task_arg = args[i];
        tasks[i].free_task_self = false; // 未使用
    }

    uint64_t *task_ids = thread_pool_submit_batch_tasks(pool, tasks, BATCH_COUNT,
                                                        test_complete_cb, NULL,
                                                        batch_complete_cb, (void*)(uintptr_t)batch_req);
    assert(task_ids != NULL);
    printf("Batch submitted, task IDs: ");
    for (int i = 0; i < BATCH_COUNT; i++) {
        printf("%lu ", task_ids[i]);
    }
    printf("\n");
    free(task_ids);

    // 逐个发送通知，每个通知执行一个任务
    for (int i = 0; i < BATCH_COUNT; i++) {
        mock_event_queue_push(batch_req);
        usleep(20000); // 短暂等待，让asyncPoll处理
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
}

/* ---------- 测试用例3：交错通知 ---------- */
static void test_interleaved_notifications(ThreadPoolHandle pool) {
    printf("\n=== Test 3: Interleaved notifications ===\n");
    const int TASKS_PER_REQ = 3;
    uint32_t req_a = 3001, req_b = 3002;
    ThreadPoolTask tasks_a[TASKS_PER_REQ];
    ThreadPoolTask tasks_b[TASKS_PER_REQ];
    int *seqs_a[TASKS_PER_REQ];
    int *seqs_b[TASKS_PER_REQ];

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

    uint64_t *ids_a = thread_pool_submit_batch_tasks(pool, tasks_a, TASKS_PER_REQ,
                                                     test_complete_cb, NULL,
                                                     batch_complete_cb, (void*)(uintptr_t)req_a);
    uint64_t *ids_b = thread_pool_submit_batch_tasks(pool, tasks_b, TASKS_PER_REQ,
                                                     test_complete_cb, NULL,
                                                     batch_complete_cb, (void*)(uintptr_t)req_b);
    assert(ids_a && ids_b);
    free(ids_a); free(ids_b);

    // 交错发送通知：A, B, A, B, A, B
    mock_event_queue_push(req_a); usleep(20000);
    mock_event_queue_push(req_b); usleep(20000);
    mock_event_queue_push(req_a); usleep(20000);
    mock_event_queue_push(req_b); usleep(20000);
    mock_event_queue_push(req_a); usleep(20000);
    mock_event_queue_push(req_b); usleep(20000);

    test_state_wait_completion();
    test_state_wait_batch(2);

    // 验证每个request_id内部顺序
    int exec_a[TASKS_PER_REQ] = {0}, exec_b[TASKS_PER_REQ] = {0};
    int ca = 0, cb = 0;
    for (int i = 0; i < g_state.exec_index; i++) {
        int v = g_state.exec_order[i];
        if (v >= 100 && v < 200) exec_a[ca++] = v;
        else if (v >= 200 && v < 300) exec_b[cb++] = v;
    }
    assert(ca == TASKS_PER_REQ && cb == TASKS_PER_REQ);
    for (int i = 0; i < TASKS_PER_REQ; i++) {
        assert(exec_a[i] == 100 + i);
        assert(exec_b[i] == 200 + i);
    }
    printf("Test 3 passed.\n");
}

/* ---------- 测试用例4：队列容量（链表无容量限制，但可提交大量任务） ---------- */
static void test_many_tasks(ThreadPoolHandle pool) {
    printf("\n=== Test 4: Many tasks ===\n");
    const int LARGE_COUNT = 100;
    uint32_t large_req = 4001;
    ThreadPoolTask large_tasks[LARGE_COUNT];
    int *large_seqs[LARGE_COUNT];

    test_state_init(LARGE_COUNT);
    for (int i = 0; i < LARGE_COUNT; i++) {
        large_seqs[i] = malloc(sizeof(int));
        *large_seqs[i] = i;
        large_tasks[i].request_id = large_req;
        large_tasks[i].task_func = test_task;
        large_tasks[i].task_arg = large_seqs[i];
    }

    uint64_t *task_ids = thread_pool_submit_batch_tasks(pool, large_tasks, LARGE_COUNT,
                                                        test_complete_cb, NULL,
                                                        batch_complete_cb, (void*)(uintptr_t)large_req);
    assert(task_ids != NULL);
    free(task_ids);

    // 发送所有通知
    for (int i = 0; i < LARGE_COUNT; i++) {
        mock_event_queue_push(large_req);
    }

    test_state_wait_completion();
    test_state_wait_batch(1);
    printf("Test 4 passed (all %d tasks completed).\n", LARGE_COUNT);
}

/* ---------- 测试用例5：取消任务 ---------- */
static void test_cancel_tasks(ThreadPoolHandle pool) {
    printf("\n=== Test 5: Cancel tasks ===\n");
    const int CANCEL_COUNT = 10;
    uint32_t cancel_req = 5001;
    ThreadPoolTask cancel_tasks[CANCEL_COUNT];
    int *cancel_seqs[CANCEL_COUNT];

    test_state_init(CANCEL_COUNT); // 我们将取消一部分，不等待所有完成，所以总任务数设为被取消的数量？实际上我们要提交10个，取消5个，然后通知5个，期待完成5个。
    // 但为了简单，我们提交10个，取消5个，然后发送5个通知，验证完成的只有5个。
    // 重新初始化状态，total_tasks 设为5（期望完成的）
    test_state_init(5);
    for (int i = 0; i < CANCEL_COUNT; i++) {
        cancel_seqs[i] = malloc(sizeof(int));
        *cancel_seqs[i] = i + 50;
        cancel_tasks[i].request_id = cancel_req;
        cancel_tasks[i].task_func = test_task;
        cancel_tasks[i].task_arg = cancel_seqs[i];
    }

    uint64_t *task_ids = thread_pool_submit_batch_tasks(pool, cancel_tasks, CANCEL_COUNT,
                                                        test_complete_cb, NULL,
                                                        batch_complete_cb, (void*)(uintptr_t)cancel_req);
    assert(task_ids != NULL);
    free(task_ids);

    // 取消前5个任务（request_id相同，会取消所有10个？但我们要取消部分，需要区分？目前接口是根据request_id取消所有，所以无法取消部分。
    // 为了测试取消部分，需要不同的request_id。我们可以提交两个不同的request_id，取消其中一个。
    // 重新设计：提交两个批次，取消其中一个。
    printf("Cancelling all tasks with req %u\n", cancel_req);
    int canceled = thread_pool_cancel_tasks_by_req(pool, cancel_req);
    assert(canceled == CANCEL_COUNT); // 应该取消10个

    // 现在没有任何任务可执行了
    // 但为了验证，我们可以再提交一个不同req的任务并执行
    uint32_t other_req = 5002;
    int *other_arg = malloc(sizeof(int)); *other_arg = 99;
    uint64_t other_id = thread_pool_submit_task(pool, other_req, test_task, other_arg, test_complete_cb, NULL);
    assert(other_id != 0);
    mock_event_queue_push(other_req);
    test_state_wait_completion(); // 等待这一个任务完成
    assert(g_state.completed_count == 1);
    assert(g_state.exec_order[0] == 99);
    printf("Test 5 passed.\n");
}

/* ---------- 测试用例6：销毁 ---------- */
static void test_destroy(ThreadPoolHandle pool) {
    printf("\n=== Test 6: Destroy ===\n");
    thread_pool_destroy(pool);
    printf("Thread pool destroyed.\n");
}

/* ---------- 主函数 ---------- */
int main(void) {
    printf("Starting thread pool unit tests (TEST_MODE enabled)...\n");

    // 初始化模拟事件队列
    mock_event_queue_init(64);

    // 初始化线程池（链表不需要容量参数）
    ThreadPoolHandle pool = thread_pool_init(2, 0); // worker_queue_cap 被忽略
    assert(pool != NULL);

    // 设置 URMA 信息（避免空指针，但在测试模式下不会真正使用）
    static urma_jfce_t dummy_jfce;
    static urma_jfc_t dummy_jfc;
    pool->urmaInfo.jfce = &dummy_jfce;
    pool->urmaInfo.jfc = &dummy_jfc;
    pool->urmaInfo.urma_event_mode = false;

    // 启动线程池
    int ret = thread_pool_start(pool);
    assert(ret == 0);
    printf("Thread pool started.\n");

    // 运行测试
    test_single_tasks(pool);
    test_batch_tasks(pool);
    test_interleaved_notifications(pool);
    test_many_tasks(pool);
    test_cancel_tasks(pool);
    test_destroy(pool);

    // 清理模拟队列
    mock_event_queue_destroy();

    // 清理测试状态
    free(g_state.exec_order);
    pthread_mutex_destroy(&g_state.lock);
    pthread_cond_destroy(&g_state.cond);

    printf("\nAll tests passed!\n");
    return 0;
}