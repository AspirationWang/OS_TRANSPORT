#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include "os_transport_thread_pool_internal.h"

// ===================== 日志宏（简化版） =====================
#ifndef OS_LOG
#define OS_LOG(level, fmt, ...) \
    do { \
        printf("[%s][%s:%d] " fmt "\n", level, __FILE__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#endif

#define LOG_INFO(fmt, ...)  OS_LOG("INFO", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  OS_LOG("WARN", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) OS_LOG("ERROR", fmt, ##__VA_ARGS__)

// ===================== Worker队列操作 =====================
/**
 * @brief Worker队列入队
 */
static int worker_queue_push(WorkerThread* worker, const ThreadPoolTask* task) {
    if (worker == NULL || task == NULL || worker->queue_size >= worker->queue_cap) {
        return -1;
    }

    worker->task_queue[worker->queue_tail] = *task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    return 0;
}

/**
 * @brief Worker队列出队
 */
static int worker_queue_pop(WorkerThread* worker, ThreadPoolTask* task) {
    if (worker == NULL || task == NULL || worker->queue_size == 0) {
        return -1;
    }

    *task = worker->task_queue[worker->queue_head];
    worker->queue_head = (worker->queue_head + 1) % worker->queue_cap;
    worker->queue_size--;
    return 0;
}

// ===================== Pending队列操作（核心缓存） =====================
/**
 * @brief 初始化Pending队列
 */
static int pending_queue_init(PendingTaskQueue* queue, uint32_t init_cap) {
    if (queue == NULL) return -1;

    if (init_cap == 0) init_cap = 1024; // 默认容量
    queue->tasks = (ThreadPoolTask**)calloc(init_cap, sizeof(ThreadPoolTask*));
    if (queue->tasks == NULL) {
        LOG_ERROR("Pending queue malloc failed");
        return -1;
    }

    queue->cap = init_cap;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->is_destroying = false;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_has_task, NULL);
    return 0;
}

/**
 * @brief Pending队列动态扩容（翻倍）
 */
static int pending_queue_resize(PendingTaskQueue* queue) {
    if (queue == NULL) return -1;

    uint32_t new_cap = queue->cap * 2;
    ThreadPoolTask** new_tasks = (ThreadPoolTask**)calloc(new_cap, sizeof(ThreadPoolTask*));
    if (new_tasks == NULL) {
        LOG_ERROR("Pending queue resize failed (new cap=%d)", new_cap);
        return -1;
    }

    // 拷贝原有任务
    for (uint32_t i = 0; i < queue->size; i++) {
        uint32_t idx = (queue->head + i) % queue->cap;
        new_tasks[i] = queue->tasks[idx];
    }

    free(queue->tasks);
    queue->tasks = new_tasks;
    queue->cap = new_cap;
    queue->head = 0;
    queue->tail = queue->size;

    LOG_INFO("Pending queue resize: %d → %d", queue->cap/2, queue->cap);
    return 0;
}

/**
 * @brief Pending队列入队（缓存任务）
 */
static int pending_queue_push(PendingTaskQueue* queue, ThreadPoolTask* task) {
    if (queue == NULL || task == NULL) return -1;

    pthread_mutex_lock(&queue->mutex);

    // 队列满则扩容
    while (queue->size >= queue->cap) {
        pthread_mutex_unlock(&queue->mutex);
        if (pending_queue_resize(queue) != 0) {
            LOG_ERROR("task %lu lost (pending queue resize failed)", task->task_id);
            free(task);
            return -1;
        }
        pthread_mutex_lock(&queue->mutex);
    }

    // 入队
    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;

    // 关键：加锁范围内发送signal，确保信号不丢失
    pthread_cond_signal(&queue->cond_has_task);
    pthread_mutex_unlock(&queue->mutex);

    LOG_INFO("task %lu added to pending queue (size=%d)", task->task_id, queue->size);
    return 0;
}

/**
 * @brief Pending队列出队（无超时，仅靠业务逻辑）
 * @param queue pending队列指针
 * @param pool 线程池指针（感知全局销毁状态）
 * @return 任务指针（NULL=队列销毁/无任务）
 */
static ThreadPoolTask* pending_queue_pop(PendingTaskQueue* queue, struct _ThreadPool* pool) {
    if (queue == NULL || pool == NULL) return NULL;

    pthread_mutex_lock(&queue->mutex);

    // 核心循环：队列空 + 未销毁 → 阻塞等待
    // 循环条件必须同时判断「队列销毁」和「线程池销毁」，避免卡死
    while (queue->size == 0 && !queue->is_destroying && !pool->is_destroying) {
        // 纯业务逻辑等待，无超时
        int ret = pthread_cond_wait(&queue->cond_has_task, &queue->mutex);
        if (ret != 0) {
            LOG_ERROR("pthread_cond_wait failed (err=%d)", ret);
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
    }

    // 场景1：销毁触发 → 直接退出
    if (queue->is_destroying || pool->is_destroying) {
        pthread_mutex_unlock(&queue->mutex);
        LOG_INFO("pending queue pop exit (destroying: queue=%d, pool=%d)",
                 queue->is_destroying, pool->is_destroying);
        return NULL;
    }

    // 场景2：有任务 → 正常出队
    ThreadPoolTask* task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);
    LOG_INFO("task %lu pop from pending queue (remaining size=%d)", task->task_id, queue->size);
    return task;
}

/**
 * @brief 销毁Pending队列
 */
static void pending_queue_destroy(PendingTaskQueue* queue) {
    if (queue == NULL) return;

    // 标记销毁，唤醒所有等待线程
    pthread_mutex_lock(&queue->mutex);
    queue->is_destroying = true;
    pthread_cond_broadcast(&queue->cond_has_task);
    pthread_mutex_unlock(&queue->mutex);

    // 释放所有任务
    pthread_mutex_lock(&queue->mutex);
    for (uint32_t i = 0; i < queue->size; i++) {
        uint32_t idx = (queue->head + i) % queue->cap;
        free(queue->tasks[idx]);
    }
    free(queue->tasks);
    pthread_mutex_unlock(&queue->mutex);

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_has_task);
}

// ===================== 任务分配策略（核心） =====================
/**
 * @brief 查找最优Worker（优先空闲→最少任务）
 */
static int find_best_worker(struct _ThreadPool* pool) {
    int target_idx = -1;
    uint32_t min_task_count = UINT32_MAX;

    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        pthread_mutex_lock(&worker->mutex);

        // 跳过已退出的Worker
        if (worker->state == WORKER_STATE_EXIT || pool->is_destroying) {
            pthread_mutex_unlock(&worker->mutex);
            continue;
        }

        // 优先选择空闲Worker（IDLE状态，队列空）
        if (worker->state == WORKER_STATE_IDLE && worker->queue_size == 0) {
            target_idx = i;
            pthread_mutex_unlock(&worker->mutex);
            return target_idx;
        }

        // 记录任务数最少的Worker
        if (worker->queue_size < min_task_count) {
            min_task_count = worker->queue_size;
            target_idx = i;
        }

        pthread_mutex_unlock(&worker->mutex);
    }

    return target_idx;
}

// ===================== Worker线程逻辑（按需唤醒） =====================
static void* worker_thread_func(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    LOG_INFO("Worker %d initialized (state=INIT)", worker->worker_idx);

    // 初始化为IDLE状态，等待任务
    pthread_mutex_lock(&worker->mutex);
    worker->state = WORKER_STATE_IDLE;
    pthread_mutex_unlock(&worker->mutex);

    ThreadPoolTask task;
    while (1) {
        pthread_mutex_lock(&worker->mutex);

        // 等待任务：队列空 + 未退出
        while (worker->queue_size == 0 && worker->state != WORKER_STATE_EXIT) {
            worker->state = WORKER_STATE_IDLE;
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }

        // 线程池销毁/Worker退出
        if (worker->state == WORKER_STATE_EXIT || worker->pool->is_destroying) {
            worker->state = WORKER_STATE_EXIT;
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        // 标记为BUSY，取出任务
        worker->state = WORKER_STATE_BUSY;
        int ret = worker_queue_pop(worker, &task);
        pthread_mutex_unlock(&worker->mutex);

        if (ret != 0) {
            LOG_WARN("Worker %d pop task failed", worker->worker_idx);
            continue;
        }

        // 执行任务
        LOG_INFO("Worker %d start task %lu", worker->worker_idx, task.task_id);
        bool success = true;
        if (task.task_func != NULL) {
            task.task_func(task.task_arg); // 执行用户任务
        } else {
            success = false;
            LOG_ERROR("Worker %d task %lu has no func", worker->worker_idx, task.task_id);
        }

        // 更新任务状态
        task.is_completed = true;
        pthread_mutex_lock(&worker->pool->stats_mutex);
        worker->pool->running_tasks--;
        worker->pool->completed_tasks++;
        pthread_mutex_unlock(&worker->pool->stats_mutex);

        // 通知外部任务完成
        if (worker->pool->complete_cb != NULL) {
            worker->pool->complete_cb(task.task_id, success, worker->pool->cb_user_data);
        }

        // 销毁任务（参数由用户释放）
        LOG_INFO("Worker %d finish task %lu (success=%d)", worker->worker_idx, task.task_id, success);

        // 标记为IDLE，触发asyncPoll处理pending任务
        pthread_mutex_lock(&worker->mutex);
        worker->state = WORKER_STATE_IDLE;
        pthread_mutex_unlock(&worker->mutex);

        // 通知asyncPoll：有Worker空闲
        pthread_cond_signal(&worker->pool->cond_interrupt);
    }

    LOG_INFO("Worker %d exit (state=EXIT)", worker->worker_idx);
    pthread_exit(NULL);
    return NULL;
}

// ===================== AsyncPoll线程逻辑（调度中枢） =====================
static void* async_poll_thread_func(void* arg) {
    struct _ThreadPool* pool = (struct _ThreadPool*)arg;
    LOG_INFO("AsyncPoll thread initialized");

    // 等待线程池启动
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    if (pool->is_destroying) {
        LOG_INFO("AsyncPoll exit (pool destroying)");
        pthread_exit(NULL);
        return NULL;
    }

    LOG_INFO("AsyncPoll thread started");

    while (!pool->is_destroying) {
        // 第一步：处理Pending队列（优先分发缓存任务）
        while (1) {
            ThreadPoolTask* pending_task = pending_queue_pop(&pool->pending_queue, pool);
            if (pending_task == NULL) break;

            // 查找最优Worker
            int target_idx = find_best_worker(pool);
            if (target_idx < 0) {
                LOG_WARN("No worker available, re-add task %lu to pending", pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }

            // 尝试放入Worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, pending_task);
            pthread_mutex_unlock(&worker->mutex);

            if (ret == 0) {
                // 唤醒Worker执行任务
                pthread_cond_signal(&worker->cond_task);
                LOG_INFO("AsyncPoll assign pending task %lu to Worker %d (queue size=%d)",
                         pending_task->task_id, target_idx, worker->queue_size);
                free(pending_task);
            } else {
                // Worker队列满，重新加入pending
                LOG_WARN("Worker %d queue full, re-add task %lu to pending", target_idx, pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }
        }

        // 第二步：处理外部通知（任务提交/自定义事件）
        pthread_mutex_lock(&pool->global_mutex);
        while (!pool->has_notify && !pool->is_destroying) {
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }

        if (pool->is_destroying) {
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        // 读取通知信息
        uint32_t notify_type = pool->notify_type;
        void* notify_data = pool->notify_data;
        pool->has_notify = false;
        pthread_mutex_unlock(&pool->global_mutex);

        // 处理任务提交通知（类型0）
        if (notify_type == 0) {
            ThreadPoolTask* task = (ThreadPoolTask*)notify_data;
            if (task == NULL) {
                LOG_ERROR("AsyncPoll receive null task");
                continue;
            }

            // 查找最优Worker
            int target_idx = find_best_worker(pool);
            if (target_idx < 0) {
                LOG_WARN("No worker available, add task %lu to pending", task->task_id);
                pending_queue_push(&pool->pending_queue, task);
                continue;
            }

            // 尝试放入Worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, task);
            pthread_mutex_unlock(&worker->mutex);

            if (ret == 0) {
                // 唤醒Worker执行任务
                pthread_cond_signal(&worker->cond_task);
                LOG_INFO("AsyncPoll assign task %lu to Worker %d (queue size=%d)",
                         task->task_id, target_idx, worker->queue_size);
                free(task);

                // 更新运行中任务数
                pthread_mutex_lock(&pool->stats_mutex);
                pool->running_tasks++;
                pthread_mutex_unlock(&pool->stats_mutex);
            } else {
                // Worker队列满，加入pending
                LOG_WARN("Worker %d queue full, add task %lu to pending", target_idx, task->task_id);
                pending_queue_push(&pool->pending_queue, task);
            }
        } else {
            // 处理自定义事件
            LOG_INFO("AsyncPoll receive custom notify (type=%d, data=%p)", notify_type, notify_data);
        }
    }

    LOG_INFO("AsyncPoll thread exit");
    pthread_exit(NULL);
    return NULL;
}

// ===================== 对外接口实现 =====================
/**
 * @brief 初始化线程池
 */
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    if (worker_queue_cap == 0) {
        LOG_ERROR("Worker queue cap cannot be 0");
        return NULL;
    }

    // 分配线程池内存
    struct _ThreadPool* pool = (struct _ThreadPool*)calloc(1, sizeof(struct _ThreadPool));
    if (pool == NULL) {
        LOG_ERROR("ThreadPool malloc failed");
        return NULL;
    }

    // 初始化全局同步原语
    pthread_mutex_init(&pool->global_mutex, NULL);
    pthread_cond_init(&pool->cond_interrupt, NULL);
    pthread_cond_init(&pool->cond_all_done, NULL);

    // 初始化任务ID生成器
    pool->next_task_id = 1;
    pthread_mutex_init(&pool->task_id_mutex, NULL);

    // 初始化任务统计
    pool->running_tasks = 0;
    pool->completed_tasks = 0;
    pthread_mutex_init(&pool->stats_mutex, NULL);

    // 初始化Pending队列
    if (pending_queue_init(&pool->pending_queue, pending_queue_cap) != 0) {
        LOG_ERROR("Pending queue init failed");
        thread_pool_destroy(pool);
        return NULL;
    }

    // 初始化64个Worker线程（仅创建，不启动）
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        worker->queue_cap = worker_queue_cap;
        worker->task_queue = (ThreadPoolTask*)calloc(worker_queue_cap, sizeof(ThreadPoolTask));
        if (worker->task_queue == NULL) {
            LOG_ERROR("Worker %d queue malloc failed", i);
            thread_pool_destroy(pool);
            return NULL;
        }

        worker->state = WORKER_STATE_INIT;
        worker->queue_head = 0;
        worker->queue_tail = 0;
        worker->queue_size = 0;
        worker->worker_idx = i;
        worker->pool = pool;
        pthread_mutex_init(&worker->mutex, NULL);
        pthread_cond_init(&worker->cond_task, NULL);

        // 创建Worker线程（初始化后阻塞，按需唤醒）
        if (pthread_create(&worker->tid, NULL, worker_thread_func, worker) != 0) {
            LOG_ERROR("Create Worker %d failed (err=%d)", i, errno);
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    // 创建AsyncPoll线程（初始化后阻塞）
    if (pthread_create(&pool->async_poll_tid, NULL, async_poll_thread_func, pool) != 0) {
        LOG_ERROR("Create AsyncPoll failed (err=%d)", errno);
        thread_pool_destroy(pool);
        return NULL;
    }

    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;
    pool->has_notify = false;

    LOG_INFO("ThreadPool init success (1 AsyncPoll + 64 Workers)");
    return (ThreadPoolHandle)pool;
}

/**
 * @brief 启动线程池
 */
int thread_pool_start(ThreadPoolHandle handle) {
    if (handle == NULL || !handle->is_initialized || handle->is_running) {
        LOG_ERROR("Invalid ThreadPool state (init=%d, running=%d)",
                 handle->is_initialized, handle->is_running);
        return -1;
    }

    // 标记运行，唤醒AsyncPoll
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = true;
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("ThreadPool start success");
    return 0;
}

/**
 * @brief 外部提交任务
 */
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                    void (*task_func)(void* arg),
                                    void* task_arg,
                                    TaskCompleteCb complete_cb,
                                    void* user_data) {
    if (handle == NULL || !handle->is_initialized || handle->is_destroying || task_func == NULL) {
        LOG_ERROR("Invalid param (handle=%p, init=%d, destroying=%d, func=%p)",
                 handle, handle->is_initialized, handle->is_destroying, task_func);
        return 0;
    }

    // 生成唯一任务ID
    pthread_mutex_lock(&handle->task_id_mutex);
    uint64_t task_id = handle->next_task_id++;
    pthread_mutex_unlock(&handle->task_id_mutex);

    // 构造任务
    ThreadPoolTask* task = (ThreadPoolTask*)malloc(sizeof(ThreadPoolTask));
    if (task == NULL) {
        LOG_ERROR("Task %lu malloc failed", task_id);
        return 0;
    }
    task->task_id = task_id;
    task->task_func = task_func;
    task->task_arg = task_arg;
    task->is_completed = false;

    // 保存回调
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;

    // 触发AsyncPoll中断
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = 0;
    handle->notify_data = task;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("Submit task %lu success", task_id);
    return task_id;
}

/**
 * @brief 通用通知AsyncPoll
 */
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (handle == NULL || !handle->is_initialized || handle->is_destroying) {
        LOG_ERROR("Invalid param (handle=%p, init=%d, destroying=%d)",
                 handle, handle->is_initialized, handle->is_destroying);
        return -1;
    }

    // 触发中断
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = notify_type;
    handle->notify_data = data;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("Notify AsyncPoll (type=%d, data=%p)", notify_type, data);
    return 0;
}

/**
 * @brief 销毁线程池
 */
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (handle == NULL) return;

    LOG_INFO("ThreadPool destroying...");

    // 标记销毁，唤醒所有线程
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    handle->has_notify = true;
    pthread_cond_broadcast(&handle->cond_interrupt); // 唤醒asyncPoll
    pthread_mutex_unlock(&handle->global_mutex);

    // 第二步：强制唤醒pending队列的所有wait线程（核心！）
    pthread_mutex_lock(&handle->pending_queue.mutex);
    handle->pending_queue.is_destroying = true; // 标记队列销毁
    pthread_cond_broadcast(&handle->pending_queue.cond_has_task); // 广播唤醒
    pthread_mutex_unlock(&handle->pending_queue.mutex);

    // 第三步：唤醒所有worker线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_mutex_lock(&worker->mutex);
        worker->state = WORKER_STATE_EXIT;
        pthread_cond_broadcast(&worker->cond_task);
        pthread_mutex_unlock(&worker->mutex);
    }

    // 销毁Pending队列
    pending_queue_destroy(&handle->pending_queue);

    // 等待AsyncPoll退出
    pthread_join(handle->async_poll_tid, NULL);

    // 等待所有Worker退出并清理
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_join(worker->tid, NULL);
        free(worker->task_queue);
        pthread_mutex_destroy(&worker->mutex);
        pthread_cond_destroy(&worker->cond_task);
    }

    // 释放全局资源
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->stats_mutex);
    free(handle);

    LOG_INFO("ThreadPool destroy success");
}