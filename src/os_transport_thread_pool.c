#include "os_transport_thread_pool_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 哈希函数
static uint32_t hash_req_id(uint64_t req_id) {
    return (uint32_t)(req_id ^ (req_id >> 20)) % REQ_HASH_SIZE;
}

// 内部任务包装（增加 request_id）
typedef struct {
    void (*user_func)(void*);
    void* user_arg;
    TaskCompleteCb complete_cb;
    void* user_data;
    uint64_t task_id;
    uint64_t request_id;      // 从外部任务复制
    bool success;
} InternalTask;

// 任务包装函数
static void internal_task_wrapper(void* arg) {
    InternalTask* itask = (InternalTask*)arg;
    LOG_DEBUG("Task %lu (req=%lu) started", itask->task_id, itask->request_id);
    itask->user_func(itask->user_arg);
    if (itask->complete_cb) {
        itask->complete_cb(itask->task_id, itask->success, itask->user_data);
    }
    LOG_DEBUG("Task %lu completed", itask->task_id);
    free(itask);
}

// 生成唯一任务ID
static uint64_t generate_task_id(ThreadPoolHandle pool) {
    uint64_t id;
    pthread_mutex_lock(&pool->task_id_mutex);
    id = pool->next_task_id++;
    pthread_mutex_unlock(&pool->task_id_mutex);
    return id;
}

// 扩展 worker 队列
static bool worker_queue_expand(WorkerThread* worker, uint32_t new_cap) {
    ThreadPoolTask** new_q = malloc(new_cap * sizeof(ThreadPoolTask*));
    if (!new_q) return false;
    uint32_t count = worker->queue_size;
    for (uint32_t i = 0; i < count; i++) {
        new_q[i] = worker->task_queue[(worker->queue_head + i) % worker->queue_cap];
    }
    free(worker->task_queue);
    worker->task_queue = new_q;
    worker->queue_head = 0;
    worker->queue_tail = count;
    worker->queue_cap = new_cap;
    LOG_DEBUG("Worker %d queue expanded to %u", worker->worker_idx, new_cap);
    return true;
}

// 向 worker 队列添加任务（必须已持有 worker->mutex）
static bool worker_queue_push(WorkerThread* worker, ThreadPoolTask* task) {
    if (worker->queue_size >= worker->queue_cap) {
        uint32_t new_cap = worker->queue_cap * 2;
        if (!worker_queue_expand(worker, new_cap)) return false;
    }
    worker->task_queue[worker->queue_tail] = task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    LOG_DEBUG("Worker %d pushed task %lu, queue size now %u", 
              worker->worker_idx, task->task_id, worker->queue_size);
    return true;
}

// 从 worker 队列取出指定 request_id 的第一个任务（必须已持有 worker->mutex）
static ThreadPoolTask* worker_queue_pop_by_req(WorkerThread* worker, uint64_t req_id) {
    if (worker->queue_size == 0) return NULL;
    // 从队头开始搜索第一个匹配的
    uint32_t idx = worker->queue_head;
    for (uint32_t i = 0; i < worker->queue_size; i++) {
        ThreadPoolTask* task = worker->task_queue[(idx + i) % worker->queue_cap];
        if (task->request_id == req_id) {
            // 找到，需要从队列中移除
            // 将后面的元素向前移动
            uint32_t pos = (idx + i) % worker->queue_cap;
            // 如果 pos 不是队尾，需要移动
            // 简单方法：将 pos 之后的元素依次前移
            while (i < worker->queue_size - 1) {
                uint32_t next = (pos + 1) % worker->queue_cap;
                worker->task_queue[pos] = worker->task_queue[next];
                pos = next;
                i++;
            }
            // 此时 pos 是原队尾的前一个？实际上我们移动了 i 次，最后 pos 指向原队尾
            // 修正 tail
            worker->queue_tail = (worker->queue_tail + worker->queue_cap - 1) % worker->queue_cap;
            worker->queue_size--;
            LOG_DEBUG("Worker %d popped task %lu for req %lu", worker->worker_idx, task->task_id, req_id);
            return task;
        }
    }
    return NULL; // 未找到
}

// 查找最佳 worker
static WorkerThread* select_best_worker(ThreadPoolHandle pool) {
    WorkerThread* best = NULL;
    uint32_t min_load = UINT32_MAX;
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &pool->workers[i];
        pthread_mutex_lock(&w->mutex);
        if (w->state == WORKER_STATE_IDLE) {
            best = w;
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        if (w->state == WORKER_STATE_BUSY) {
            uint32_t load = w->queue_size;
            if (load < min_load) {
                min_load = load;
                best = w;
            }
        }
        pthread_mutex_unlock(&w->mutex);
    }
    return best;
}

// 哈希表操作：根据 request_id 查找或创建上下文
static RequestContext* find_req_context(ThreadPoolHandle pool, uint64_t req_id) {
    uint32_t h = hash_req_id(req_id);
    pthread_mutex_lock(&pool->req_hash_mutex);
    RequestContext* ctx = pool->req_hash[h];
    while (ctx) {
        if (ctx->request_id == req_id) break;
        ctx = ctx->next;
    }
    pthread_mutex_unlock(&pool->req_hash_mutex);
    return ctx;
}

static void insert_req_context(ThreadPoolHandle pool, RequestContext* ctx) {
    uint32_t h = hash_req_id(ctx->request_id);
    pthread_mutex_lock(&pool->req_hash_mutex);
    ctx->next = pool->req_hash[h];
    pool->req_hash[h] = ctx;
    pthread_mutex_unlock(&pool->req_hash_mutex);
}

static void remove_req_context(ThreadPoolHandle pool, uint64_t req_id) {
    uint32_t h = hash_req_id(req_id);
    pthread_mutex_lock(&pool->req_hash_mutex);
    RequestContext** p = &pool->req_hash[h];
    while (*p) {
        if ((*p)->request_id == req_id) {
            RequestContext* tmp = *p;
            *p = tmp->next;
            free(tmp);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&pool->req_hash_mutex);
}

// worker 线程主函数
static void* worker_routine(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    ThreadPoolHandle pool = worker->pool;
    LOG_INFO("Worker %d started", worker->worker_idx);

    pthread_mutex_lock(&worker->mutex);
    worker->state = WORKER_STATE_IDLE;
    pthread_cond_signal(&worker->cond_task); // 通知主线程已就绪

    while (1) {
        // 等待通知（pending_req 非0或销毁）
        while (worker->pending_req == 0 && !pool->is_destroying) {
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }
        if (pool->is_destroying && worker->pending_req == 0) {
            worker->state = WORKER_STATE_EXIT;
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        uint64_t req_to_exec = worker->pending_req;
        worker->pending_req = 0; // 清空，准备下一个通知

        // 从队列中取出该 request_id 的第一个任务
        ThreadPoolTask* task = worker_queue_pop_by_req(worker, req_to_exec);
        if (task) {
            worker->state = WORKER_STATE_BUSY;
            pthread_mutex_unlock(&worker->mutex);

            // 执行任务
            task->task_func(task->task_arg);
            task->is_completed = true;

            // 更新统计
            pthread_mutex_lock(&pool->stats_mutex);
            pool->completed_tasks++;
            pthread_mutex_unlock(&pool->stats_mutex);

            // 处理该 request_id 的计数
            RequestContext* ctx = find_req_context(pool, req_to_exec);
            if (ctx) {
                pthread_mutex_lock(&pool->req_hash_mutex); // 保护 ctx 计数
                ctx->pending_count--;
                if (ctx->pending_count == 0) {
                    // 批次完成，调用回调
                    TaskCompleteCb batch_cb = ctx->batch_cb;
                    void* batch_data = ctx->batch_user_data;
                    pthread_mutex_unlock(&pool->req_hash_mutex);
                    if (batch_cb) {
                        batch_cb(0, true, batch_data); // task_id 用0表示批次
                    }
                    remove_req_context(pool, req_to_exec);
                } else {
                    pthread_mutex_unlock(&pool->req_hash_mutex);
                }
            }

            // 释放任务结构
            free(task);

            pthread_mutex_lock(&worker->mutex);
            worker->state = WORKER_STATE_IDLE;
        }
        // 继续循环，等待下一个通知
    }
    LOG_INFO("Worker %d exiting", worker->worker_idx);
    return NULL;
}

// asyncPoll 线程主函数
static void* async_poll_routine(void* arg) {
    ThreadPoolHandle pool = (ThreadPoolHandle)arg;
    LOG_INFO("asyncPoll thread started");

    pthread_mutex_lock(&pool->start_mutex);
    pool->is_started = true;
    pthread_cond_signal(&pool->cond_start);
    pthread_mutex_unlock(&pool->start_mutex);

    while (1) {
        pthread_mutex_lock(&pool->global_mutex);
        // 等待通知队列非空或销毁
        while (pool->notify_queue_size == 0 && !pool->is_destroying) {
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }
        if (pool->is_destroying && pool->notify_queue_size == 0) {
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        // 处理所有通知
        while (pool->notify_queue_size > 0) {
            NotifyItem item = pool->notify_queue[pool->notify_queue_head];
            pool->notify_queue_head = (pool->notify_queue_head + 1) % pool->notify_queue_cap;
            pool->notify_queue_size--;
            pthread_mutex_unlock(&pool->global_mutex);

            if (item.type == 1) { // 任务执行通知
                uint64_t req_id = (uint64_t)(uintptr_t)item.data;
                LOG_DEBUG("asyncPoll received task notify for req %lu", req_id);
                // 查找该 req 绑定的 worker
                RequestContext* ctx = find_req_context(pool, req_id);
                if (ctx) {
                    WorkerThread* worker = &pool->workers[ctx->worker_idx];
                    pthread_mutex_lock(&worker->mutex);
                    worker->pending_req = req_id;
                    pthread_cond_signal(&worker->cond_task);
                    pthread_mutex_unlock(&worker->mutex);
                } else {
                    LOG_WARN("No context found for req %lu", req_id);
                }
            } else {
                // 其他类型通知，可扩展
                LOG_INFO("asyncPoll received generic notify type %u", item.type);
            }

            pthread_mutex_lock(&pool->global_mutex);
        }
        pthread_mutex_unlock(&pool->global_mutex);
    }

    LOG_INFO("asyncPoll thread exiting");
    return NULL;
}

// 初始化线程池
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    (void)pending_queue_cap; // 不再使用 pending 队列
    if (worker_queue_cap < 2) worker_queue_cap = 2;

    ThreadPoolHandle pool = calloc(1, sizeof(struct _ThreadPool));
    if (!pool) return NULL;

    // 初始化锁和条件变量
    pthread_mutex_init(&pool->task_id_mutex, NULL);
    pthread_mutex_init(&pool->global_mutex, NULL);
    pthread_mutex_init(&pool->stats_mutex, NULL);
    pthread_mutex_init(&pool->start_mutex, NULL);
    pthread_mutex_init(&pool->req_hash_mutex, NULL);
    pthread_cond_init(&pool->cond_interrupt, NULL);
    pthread_cond_init(&pool->cond_all_done, NULL);
    pthread_cond_init(&pool->cond_start, NULL);

    // 初始化 worker
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &pool->workers[i];
        pthread_mutex_init(&w->mutex, NULL);
        pthread_cond_init(&w->cond_task, NULL);
        w->state = WORKER_STATE_INIT;
        w->worker_idx = i;
        w->pool = pool;
        w->queue_cap = worker_queue_cap;
        w->queue_head = w->queue_tail = w->queue_size = 0;
        w->pending_req = 0;
        w->task_queue = malloc(worker_queue_cap * sizeof(ThreadPoolTask*));
        if (!w->task_queue) {
            // 清理已分配的资源
            for (int j = 0; j < i; j++) {
                free(pool->workers[j].task_queue);
                pthread_mutex_destroy(&pool->workers[j].mutex);
                pthread_cond_destroy(&pool->workers[j].cond_task);
            }
            free(pool);
            return NULL;
        }
    }

    // 初始化通知队列
    pool->notify_queue_cap = 64;
    pool->notify_queue = malloc(pool->notify_queue_cap * sizeof(NotifyItem));
    if (!pool->notify_queue) {
        // 清理
        return NULL;
    }

    // 哈希表初始为空
    memset(pool->req_hash, 0, sizeof(pool->req_hash));

    pool->next_task_id = 1;
    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;

    // 创建所有 worker 线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &pool->workers[i];
        pthread_mutex_lock(&w->mutex);
        int ret = pthread_create(&w->tid, NULL, worker_routine, w);
        if (ret != 0) {
            LOG_ERROR("Failed to create worker %d: %s", i, strerror(ret));
            pthread_mutex_unlock(&w->mutex);
            // 清理已创建的线程（简化：标记销毁并等待已创建的退出，然后返回NULL）
            // 这里简单处理：调用 thread_pool_destroy 清理，但需避免递归
            // 我们手动清理已创建的线程
            pool->is_destroying = true; // 让已创建的线程退出
            for (int j = 0; j < i; j++) {
                pthread_mutex_lock(&pool->workers[j].mutex);
                pthread_cond_signal(&pool->workers[j].cond_task);
                pthread_mutex_unlock(&pool->workers[j].mutex);
                pthread_join(pool->workers[j].tid, NULL);
            }
            // 释放资源
            free(pool->pending_queue.tasks);
            free(pool->notify_queue);
            for (int j = 0; j < 64; j++) {
                free(pool->workers[j].task_queue);
                pthread_mutex_destroy(&pool->workers[j].mutex);
                pthread_cond_destroy(&pool->workers[j].cond_task);
            }
            pthread_mutex_destroy(&pool->task_id_mutex);
            pthread_mutex_destroy(&pool->global_mutex);
            pthread_mutex_destroy(&pool->stats_mutex);
            pthread_mutex_destroy(&pool->start_mutex);
            pthread_cond_destroy(&pool->cond_interrupt);
            pthread_cond_destroy(&pool->cond_all_done);
            pthread_cond_destroy(&pool->cond_start);
            free(pool);
            return NULL;
        }
        while (w->state == WORKER_STATE_INIT) {
            pthread_cond_wait(&w->cond_task, &w->mutex);
        }
        pthread_mutex_unlock(&w->mutex);
    }

    LOG_INFO("Thread pool initialized");
    return pool;
}

// 启动 asyncPoll
int thread_pool_start(ThreadPoolHandle handle) {
    if (!handle || handle->is_running) return -1;
    pthread_mutex_lock(&handle->start_mutex);
    int ret = pthread_create(&handle->async_poll_tid, NULL, async_poll_routine, handle);
    if (ret != 0) {
        pthread_mutex_unlock(&handle->start_mutex);
        return -1;
    }
    while (!handle->is_started) {
        pthread_cond_wait(&handle->cond_start, &handle->start_mutex);
    }
    pthread_mutex_unlock(&handle->start_mutex);
    handle->is_running = true;
    LOG_INFO("Thread pool started");
    return 0;
}

// 提交单个任务
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                 void (*task_func)(void* arg),
                                 void* task_arg,
                                 TaskCompleteCb complete_cb,
                                 void* user_data) {
    // 此接口未提供 request_id，我们默认 request_id=0 或需要从某处获取？
    // 根据新设计，每个任务应有 request_id，但原接口未包含。为了兼容，我们暂时使用0，
    // 但实际使用时可能需要扩展。这里假设用户通过其他方式传递，或者我们增加一个带request_id的接口。
    // 由于头文件已修改 ThreadPoolTask，但此函数未变，我们只能将 request_id 设为0。
    // 建议增加一个新接口，但为简化，此处用0。
    return thread_pool_submit_task_with_req(handle, 0, task_func, task_arg, complete_cb, user_data);
}

// 实际内部使用的带 request_id 的提交函数
static uint64_t submit_task_with_req(ThreadPoolHandle handle, uint64_t request_id,
                                      void (*task_func)(void*), void* task_arg,
                                      TaskCompleteCb complete_cb, void* user_data) {
    if (!handle || !task_func || !handle->is_running) return 0;

    InternalTask* itask = malloc(sizeof(InternalTask));
    if (!itask) return 0;
    itask->user_func = task_func;
    itask->user_arg = task_arg;
    itask->complete_cb = complete_cb;
    itask->user_data = user_data;
    itask->request_id = request_id;
    itask->success = true;

    ThreadPoolTask* task = malloc(sizeof(ThreadPoolTask));
    if (!task) {
        free(itask);
        return 0;
    }
    task->task_id = generate_task_id(handle);
    task->request_id = request_id;
    task->task_func = internal_task_wrapper;
    task->task_arg = itask;
    task->is_completed = false;
    itask->task_id = task->task_id;

    // 选择 worker
    WorkerThread* worker = select_best_worker(handle);
    if (!worker) {
        LOG_ERROR("No worker available for task %lu", task->task_id);
        free(task);
        free(itask);
        return 0;
    }

    pthread_mutex_lock(&worker->mutex);
    if (!worker_queue_push(worker, task)) {
        pthread_mutex_unlock(&worker->mutex);
        free(task);
        free(itask);
        return 0;
    }
    pthread_mutex_unlock(&worker->mutex);

    // 更新 request_id 上下文
    RequestContext* ctx = find_req_context(handle, request_id);
    if (ctx) {
        pthread_mutex_lock(&handle->req_hash_mutex);
        ctx->pending_count++;
        pthread_mutex_unlock(&handle->req_hash_mutex);
    } else {
        ctx = malloc(sizeof(RequestContext));
        ctx->request_id = request_id;
        ctx->worker_idx = worker->worker_idx;
        ctx->pending_count = 1;
        ctx->batch_cb = NULL;  // 单个任务无批次回调
        ctx->batch_user_data = NULL;
        insert_req_context(handle, ctx);
    }

    LOG_DEBUG("Task %lu (req=%lu) submitted to worker %d", task->task_id, request_id, worker->worker_idx);
    return task->task_id;
}

// 为兼容原接口，我们保留原函数名，但内部调用 submit_task_with_req(0)
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                 void (*task_func)(void* arg),
                                 void* task_arg,
                                 TaskCompleteCb complete_cb,
                                 void* user_data) {
    return submit_task_with_req(handle, 0, task_func, task_arg, complete_cb, user_data);
}

// 新增带 request_id 的单任务提交（可选）
uint64_t thread_pool_submit_task_with_req(ThreadPoolHandle handle,
                                          uint64_t request_id,
                                          void (*task_func)(void* arg),
                                          void* task_arg,
                                          TaskCompleteCb complete_cb,
                                          void* user_data) {
    return submit_task_with_req(handle, request_id, task_func, task_arg, complete_cb, user_data);
}

// 批量提交
uint64_t* thread_pool_submit_batch_tasks(ThreadPoolHandle handle,
                                         ThreadPoolTask* tasks,
                                         uint32_t task_count,
                                         TaskCompleteCb complete_cb,
                                         void* user_data,
                                         TaskCompleteCb batch_complete_cb,
                                         void* batch_user_data) {
    if (!handle || !tasks || task_count == 0 || !handle->is_running) return NULL;
    // 检查所有任务的 request_id 必须相同
    uint64_t req_id = tasks[0].request_id;
    for (uint32_t i = 1; i < task_count; i++) {
        if (tasks[i].request_id != req_id) {
            LOG_ERROR("Batch tasks have inconsistent request_id");
            return NULL;
        }
    }

    uint64_t* task_ids = malloc(task_count * sizeof(uint64_t));
    if (!task_ids) return NULL;

    // 选择同一个 worker
    WorkerThread* target_worker = select_best_worker(handle);
    if (!target_worker) {
        free(task_ids);
        return NULL;
    }
    int worker_idx = target_worker->worker_idx;

    pthread_mutex_lock(&target_worker->mutex);

    bool success = true;
    for (uint32_t i = 0; i < task_count; i++) {
        InternalTask* itask = malloc(sizeof(InternalTask));
        if (!itask) {
            success = false;
            break;
        }
        itask->user_func = tasks[i].task_func;
        itask->user_arg = tasks[i].task_arg;
        itask->complete_cb = complete_cb;
        itask->user_data = user_data;
        itask->request_id = req_id;
        itask->success = true;

        ThreadPoolTask* task = malloc(sizeof(ThreadPoolTask));
        if (!task) {
            free(itask);
            success = false;
            break;
        }
        task->task_id = generate_task_id(handle);
        task->request_id = req_id;
        task->task_func = internal_task_wrapper;
        task->task_arg = itask;
        task->is_completed = false;
        itask->task_id = task->task_id;
        task_ids[i] = task->task_id;

        if (!worker_queue_push(target_worker, task)) {
            free(task);
            free(itask);
            success = false;
            break;
        }
    }

    if (!success) {
        pthread_mutex_unlock(&target_worker->mutex);
        free(task_ids);
        return NULL;
    }

    pthread_mutex_unlock(&target_worker->mutex);

    // 更新 request_id 上下文
    RequestContext* ctx = find_req_context(handle, req_id);
    if (ctx) {
        pthread_mutex_lock(&handle->req_hash_mutex);
        ctx->pending_count += task_count;
        pthread_mutex_unlock(&handle->req_hash_mutex);
    } else {
        ctx = malloc(sizeof(RequestContext));
        ctx->request_id = req_id;
        ctx->worker_idx = worker_idx;
        ctx->pending_count = task_count;
        ctx->batch_cb = batch_complete_cb;
        ctx->batch_user_data = batch_user_data;
        insert_req_context(handle, ctx);
    }

    LOG_DEBUG("Batch of %u tasks (req=%lu) submitted to worker %d", task_count, req_id, worker_idx);
    return task_ids;
}

// 通用通知接口（保留原样，但内部使用）
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle || !handle->is_running) return -1;
    pthread_mutex_lock(&handle->global_mutex);
    if (handle->notify_queue_size >= handle->notify_queue_cap) {
        pthread_mutex_unlock(&handle->global_mutex);
        LOG_WARN("Notify queue full, type %u dropped", notify_type);
        return -1;
    }
    handle->notify_queue[handle->notify_queue_tail].type = notify_type;
    handle->notify_queue[handle->notify_queue_tail].data = data;
    handle->notify_queue_tail = (handle->notify_queue_tail + 1) % handle->notify_queue_cap;
    handle->notify_queue_size++;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);
    LOG_DEBUG("Notify type %u sent", notify_type);
    return 0;
}

// 专用任务通知接口
int async_poll_notify_task(ThreadPoolHandle handle, uint64_t request_id) {
    return async_poll_notify(handle, 1, (void*)(uintptr_t)request_id);
}

// 销毁线程池
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (!handle) return;
    LOG_INFO("Destroying thread pool...");
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    // 唤醒所有 worker
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        pthread_mutex_lock(&w->mutex);
        pthread_cond_signal(&w->cond_task);
        pthread_mutex_unlock(&w->mutex);
    }

    if (handle->async_poll_tid) pthread_join(handle->async_poll_tid, NULL);
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        if (w->tid) pthread_join(w->tid, NULL);
    }

    // 释放队列中剩余任务
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        pthread_mutex_lock(&w->mutex);
        while (w->queue_size > 0) {
            ThreadPoolTask* task = worker_queue_pop_by_req(w, UINT64_MAX); // 取任意
            if (task) {
                free(task->task_arg);
                free(task);
            }
        }
        pthread_mutex_unlock(&w->mutex);
        free(w->task_queue);
        pthread_mutex_destroy(&w->mutex);
        pthread_cond_destroy(&w->cond_task);
    }

    // 释放哈希表中的剩余上下文
    for (int i = 0; i < REQ_HASH_SIZE; i++) {
        RequestContext* ctx = handle->req_hash[i];
        while (ctx) {
            RequestContext* next = ctx->next;
            free(ctx);
            ctx = next;
        }
    }

    free(handle->notify_queue);
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_mutex_destroy(&handle->stats_mutex);
    pthread_mutex_destroy(&handle->start_mutex);
    pthread_mutex_destroy(&handle->req_hash_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);
    pthread_cond_destroy(&handle->cond_start);
    free(handle);
    LOG_INFO("Thread pool destroyed");
}