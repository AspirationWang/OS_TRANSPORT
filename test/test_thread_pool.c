/*
 * test_thread_pool.c - 线程池单元测试
 * 编译命令：
 *   gcc -g -o test_thread_pool os_transport_thread_pool.c test_thread_pool.c -lpthread -I. -D_GNU_SOURCE
 *
 * 注意：需要提供模拟的URMA函数，本文件中已实现模拟。
 */

 #define _GNU_SOURCE
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <unistd.h>
 #include <assert.h>
 #include <pthread.h>
 #include <errno.h>
 
 /* 模拟URMA类型和常量 */
 typedef enum {
     URMA_SUCCESS = 0,
     URMA_CR_OPC_WRITE_WITH_IMM = 1,
     URMA_CR_OPC_SEND = 2
 } urma_status_t;
 
 typedef struct urma_jfce {} urma_jfce_t;
 typedef struct urma_jfc {} urma_jfc_t;
 
 typedef struct urma_cr {
     urma_cr_opcode_t opcode;
     urma_status_t status;
     uint64_t imm_data;
     uint64_t user_ctx;
 } urma_cr_t;
 
 typedef int urma_cr_opcode_t; /* 简化 */
 
 /* 模拟URMA函数声明 */
 int urma_wait_jfc(urma_jfce_t *jfce, int num, int timeout, urma_jfc_t **ev_jfc);
 int urma_poll_jfc(urma_jfc_t *jfc, uint32_t cr_num, urma_cr_t *cr);
 void urma_ack_jfc(urma_jfc_t **jfc, uint32_t *ack_cnt, int num);
 urma_status_t urma_rearm_jfc(urma_jfc_t *jfc, int flag);
 
 /* 模拟事件队列，用于控制urma_poll_jfc返回的事件 */
 typedef struct {
     uint64_t *events;          // 存放 user_ctx 或 imm_data（实际存 request_id）
     uint32_t cap;
     uint32_t head;
     uint32_t tail;
     uint32_t size;
     pthread_mutex_t lock;
     pthread_cond_t cond;
 } MockEventQueue;
 
 static MockEventQueue g_mock_queue = {0};
 
 void mock_event_queue_init(uint32_t cap) {
     g_mock_queue.events = malloc(cap * sizeof(uint64_t));
     g_mock_queue.cap = cap;
     g_mock_queue.head = g_mock_queue.tail = g_mock_queue.size = 0;
     pthread_mutex_init(&g_mock_queue.lock, NULL);
     pthread_cond_init(&g_mock_queue.cond, NULL);
 }
 
 void mock_event_queue_push(uint64_t request_id) {
     pthread_mutex_lock(&g_mock_queue.lock);
     if (g_mock_queue.size >= g_mock_queue.cap) {
         // 扩容
         uint32_t new_cap = g_mock_queue.cap * 2;
         uint64_t *new_events = malloc(new_cap * sizeof(uint64_t));
         for (uint32_t i = 0; i < g_mock_queue.size; i++) {
             new_events[i] = g_mock_queue.events[(g_mock_queue.head + i) % g_mock_queue.cap];
         }
         free(g_mock_queue.events);
         g_mock_queue.events = new_events;
         g_mock_queue.cap = new_cap;
         g_mock_queue.head = 0;
         g_mock_queue.tail = g_mock_queue.size;
     }
     g_mock_queue.events[g_mock_queue.tail] = request_id;
     g_mock_queue.tail = (g_mock_queue.tail + 1) % g_mock_queue.cap;
     g_mock_queue.size++;
     pthread_cond_signal(&g_mock_queue.cond);
     pthread_mutex_unlock(&g_mock_queue.lock);
 }
 
 /* 从队列中取一个事件，返回0表示无事件 */
 static int mock_event_queue_pop(uint64_t *req_id) {
     pthread_mutex_lock(&g_mock_queue.lock);
     if (g_mock_queue.size == 0) {
         pthread_mutex_unlock(&g_mock_queue.lock);
         return 0;
     }
     *req_id = g_mock_queue.events[g_mock_queue.head];
     g_mock_queue.head = (g_mock_queue.head + 1) % g_mock_queue.cap;
     g_mock_queue.size--;
     pthread_mutex_unlock(&g_mock_queue.lock);
     return 1;
 }
 
 void mock_event_queue_destroy(void) {
     free(g_mock_queue.events);
     pthread_mutex_destroy(&g_mock_queue.lock);
     pthread_cond_destroy(&g_mock_queue.cond);
 }
 
 /* 模拟URMA函数实现 */
 int urma_wait_jfc(urma_jfce_t *jfce, int num, int timeout, urma_jfc_t **ev_jfc) {
     (void)jfce; (void)num; (void)timeout;
     // 模拟：如果事件队列非空，返回1并设置ev_jfc为某个非空指针
     pthread_mutex_lock(&g_mock_queue.lock);
     int has_event = (g_mock_queue.size > 0) ? 1 : 0;
     pthread_mutex_unlock(&g_mock_queue.lock);
     if (has_event) {
         *ev_jfc = (urma_jfc_t*)0x1234; // 任意非空指针
         return 1;
     }
     return 0;
 }
 
 int urma_poll_jfc(urma_jfc_t *jfc, uint32_t cr_num, urma_cr_t *cr) {
     (void)jfc;
     // 从事件队列中取出最多 cr_num 个事件
     uint64_t req_id;
     int cnt = 0;
     while (cnt < (int)cr_num && mock_event_queue_pop(&req_id)) {
         cr[cnt].opcode = URMA_CR_OPC_SEND;      // 使用 SEND 类型，user_ctx 携带 request_id
         cr[cnt].status = URMA_SUCCESS;
         cr[cnt].user_ctx = req_id;               // request_id 放入 user_ctx
         cr[cnt].imm_data = 0;
         cnt++;
     }
     return cnt;
 }
 
 void urma_ack_jfc(urma_jfc_t **jfc, uint32_t *ack_cnt, int num) {
     (void)jfc; (void)ack_cnt; (void)num;
     // 模拟空操作
 }
 
 urma_status_t urma_rearm_jfc(urma_jfc_t *jfc, int flag) {
     (void)jfc; (void)flag;
     return URMA_SUCCESS;
 }
 
 /* 包含线程池头文件（必须放在模拟定义之后） */
 #include "os_transport_thread_pool.h"
 #include "os_transport_thread_pool_internal.h"
 
 /* 测试全局状态 */
 typedef struct {
     pthread_mutex_t lock;
     pthread_cond_t cond;
     int completed_count;          // 单个任务完成计数
     int batch_completed_count;     // 批次完成计数
     int *exec_order;               // 记录每个任务执行的序号（按完成顺序）
     int exec_index;
     int total_tasks;               // 预期总任务数
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
 
 /* 任务函数：记录执行顺序并释放参数 */
 static void test_task(void *arg) {
     int seq = *(int *)arg;
     printf("Executing task seq %d in thread %lu\n", seq, (unsigned long)pthread_self());
 
     pthread_mutex_lock(&g_state.lock);
     g_state.exec_order[g_state.exec_index++] = seq;
     pthread_mutex_unlock(&g_state.lock);
 
     free(arg); // 释放序号内存
 }
 
 /* 单个任务完成回调 */
 static void test_complete_cb(uint64_t task_id, bool success, void *user_data) {
     (void)user_data;
     pthread_mutex_lock(&g_state.lock);
     g_state.completed_count++;
     pthread_cond_signal(&g_state.cond);
     pthread_mutex_unlock(&g_state.lock);
     printf("Task %lu completed, success=%d\n", task_id, success);
 }
 
 /* 批次完成回调 */
 static void batch_complete_cb(uint64_t task_id, bool success, void *user_data) {
     uint32_t req_id = (uint32_t)(uintptr_t)user_data;
     printf("Batch complete for request_id %u, success=%d\n", req_id, success);
 
     pthread_mutex_lock(&g_state.lock);
     g_state.batch_completed_count++;
     pthread_cond_signal(&g_state.cond);
     pthread_mutex_unlock(&g_state.lock);
 }
 
 /* 测试1：单个任务提交与执行 */
 static void test_single_tasks(ThreadPoolHandle pool) {
     printf("\n=== Test 1: Single tasks with different request_ids ===\n");
     test_state_init(2);
     uint32_t req1 = 1001, req2 = 1002;
     int *arg1 = malloc(sizeof(int)); *arg1 = 1;
     int *arg2 = malloc(sizeof(int)); *arg2 = 2;
 
     uint64_t id1 = thread_pool_submit_task(pool, req1, test_task, arg1, test_complete_cb, NULL);
     uint64_t id2 = thread_pool_submit_task(pool, req2, test_task, arg2, test_complete_cb, NULL);
     assert(id1 != 0 && id2 != 0);
 
     // 发送通知（模拟urma事件）
     mock_event_queue_push(req1);
     mock_event_queue_push(req2);
 
     test_state_wait_completion();
     assert(g_state.exec_order[0] == 1 && g_state.exec_order[1] == 2);
     printf("Test 1 passed.\n");
 }
 
 /* 测试2：批量任务提交，验证顺序和批次回调 */
 static void test_batch_tasks(ThreadPoolHandle pool) {
     printf("\n=== Test 2: Batch tasks with same request_id ===\n");
     const int BATCH_COUNT = 5;
     uint32_t batch_req = 2001;
     ThreadPoolTask batch_tasks[BATCH_COUNT];
     int *args[BATCH_COUNT];
 
     test_state_init(BATCH_COUNT);
     for (int i = 0; i < BATCH_COUNT; i++) {
         args[i] = malloc(sizeof(int));
         *args[i] = i + 10; // 序号从10开始
         batch_tasks[i].request_id = batch_req;
         batch_tasks[i].task_func = test_task;
         batch_tasks[i].task_arg = args[i];
         batch_tasks[i].free_task_self = false; // 未使用
     }
 
     uint64_t *task_ids = thread_pool_submit_batch_tasks(pool, batch_tasks, BATCH_COUNT,
                                                         test_complete_cb, NULL,
                                                         batch_complete_cb, (void*)(uintptr_t)batch_req);
     assert(task_ids != NULL);
     free(task_ids);
 
     // 逐个发送通知，每个通知只执行一个任务
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
 
 /* 测试3：多个不同request_id交错通知 */
 static void test_interleaved_notifications(ThreadPoolHandle pool) {
     printf("\n=== Test 3: Interleaved notifications for different request_ids ===\n");
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
     test_state_wait_batch(2);
 
     // 验证每个request_id内部顺序
     int exec_a[TASKS_PER_REQ] = {0};
     int exec_b[TASKS_PER_REQ] = {0};
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
 }
 
 /* 测试4：队列扩容（提交大量任务） */
 static void test_queue_expansion(ThreadPoolHandle pool) {
     printf("\n=== Test 4: Queue expansion (many tasks) ===\n");
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
 
 /* 测试5：销毁线程池（确保资源释放） */
 static void test_destroy(ThreadPoolHandle pool) {
     printf("\n=== Test 5: Destroy thread pool ===\n");
     thread_pool_destroy(pool);
     printf("Thread pool destroyed.\n");
 }
 
 int main(void) {
     printf("Starting thread pool unit tests...\n");
 
     // 初始化模拟事件队列
     mock_event_queue_init(64);
 
     // 初始化线程池（worker队列容量设为2以测试扩容）
     ThreadPoolHandle pool = thread_pool_init(2, 0);
     assert(pool != NULL);
 
     // 设置URMA信息（模拟，实际可留空，但代码中会使用，所以需要提供非空指针以通过检查）
     // 注意：async_poll_routine_wait_poll 中会检查 pool->urmaInfo.jfce 和 urma_event_mode
     // 为避免错误，我们设置合理的模拟值
     pool->urmaInfo.jfce = (urma_jfce_t*)0x1234;  // 任意非空指针
     pool->urmaInfo.jfc = (urma_jfc_t*)0x5678;
     pool->urmaInfo.urma_event_mode = false;      // 先使用非事件模式，简化
 
     // 启动线程池
     int ret = thread_pool_start(pool);
     assert(ret == 0);
     printf("Thread pool started.\n");
 
     // 运行测试
     test_single_tasks(pool);
     test_batch_tasks(pool);
     test_interleaved_notifications(pool);
     test_queue_expansion(pool);
     test_destroy(pool);
 
     // 清理模拟事件队列
     mock_event_queue_destroy();
 
     // 清理测试状态
     free(g_state.exec_order);
     pthread_mutex_destroy(&g_state.lock);
     pthread_cond_destroy(&g_state.cond);
 
     printf("\nAll tests passed successfully!\n");
     return 0;
 }