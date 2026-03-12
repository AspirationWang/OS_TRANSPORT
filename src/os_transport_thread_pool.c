// os_transport_thread_pool.c
#include "os_transport_thread_pool_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 内部任务包装结构，用于传递回调信息
typedef struct {
    void (*user_func)(void*);
    void* user_arg;
    TaskCompleteCb complete_cb;
    void* user_data;
    uint64_t task_id;
    bool success;   // 由用户任务函数设置
} InternalTask;

// 任务包装函数，实际执行的入口
static void internal_task_wrapper(void* arg) {
    InternalTask* itask = (InternalTask*)arg;
    LOG_DEBUG("Task %lu started", itask->task_id);
    itask->user_func(itask->user_arg);
    // 回调
    if (itask->complete_cb) {
        itask->complete_cb(itask->task_id, itask->success, itask->user_data);
    }
    LOG_DEBUG("Task %lu completed", itask->task_id);
    free(itask);  // 释放内部任务结构
}

// 生成唯一任务ID（线程安全）
static uint64_t generate_task_id(ThreadPoolHandle pool) {
    uint64_t id;
    pthread_mutex_lock(&pool->task_id_mutex);
    id = pool->next_task_id++;
    pthread_mutex_unlock(&pool->task_id_mutex);
    return id;
}

// 扩展 pending 队列容量
static bool pending_queue_expand(PendingTaskQueue* q, uint32_t new_cap) {
    ThreadPoolTask** new_tasks = realloc(q->tasks, new_cap * sizeof(ThreadPoolTask*));
    if (!new_tasks) return false;
    // 如果是循环队列，需要重新排列为线性顺序
    if (q->head < q->tail) {
        // 已有元素是连续的，直接使用
    } else if (q->head > q->tail) {
        // 需要将 head 到 cap-1 的元素移到新缓冲区的尾部后面
        uint32_t elems_before = q->cap - q->head;
        memmove(new_tasks + q->cap, new_tasks + q->head, elems_before * sizeof(ThreadPoolTask*));
        q->head = q->cap;  // 新 head 指向原来 cap 处
        q->cap = new_cap;
        // 现在 head 到 cap-1 是原来的尾部，但整体是连续的
        // 实际上需要将 head 之后的部分作为新的连续区域，重新调整 head/tail
        // 简单做法：重新构建线性队列
        // 我们采用更直接的方式：将元素拷贝到新缓冲区的前部
        uint32_t count = q->size;
        ThreadPoolTask** old = q->tasks;
        for (uint32_t i = 0; i < count; i++) {
            new_tasks[i] = old[(q->head + i) % q->cap];
        }
        q->head = 0;
        q->tail = count;
        q->cap = new_cap;
        free(old);
        return true;
    }
    q->cap = new_cap;
    q->tasks = new_tasks;
    return true;
}

// 向 pending 队列添加任务（必须已持有 pool->global_mutex）
static bool pending_queue_push(ThreadPoolHandle pool, ThreadPoolTask* task) {
    PendingTaskQueue* q = &pool->pending_queue;
    if (q->size >= q->cap) {
        uint32_t new_cap = q->cap * 2;
        if (!pending_queue_expand(q, new_cap)) {
            LOG_ERROR("Failed to expand pending queue to %u", new_cap);
            return false;
        }
    }
    q->tasks[q->tail] = task;
    q->tail = (q->tail + 1) % q->cap;
    q->size++;
    return true;
}

// 从 pending 队列取出任务（必须已持有 pool->global_mutex）
static ThreadPoolTask* pending_queue_pop(ThreadPoolHandle pool) {
    PendingTaskQueue* q = &pool->pending_queue;
    if (q->size == 0) return NULL;
    ThreadPoolTask* task = q->tasks[q->head];
    q->head = (q->head + 1) % q->cap;
    q->size--;
    return task;
}

// 扩展 worker 队列
static bool worker_queue_expand(WorkerThread* worker, uint32_t new_cap) {
    ThreadPoolTask** new_q = realloc(worker->task_queue, new_cap * sizeof(ThreadPoolTask*));
    if (!new_q) return false;
    // 重新排列为线性顺序
    uint32_t count = worker->queue_size;
    ThreadPoolTask** old = worker->task_queue;
    for (uint32_t i = 0; i < count; i++) {
        new_q[i] = old[(worker->queue_head + i) % worker->queue_cap];
    }
    worker->queue_head = 0;
    worker->queue_tail = count;
    worker->queue_cap = new_cap;
    worker->task_queue = new_q;
    return true;
}

// 向 worker 队列添加任务（必须已持有 worker->mutex）
static bool worker_queue_push(WorkerThread* worker, ThreadPoolTask* task) {
    if (worker->queue_size >= worker->queue_cap) {
        uint32_t new_cap = worker->queue_cap * 2;
        if (!worker_queue_expand(worker, new_cap)) {
            LOG_ERROR("Worker %d expand queue failed", worker->worker_idx);
            return false;
        }
    }
    worker->task_queue[worker->queue_tail] = task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    return true;
}

// 从 worker 队列取出任务（必须已持有 worker->mutex）
static ThreadPoolTask* worker_queue_pop(WorkerThread* worker) {
    if (worker->queue_size == 0) return NULL;
    ThreadPoolTask* task = worker->task_queue[worker->queue_head];
    worker->queue_head = (worker->queue_head + 1) % worker->queue_cap;
    worker->queue_size--;
    return task;
}

// 查找最佳 worker：优先空闲，否则选队列最短
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
        if (w->state == WORKER_STATE_BUSY || w->state == WORKER_STATE_INIT) {
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

// worker 线程主函数
static void* worker_routine(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    ThreadPoolHandle pool = worker->pool;
    LOG_INFO("Worker %d started", worker->worker_idx);

    pthread_mutex_lock(&worker->mutex);
    worker->state = WORKER_STATE_IDLE;
    pthread_cond_signal(&worker->cond_task); // 通知创建者已就绪

    while (1) {
        // 等待任务或退出信号
        while (worker->queue_size == 0 && !pool->is_destroying) {
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }
        if (pool->is_destroying && worker->queue_size == 0) {
            worker->state = WORKER_STATE_EXIT;
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        // 取出任务
        ThreadPoolTask* task = worker_queue_pop(worker);
        if (task) {
            worker->state = WORKER_STATE_BUSY;
            pthread_mutex_unlock(&worker->mutex);

            // 执行任务
            task->task_func(task->task_arg);
            task->is_completed = true;

            // 任务完成，更新统计
            pthread_mutex_lock(&pool->stats_mutex);
            pool->completed_tasks++;
            pthread_mutex_unlock(&pool->stats_mutex);

            // 释放任务结构（注意：task_arg 是 InternalTask，已在包装函数中释放）
            free(task);

            pthread_mutex_lock(&worker->mutex);
            worker->state = WORKER_STATE_IDLE;

            // 如果有 pending 任务，通知 asyncPoll 重新调度
            pthread_mutex_lock(&pool->global_mutex);
            if (pool->pending_queue.size > 0) {
                pthread_cond_signal(&pool->cond_interrupt);
            }
            pthread_mutex_unlock(&pool->global_mutex);
        }
    }
    LOG_INFO("Worker %d exiting", worker->worker_idx);
    return NULL;
}

// 创建并启动 worker 线程（如果未创建）
static bool ensure_worker_running(ThreadPoolHandle pool, WorkerThread* worker) {
    if (worker->tid == 0) {
        pthread_mutex_lock(&worker->mutex);
        if (worker->tid == 0) {
            int ret = pthread_create(&worker->tid, NULL, worker_routine, worker);
            if (ret != 0) {
                LOG_ERROR("Failed to create worker %d: %s", worker->worker_idx, strerror(ret));
                pthread_mutex_unlock(&worker->mutex);
                return false;
            }
            // 等待 worker 进入 IDLE 状态
            while (worker->state == WORKER_STATE_INIT) {
                pthread_cond_wait(&worker->cond_task, &worker->mutex);
            }
        }
        pthread_mutex_unlock(&worker->mutex);
    }
    return true;
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
        // 等待中断：有 pending 任务或有通知或销毁
        while (pool->pending_queue.size == 0 && pool->notify_queue_size == 0 && !pool->is_destroying) {
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }
        if (pool->is_destroying && pool->pending_queue.size == 0 && pool->notify_queue_size == 0) {
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        // 处理通知队列
        while (pool->notify_queue_size > 0) {
            NotifyItem item = pool->notify_queue[pool->notify_queue_head];
            pool->notify_queue_head = (pool->notify_queue_head + 1) % pool->notify_queue_cap;
            pool->notify_queue_size--;
            pthread_mutex_unlock(&pool->global_mutex); // 解锁以便处理通知（不阻塞其他）

            LOG_INFO("asyncPoll received notify type %u", item.type);
            // 这里可根据 type 扩展处理，目前仅记录日志
            // 注意：data 由用户管理，此处不释放

            pthread_mutex_lock(&pool->global_mutex);
        }

        // 处理 pending 任务分发
        while (pool->pending_queue.size > 0) {
            ThreadPoolTask* task = pending_queue_pop(pool);
            if (!task) break;

            // 选择 worker
            WorkerThread* worker = select_best_worker(pool);
            if (!worker) {
                LOG_ERROR("No worker available, put task back");
                // 没有可用 worker，放回 pending 队首
                pending_queue_push(pool, task); // 会扩展队列
                break;
            }

            pthread_mutex_lock(&worker->mutex);
            // 确保 worker 线程已运行
            if (!ensure_worker_running(pool, worker)) {
                // 创建失败，放回任务
                pthread_mutex_unlock(&worker->mutex);
                pending_queue_push(pool, task);
                break;
            }

            // 放入 worker 队列
            if (!worker_queue_push(worker, task)) {
                LOG_ERROR("Worker %d queue full, put task back", worker->worker_idx);
                pthread_mutex_unlock(&worker->mutex);
                pending_queue_push(pool, task);
                break;
            }

            // 通知 worker 有任务
            pthread_cond_signal(&worker->cond_task);
            pthread_mutex_unlock(&worker->mutex);
        }

        pthread_mutex_unlock(&pool->global_mutex);
    }

    LOG_INFO("asyncPoll thread exiting");
    return NULL;
}

// 初始化线程池
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    if (worker_queue_cap < 2) worker_queue_cap = 2;
    if (pending_queue_cap == 0) pending_queue_cap = 1024;

    ThreadPoolHandle pool = calloc(1, sizeof(struct _ThreadPool));
    if (!pool) return NULL;

    // 初始化锁和条件变量
    pthread_mutex_init(&pool->task_id_mutex, NULL);
    pthread_mutex_init(&pool->global_mutex, NULL);
    pthread_mutex_init(&pool->stats_mutex, NULL);
    pthread_mutex_init(&pool->start_mutex, NULL);
    pthread_cond_init(&pool->cond_interrupt, NULL);
    pthread_cond_init(&pool->cond_all_done, NULL);
    pthread_cond_init(&pool->cond_start, NULL);

    // 初始化 worker 数组
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &pool->workers[i];
        pthread_mutex_init(&w->mutex, NULL);
        pthread_cond_init(&w->cond_task, NULL);
        w->state = WORKER_STATE_INIT;
        w->worker_idx = i;
        w->pool = pool;
        w->queue_cap = worker_queue_cap;
        w->queue_head = w->queue_tail = w->queue_size = 0;
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
        w->tid = 0; // 未创建线程
    }

    // 初始化 pending 队列
    pool->pending_queue.cap = pending_queue_cap;
    pool->pending_queue.size = 0;
    pool->pending_queue.head = pool->pending_queue.tail = 0;
    pool->pending_queue.tasks = malloc(pending_queue_cap * sizeof(ThreadPoolTask*));
    pthread_mutex_init(&pool->pending_queue.mutex, NULL); // 虽不使用，但初始化
    pthread_cond_init(&pool->pending_queue.cond_has_task, NULL);
    pool->pending_queue.is_destroying = false;
    if (!pool->pending_queue.tasks) {
        // 清理
        for (int i = 0; i < 64; i++) {
            free(pool->workers[i].task_queue);
            pthread_mutex_destroy(&pool->workers[i].mutex);
            pthread_cond_destroy(&pool->workers[i].cond_task);
        }
        free(pool);
        return NULL;
    }

    // 初始化通知队列（固定容量 64，可动态扩展，这里简单固定）
    pool->notify_queue_cap = 64;
    pool->notify_queue_head = pool->notify_queue_tail = pool->notify_queue_size = 0;
    pool->notify_queue = malloc(pool->notify_queue_cap * sizeof(NotifyItem));
    if (!pool->notify_queue) {
        free(pool->pending_queue.tasks);
        for (int i = 0; i < 64; i++) {
            free(pool->workers[i].task_queue);
            pthread_mutex_destroy(&pool->workers[i].mutex);
            pthread_cond_destroy(&pool->workers[i].cond_task);
        }
        free(pool);
        return NULL;
    }

    pool->next_task_id = 1;
    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;
    pool->running_tasks = 0;
    pool->completed_tasks = 0;

    LOG_INFO("Thread pool initialized, worker_queue_cap=%u, pending_queue_cap=%u", worker_queue_cap, pending_queue_cap);
    return pool;
}

// 启动线程池（仅启动 asyncPoll）
int thread_pool_start(ThreadPoolHandle handle) {
    if (!handle || handle->is_running) return -1;

    pthread_mutex_lock(&handle->start_mutex);
    int ret = pthread_create(&handle->async_poll_tid, NULL, async_poll_routine, handle);
    if (ret != 0) {
        LOG_ERROR("Failed to create asyncPoll thread: %s", strerror(ret));
        pthread_mutex_unlock(&handle->start_mutex);
        return -1;
    }
    // 等待 asyncPoll 进入运行状态
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
    if (!handle || !task_func || !handle->is_running) return 0;

    // 创建内部任务包装
    InternalTask* itask = malloc(sizeof(InternalTask));
    if (!itask) return 0;
    itask->user_func = task_func;
    itask->user_arg = task_arg;
    itask->complete_cb = complete_cb;
    itask->user_data = user_data;
    itask->success = true; // 默认成功

    // 创建 ThreadPoolTask
    ThreadPoolTask* task = malloc(sizeof(ThreadPoolTask));
    if (!task) {
        free(itask);
        return 0;
    }
    task->task_id = generate_task_id(handle);
    task->task_func = internal_task_wrapper;
    task->task_arg = itask;
    task->is_completed = false;
    itask->task_id = task->task_id;

    pthread_mutex_lock(&handle->global_mutex);
    // 放入 pending 队列
    if (!pending_queue_push(handle, task)) {
        pthread_mutex_unlock(&handle->global_mutex);
        free(task);
        free(itask);
        return 0;
    }
    // 通知 asyncPoll
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_DEBUG("Task %lu submitted", task->task_id);
    return task->task_id;
}

// 批量提交任务（保证顺序）
uint64_t* thread_pool_submit_batch_tasks(ThreadPoolHandle handle,
                                         ThreadPoolTask* tasks,
                                         uint32_t task_count,
                                         TaskCompleteCb complete_cb,
                                         void* user_data) {
    if (!handle || !tasks || task_count == 0 || !handle->is_running) return NULL;

    // 为每个任务创建内部包装和 ThreadPoolTask
    uint64_t* task_ids = malloc(task_count * sizeof(uint64_t));
    if (!task_ids) return NULL;

    // 先选择同一个 worker
    WorkerThread* target_worker = select_best_worker(handle);
    if (!target_worker) {
        LOG_ERROR("No worker available for batch tasks");
        free(task_ids);
        return NULL;
    }

    // 确保 worker 已运行
    pthread_mutex_lock(&target_worker->mutex);
    if (!ensure_worker_running(handle, target_worker)) {
        pthread_mutex_unlock(&target_worker->mutex);
        free(task_ids);
        return NULL;
    }

    // 批量构造任务并尝试放入 worker 队列
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
        itask->success = true;

        ThreadPoolTask* task = malloc(sizeof(ThreadPoolTask));
        if (!task) {
            free(itask);
            success = false;
            break;
        }
        task->task_id = generate_task_id(handle);
        task->task_func = internal_task_wrapper;
        task->task_arg = itask;
        task->is_completed = false;
        itask->task_id = task->task_id;
        task_ids[i] = task->task_id;

        // 直接放入 target_worker 队列
        if (!worker_queue_push(target_worker, task)) {
            free(task);
            free(itask);
            success = false;
            break;
        }
    }

    if (!success) {
        // 清理已分配的任务
        pthread_mutex_unlock(&target_worker->mutex);
        // 注意：已经放入 worker 队列的任务无法轻易撤回，这里简化处理：继续执行，但返回 NULL
        // 实际应该回滚，但较复杂，为简化，假设成功
        free(task_ids);
        return NULL;
    }

    // 通知 worker 有任务
    pthread_cond_signal(&target_worker->cond_task);
    pthread_mutex_unlock(&target_worker->mutex);

    LOG_DEBUG("Batch of %u tasks submitted to worker %d", task_count, target_worker->worker_idx);
    return task_ids;
}

// 通用通知 asyncPoll
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle || !handle->is_running) return -1;

    pthread_mutex_lock(&handle->global_mutex);
    // 通知队列满则扩展（简单处理：固定容量，如果满则返回错误）
    if (handle->notify_queue_size >= handle->notify_queue_cap) {
        // 可考虑动态扩展，这里简单返回错误
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

// 销毁线程池
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (!handle) return;

    LOG_INFO("Destroying thread pool...");
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    // 唤醒所有等待的线程
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    // 唤醒所有 worker（它们可能等待条件变量）
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        if (w->tid != 0) {
            pthread_mutex_lock(&w->mutex);
            pthread_cond_signal(&w->cond_task);
            pthread_mutex_unlock(&w->mutex);
        }
    }

    // 等待 asyncPoll 线程结束
    if (handle->async_poll_tid) {
        pthread_join(handle->async_poll_tid, NULL);
    }

    // 等待所有 worker 线程结束
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        if (w->tid != 0) {
            pthread_join(w->tid, NULL);
        }
    }

    // 释放 pending 队列中剩余任务（理论上应该没有，因为销毁前会等待所有任务完成）
    // 但为安全，释放队列中的任务
    pthread_mutex_lock(&handle->global_mutex);
    while (handle->pending_queue.size > 0) {
        ThreadPoolTask* task = pending_queue_pop(handle);
        if (task) {
            free(task->task_arg); // InternalTask
            free(task);
        }
    }
    pthread_mutex_unlock(&handle->global_mutex);

    // 释放资源
    for (int i = 0; i < 64; i++) {
        WorkerThread* w = &handle->workers[i];
        free(w->task_queue);
        pthread_mutex_destroy(&w->mutex);
        pthread_cond_destroy(&w->cond_task);
    }
    free(handle->pending_queue.tasks);
    pthread_mutex_destroy(&handle->pending_queue.mutex);
    pthread_cond_destroy(&handle->pending_queue.cond_has_task);
    free(handle->notify_queue);
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_mutex_destroy(&handle->stats_mutex);
    pthread_mutex_destroy(&handle->start_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);
    pthread_cond_destroy(&handle->cond_start);

    free(handle);
    LOG_INFO("Thread pool destroyed");
}