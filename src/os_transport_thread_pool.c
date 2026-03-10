#include "os_transport_thread_pool_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

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
    LOG_INFO("[WZY] Enter into function worker_queue_push");
    worker->task_queue[worker->queue_tail] = *task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    return 0;
}

/**
 * @brief Worker队列出队
 */
static int worker_queue_pop(WorkerThread* worker, ThreadPoolTask* task) {
    LOG_INFO("[WZY] Enter into function worker_queue_pop");
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
    LOG_INFO("[WZY] Enter into function pending_queue_init");
    if (queue == NULL) return -1;

    // 默认初始容量1024
    if (init_cap == 0) {
        init_cap = 1024;
    }

    queue->tasks = (ThreadPoolTask**)calloc(init_cap, sizeof(ThreadPoolTask*));
    if (queue->tasks == NULL) {
        LOG_ERROR("pending queue malloc failed");
        return -1;
    }

    queue->cap = init_cap;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->is_destroying = false;  // 初始化销毁标记为false
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_has_task, NULL);
    return 0;
}

/**
 * @brief Pending队列动态扩容（翻倍）
 */
static int pending_queue_resize(PendingTaskQueue* queue) {
    LOG_INFO("[WZY] Enter into function pending_queue_resize");
    if (queue == NULL) return -1;

    uint32_t new_cap = queue->cap * 2;
    ThreadPoolTask** new_tasks = (ThreadPoolTask**)calloc(new_cap, sizeof(ThreadPoolTask*));
    if (new_tasks == NULL) {
        LOG_ERROR("pending queue resize malloc failed (new cap=%d)", new_cap);
        return -1;
    }

    // 拷贝原有任务
    for (uint32_t i = 0; i < queue->size; i++) {
        uint32_t idx = (queue->head + i) % queue->cap;
        new_tasks[i] = queue->tasks[idx];
    }

    // 替换队列
    free(queue->tasks);
    queue->tasks = new_tasks;
    queue->cap = new_cap;
    queue->head = 0;
    queue->tail = queue->size;

    LOG_INFO("pending queue resize: %d → %d", queue->cap/2, queue->cap);
    return 0;
}

/**
 * @brief Pending队列入队（缓存任务）
 */
static int pending_queue_push(PendingTaskQueue* queue, ThreadPoolTask* task) {
    LOG_INFO("[WZY] Enter into function pending_queue_push");
    if (queue == NULL || task == NULL) return -1;

    pthread_mutex_lock(&queue->mutex);

    // 队列满则扩容
    while (queue->size >= queue->cap) {
        pthread_mutex_unlock(&queue->mutex);
        if (pending_queue_resize(queue) != 0) {
            LOG_ERROR("pending queue resize failed, task %lu lost", task->task_id);
            free(task);
            return -1;
        }
        pthread_mutex_lock(&queue->mutex);
    }

    // 入队
    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;

    // 通知asyncPoll有pending任务
    pthread_cond_signal(&queue->cond_has_task);
    pthread_mutex_unlock(&queue->mutex);

    LOG_INFO("task %lu added to pending queue (pending size=%d)", task->task_id, queue->size);
    return 0;
}

/**
 * @brief Pending队列出队
 */
static ThreadPoolTask* pending_queue_pop(PendingTaskQueue* queue) {
    LOG_INFO("[WZY] Enter into function pending_queue_pop");
    if (queue == NULL) return NULL;

    pthread_mutex_lock(&queue->mutex);
    while (queue->size == 0 && !queue->is_destroying) {
        // 等待条件变量（释放锁，被唤醒后重新加锁）
        LOG_INFO("[WZY] Enter into function pending_queue_pop:queue->size == 0 && !queue->is_destroying");
        int ret = pthread_cond_wait(&queue->cond_has_task, &queue->mutex);
        LOG_INFO("[WZY] Enter into function pending_queue_pop:queue->size == 0 && !queue->is_destroying end!");
        if (ret != 0) {
            LOG_ERROR("pthread_cond_wait failed (err=%d)", ret);
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
    }
    LOG_INFO("[WZY] Enter into function pending_queue_pop: break1");
    // 场景1：队列已销毁，直接返回NULL
    if (queue->is_destroying) {
        pthread_mutex_unlock(&queue->mutex);
        LOG_INFO("pending queue is destroying, pop exit");
        return NULL;
    }

    // 场景2：有任务，正常出队
    LOG_INFO("[WZY] Enter into function pending_queue_pop: break2");
    ThreadPoolTask* task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);
    LOG_INFO("pending queue pop task %lu (remaining size=%d)", task->task_id, queue->size);
    return task;
}

/**
 * @brief 销毁Pending队列
 */
static void pending_queue_destroy(PendingTaskQueue* queue) {
    LOG_INFO("[WZY] Enter into function pending_queue_destroy");
    if (queue == NULL) return;

    // 第一步：标记销毁，唤醒所有等待的线程
    pthread_mutex_lock(&queue->mutex);
    queue->is_destroying = true;
    pthread_cond_broadcast(&queue->cond_has_task);  // 强制唤醒所有wait的线程
    pthread_mutex_unlock(&queue->mutex);

    // 第二步：释放所有pending任务
    pthread_mutex_lock(&queue->mutex);
    for (uint32_t i = 0; i < queue->size; i++) {
        uint32_t idx = (queue->head + i) % queue->cap;
        free(queue->tasks[idx]);
    }
    free(queue->tasks);
    pthread_mutex_unlock(&queue->mutex);

    // 第三步：销毁同步原语
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_has_task);
}

// ===================== 任务分配策略（核心） =====================
/**
 * @brief 查找最优Worker（优先空闲→最少任务）
 */
static int find_best_worker(struct _ThreadPool* pool) {
    LOG_INFO("[WZY] Enter into function find_best_worker");
    int target_idx = -1;
    uint32_t min_task_count = UINT32_MAX;

    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        pthread_mutex_lock(&worker->mutex);

        // 跳过已销毁/未运行的worker
        if (!worker->is_running || pool->is_destroying) {
            pthread_mutex_unlock(&worker->mutex);
            continue;
        }

        // 优先选择空闲worker（队列空）
        if (worker->is_idle && worker->queue_size == 0) {
            target_idx = i;
            pthread_mutex_unlock(&worker->mutex);
            return target_idx;
        }

        // 记录任务数最少的worker
        if (worker->queue_size < min_task_count) {
            min_task_count = worker->queue_size;
            target_idx = i;
        }

        pthread_mutex_unlock(&worker->mutex);
    }

    return target_idx;
}

// ===================== Worker线程逻辑 =====================
static void* worker_thread_func(void* arg) {
    LOG_INFO("[WZY] Enter into function worker_thread_func");
    WorkerThreadArg* worker_arg = (WorkerThreadArg*)arg;
    struct _ThreadPool* pool = worker_arg->pool;
    int worker_idx = worker_arg->worker_idx;
    free(worker_arg); // 释放参数内存

    WorkerThread* worker = &pool->workers[worker_idx];
    LOG_INFO("worker %d initialized, waiting for start", worker_idx);

    // 等待线程池启动（阻塞）
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    // 线程池已销毁，直接退出
    if (pool->is_destroying) {
        LOG_INFO("worker %d exit (pool destroying)", worker_idx);
        pthread_exit(NULL);
        return NULL;
    }

    // 标记worker为运行中+空闲
    pthread_mutex_lock(&worker->mutex);
    worker->is_running = true;
    worker->is_idle = true;
    pthread_mutex_unlock(&worker->mutex);

    LOG_INFO("worker %d started, waiting for task", worker_idx);

    ThreadPoolTask task;
    while (!pool->is_destroying) {
        // 等待任务通知（自动释放锁，被唤醒后重新加锁）
        pthread_mutex_lock(&worker->mutex);
        while (worker->queue_size == 0 && !pool->is_destroying) {
            worker->is_idle = true;
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }

        // 线程池销毁，退出循环
        if (pool->is_destroying) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        // 取出任务，标记为忙碌
        worker->is_idle = false;
        int ret = worker_queue_pop(worker, &task);
        pthread_mutex_unlock(&worker->mutex);

        if (ret != 0) {
            LOG_WARN("worker %d pop task failed", worker_idx);
            continue;
        }

        // 更新运行中任务数
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 执行任务（纯C错误处理，无try/catch）
        LOG_INFO("worker %d start processing task %lu", worker_idx, task.task_id);
        bool success = true;
        if (task.task_func != NULL) {
            task.task_func(task.task_arg); // 执行用户任务
        } else {
            success = false;
            LOG_ERROR("worker %d task %lu has no execute function", worker_idx, task.task_id);
        }

        // 任务完成：更新状态和统计
        task.is_completed = true;
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks--;
        pool->completed_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 通知外部任务完成
        if (pool->complete_cb != NULL) {
            pool->complete_cb(task.task_id, success, pool->cb_user_data);
        }

        // 销毁任务（参数由用户自行释放）
        LOG_INFO("worker %d task %lu completed (success=%d)", worker_idx, task.task_id, success);

        // 标记worker为空闲
        pthread_mutex_lock(&worker->mutex);
        worker->is_idle = true;
        pthread_mutex_unlock(&worker->mutex);

        // 通知asyncPoll：有worker空闲，可处理pending任务
        pthread_cond_signal(&pool->cond_interrupt);
        pthread_cond_signal(&pool->cond_all_done);
    }

    // 退出清理
    pthread_mutex_lock(&worker->mutex);
    worker->is_running = false;
    worker->is_idle = true;
    pthread_mutex_unlock(&worker->mutex);

    LOG_INFO("worker %d exit", worker_idx);
    pthread_exit(NULL);
    return NULL;
}

// ===================== AsyncPoll线程逻辑（调度中枢） =====================
static void* async_poll_thread_func(void* arg) {
    LOG_INFO("[WZY] Enter into function async_poll_thread_func");
    struct _ThreadPool* pool = (struct _ThreadPool*)arg;
    LOG_INFO("asyncPoll thread initialized, waiting for start");

    // 等待线程池启动（阻塞）
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        LOG_INFO("[WZY] pool is not running or destroying");
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    // 线程池已销毁，直接退出
    if (pool->is_destroying) {
        LOG_INFO("asyncPoll exit (pool destroying)");
        pthread_exit(NULL);
        return NULL;
    }

    LOG_INFO("asyncPoll thread started, monitoring interrupt");

    while (!pool->is_destroying) {
        LOG_INFO("[WZY] asyncPoll is working");
        // 第一步：优先处理pending队列（缓存的任务）
        while (1) {
            LOG_INFO("[WZY] asyncPoll is work for pending queue");
            ThreadPoolTask* pending_task = pending_queue_pop(&pool->pending_queue);
            if (pending_task == NULL) {
                LOG_INFO("[WZY] pending_task is null");
                break;
            } else {
                LOG_INFO("[WZY] pending_task is not null");
            }

            // 查找最优worker
            int target_idx = find_best_worker(pool);
            if (target_idx < 0) {
                LOG_WARN("no available worker, re-add task %lu to pending", pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }
            LOG_INFO("[WZY] best worker is %d.", target_idx);

            // 尝试将pending任务放入worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, pending_task);
            pthread_mutex_unlock(&worker->mutex);
            LOG_INFO("[WZY] push pending queue to work queue");
            if (ret == 0) {
                // 放入成功，通知worker执行
                pthread_cond_signal(&worker->cond_task);
                LOG_INFO("asyncPoll assign pending task %lu to worker %d (worker queue size=%d)",
                         pending_task->task_id, target_idx, worker->queue_size);
                free(pending_task); // 释放pending任务内存
            } else {
                // worker队列仍满，重新加入pending
                LOG_WARN("worker %d queue full, re-add task %lu to pending", target_idx, pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }
        }

        // 第二步：处理外部通知（任务提交/自定义事件）
        LOG_INFO("[WZY] working for out things");
        pthread_mutex_lock(&pool->global_mutex);
        while (!pool->has_notify && !pool->is_destroying) {
            LOG_INFO("[WZY] pool->has_notify is false and pool->is_destroying is false");
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }

        // 线程池销毁，退出循环
        if (pool->is_destroying) {
            LOG_INFO("[WZY] working for out things:pool->is_destroying");
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        // 读取通知信息
        uint32_t notify_type = pool->notify_type;
        LOG_INFO("[WZY] working for out things, type = %u", notify_type);
        void* notify_data = pool->notify_data;
        pool->has_notify = false;
        pthread_mutex_unlock(&pool->global_mutex);

        // 处理任务提交通知（类型0）
        if (notify_type == 0) {
            LOG_INFO("[WZY] working for out things, type = 0!");
            ThreadPoolTask* task = (ThreadPoolTask*)notify_data;
            if (task == NULL) {
                LOG_ERROR("asyncPoll receive null task");
                continue;
            }

            // 查找最优worker
            int target_idx = find_best_worker(pool);
            LOG_INFO("[WZY] working for out things, best worker is = %d!", target_idx);
            if (target_idx < 0) {
                LOG_WARN("no available worker, add task %lu to pending", task->task_id);
                pending_queue_push(&pool->pending_queue, task);
                continue;
            }

            // 尝试放入worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            LOG_INFO("[WZY] working for out things, try to push to worker queue");
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, task);
            pthread_mutex_unlock(&worker->mutex);

            if (ret == 0) {
                // 放入成功，通知worker执行
                pthread_cond_signal(&worker->cond_task);
                LOG_INFO("asyncPoll assign task %lu to worker %d (worker queue size=%d)",
                         task->task_id, target_idx, worker->queue_size);
                free(task); // 释放任务内存
            } else {
                // worker队列满，加入pending
                LOG_WARN("worker %d queue full, add task %lu to pending", target_idx, task->task_id);
                pending_queue_push(&pool->pending_queue, task);
            }
        } else {
            // 处理自定义事件通知（类型1+）
            LOG_INFO("asyncPoll receive custom notify (type=%d, data=%p)", notify_type, notify_data);
            // 此处可扩展自定义事件处理逻辑
        }
    }

    // 退出前处理剩余pending任务（通知失败）
    LOG_INFO("asyncPoll exit, processing remaining pending tasks");
    pthread_mutex_lock(&pool->pending_queue.mutex);
    for (uint32_t i = 0; i < pool->pending_queue.size; i++) {
        uint32_t idx = (pool->pending_queue.head + i) % pool->pending_queue.cap;
        ThreadPoolTask* task = pool->pending_queue.tasks[idx];
        if (pool->complete_cb != NULL) {
            pool->complete_cb(task->task_id, false, pool->cb_user_data);
        }
        free(task);
    }
    pthread_mutex_unlock(&pool->pending_queue.mutex);

    LOG_INFO("asyncPoll thread exit");
    pthread_exit(NULL);
    return NULL;
}

// ===================== 对外接口实现 =====================
/**
 * @brief 初始化线程池
 */
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    LOG_INFO("[WZY] Enter into function thread_pool_init");
    // 参数校验
    if (worker_queue_cap == 0) {
        LOG_ERROR("worker queue cap cannot be 0");
        return NULL;
    }

    // 分配线程池内存
    struct _ThreadPool* pool = (struct _ThreadPool*)calloc(1, sizeof(struct _ThreadPool));
    if (pool == NULL) {
        LOG_ERROR("thread pool malloc failed");
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

    // 初始化pending队列
    if (pending_queue_init(&pool->pending_queue, pending_queue_cap) != 0) {
        LOG_ERROR("pending queue init failed");
        thread_pool_destroy(pool);
        return NULL;
    }

    // 初始化64个worker线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        worker->queue_cap = worker_queue_cap;
        worker->task_queue = (ThreadPoolTask*)calloc(worker_queue_cap, sizeof(ThreadPoolTask));
        if (worker->task_queue == NULL) {
            LOG_ERROR("worker %d queue malloc failed", i);
            thread_pool_destroy(pool);
            return NULL;
        }

        // 初始化worker同步原语
        pthread_mutex_init(&worker->mutex, NULL);
        pthread_cond_init(&worker->cond_task, NULL);
        worker->is_idle = true;
        worker->is_running = false;
        worker->queue_head = 0;
        worker->queue_tail = 0;
        worker->queue_size = 0;

        // 创建worker线程参数
        WorkerThreadArg* worker_arg = (WorkerThreadArg*)malloc(sizeof(WorkerThreadArg));
        worker_arg->pool = pool;
        worker_arg->worker_idx = i;

        // 创建worker线程（初始化但不运行）
        if (pthread_create(&worker->tid, NULL, worker_thread_func, worker_arg) != 0) {
            LOG_ERROR("create worker %d failed (err=%d)", i, errno);
            free(worker_arg);
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    // 创建asyncPoll线程（初始化但不运行）
    if (pthread_create(&pool->async_poll_tid, NULL, async_poll_thread_func, pool) != 0) {
        LOG_ERROR("create asyncPoll thread failed (err=%d)", errno);
        thread_pool_destroy(pool);
        return NULL;
    }

    // 标记初始化完成
    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;
    pool->has_notify = false;
    pool->complete_cb = NULL;
    pool->cb_user_data = NULL;

    LOG_INFO("thread pool init success: 1asyncPoll + 64workers (worker cap=%d, pending cap=%d)",
             worker_queue_cap, pool->pending_queue.cap);
    return (ThreadPoolHandle)pool;
}

/**
 * @brief 启动线程池
 */
int thread_pool_start(ThreadPoolHandle handle) {
    LOG_INFO("[WZY] Enter into function thread_pool_start");
    if (handle == NULL || !handle->is_initialized || handle->is_running) {
        LOG_ERROR("invalid thread pool state (initialized=%d, running=%d)",
                 handle->is_initialized, handle->is_running);
        return -1;
    }

    // 标记运行状态，唤醒所有线程
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = true;
    pthread_cond_broadcast(&handle->cond_interrupt); // 唤醒所有阻塞的线程
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("thread pool start success");
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
    // 参数校验
    LOG_INFO("[WZY] Enter into function thread_pool_submit_task");
    if (handle == NULL || !handle->is_initialized || handle->is_destroying || task_func == NULL) {
        LOG_ERROR("invalid param (handle=%p, initialized=%d, destroying=%d, task_func=%p)",
                 handle, handle->is_initialized, handle->is_destroying, task_func);
        return 0;
    }

    // 生成唯一任务ID（互斥锁保护）
    pthread_mutex_lock(&handle->task_id_mutex);
    uint64_t task_id = handle->next_task_id++;
    pthread_mutex_unlock(&handle->task_id_mutex);

    // 构造任务（堆分配，避免栈内存失效）
    ThreadPoolTask* task = (ThreadPoolTask*)malloc(sizeof(ThreadPoolTask));
    if (task == NULL) {
        LOG_ERROR("task %lu malloc failed", task_id);
        return 0;
    }
    task->task_id = task_id;
    task->task_func = task_func;
    task->task_arg = task_arg;
    task->is_completed = false;

    // 保存回调函数
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;

    // 触发asyncPoll中断（通知有任务提交）
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = 0;
    handle->notify_data = task;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt); // 唤醒asyncPoll
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("submit task %lu success", task_id);
    return task_id;
}

/**
 * @brief 通用通知asyncPoll接口
 */
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    LOG_INFO("[WZY] Enter into function async_poll_notify");
    if (handle == NULL || !handle->is_initialized || handle->is_destroying) {
        LOG_ERROR("invalid param (handle=%p, initialized=%d, destroying=%d)",
                 handle, handle->is_initialized, handle->is_destroying);
        return -1;
    }

    // 触发asyncPoll中断
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = notify_type;
    handle->notify_data = data;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    LOG_INFO("notify asyncPoll (type=%d, data=%p)", notify_type, data);
    return 0;
}

/**
 * @brief 销毁线程池
 */
void thread_pool_destroy(ThreadPoolHandle handle) {
    LOG_INFO("[WZY] Enter into function thread_pool_destroy");
    if (handle == NULL) return;

    LOG_INFO("thread pool destroying...");

    // 标记销毁状态，唤醒所有线程
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    handle->has_notify = true;
    pthread_cond_broadcast(&handle->cond_interrupt); // 唤醒asyncPoll
    pthread_cond_broadcast(&handle->pending_queue.cond_has_task); // 唤醒pending队列
    pthread_mutex_unlock(&handle->global_mutex);

    // 等待asyncPoll线程退出
    pthread_join(handle->async_poll_tid, NULL);

    // 等待64个worker线程退出并清理
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_cond_broadcast(&worker->cond_task); // 唤醒阻塞的worker
        pthread_join(worker->tid, NULL);

        // 释放worker队列和同步原语
        free(worker->task_queue);
        pthread_mutex_destroy(&worker->mutex);
        pthread_cond_destroy(&worker->cond_task);
    }

    // 销毁pending队列
    pending_queue_destroy(&handle->pending_queue);

    // 销毁全局同步原语
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);

    // 销毁任务ID和统计锁
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->stats_mutex);

    // 释放线程池内存
    free(handle);
    LOG_INFO("thread pool destroy success");
}