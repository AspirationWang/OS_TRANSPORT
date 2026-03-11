#include "os_transport_thread_pool_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// 内部辅助函数声明
static void* async_poll_thread(void* arg);
static void* worker_thread(void* arg);
static int worker_queue_push(WorkerThread* worker, ThreadPoolTask* task);
static ThreadPoolTask* worker_queue_pop(WorkerThread* worker);
static int pending_queue_push(PendingTaskQueue* queue, ThreadPoolTask* task);
static ThreadPoolTask* pending_queue_pop(PendingTaskQueue* queue);
static int notify_queue_push(ThreadPoolHandle handle, uint32_t notify_type, void* data);
static NotifyItem* notify_queue_pop(ThreadPoolHandle handle);
static int select_best_worker(ThreadPoolHandle handle);
static void worker_process_pending(ThreadPoolHandle handle, WorkerThread* worker);
static void task_complete(ThreadPoolHandle handle, uint64_t task_id, bool success);

// ====================== 队列操作辅助函数 ======================
/**
 * @brief Worker线程任务队列入队
 */
static int worker_queue_push(WorkerThread* worker, ThreadPoolTask* task) {
    LOG_DEBUG("[WZY] Enter into function worker_queue_push");
    if (!worker || !task) {
        LOG_ERROR("worker_queue_push invalid param");
        return -1;
    }

    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start1;", worker->worker_idx);
    pthread_mutex_lock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End1;", worker->worker_idx);
    if (worker->queue_size >= worker->queue_cap) {
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start1;", worker->worker_idx);
        pthread_mutex_unlock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end1;", worker->worker_idx);
        LOG_WARN("worker[%d] queue full (cap:%u, size:%u)", 
                worker->worker_idx, worker->queue_cap, worker->queue_size);
        return -1;
    }

    worker->task_queue[worker->queue_tail] = task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    LOG_DEBUG("worker[%d] push task[%lu], queue size:%u", 
            worker->worker_idx, task->task_id, worker->queue_size);
    
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start2;", worker->worker_idx);
            pthread_mutex_unlock(&worker->mutex);
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end2;", worker->worker_idx);
    return 0;
}

/**
 * @brief Worker线程任务队列出队
 */
static ThreadPoolTask* worker_queue_pop(WorkerThread* worker) {
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:1");
    if (!worker) {
        LOG_DEBUG("[WZY] Enter into function worker_queue_pop:2");
        LOG_ERROR("worker_queue_pop invalid param");
        return NULL;
    }
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:3");
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start2;", worker->worker_idx);
    pthread_mutex_lock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End2;", worker->worker_idx);
    if (worker->queue_size == 0) {
        LOG_DEBUG("[WZY] Enter into function worker_queue_pop:4");
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start3;", worker->worker_idx);
        pthread_mutex_unlock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end3;", worker->worker_idx);
        return NULL;
    }
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:5");

    ThreadPoolTask* task = worker->task_queue[worker->queue_head];
    worker->queue_head = (worker->queue_head + 1) % worker->queue_cap;
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:6");
    worker->queue_size--;
    LOG_DEBUG("worker[%d] pop task[%lu], queue size:%u", 
            worker->worker_idx, task->task_id, worker->queue_size);
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:7");
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start4;", worker->worker_idx);
    pthread_mutex_unlock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end4;", worker->worker_idx);
    LOG_DEBUG("[WZY] Enter into function worker_queue_pop:8");
    return task;
}

/**
 * @brief 全局Pending任务队列入队
 */
static int pending_queue_push(PendingTaskQueue* queue, ThreadPoolTask* task) {
    if (!queue || !task) {
        LOG_ERROR("pending_queue_push invalid param");
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);
    if (queue->is_destroying) {
        pthread_mutex_unlock(&queue->mutex);
        LOG_WARN("pending queue is destroying, reject task[%lu]", task->task_id);
        return -1;
    }

    // 队列满时扩容（2倍）
    if (queue->size >= queue->cap) {
        uint32_t new_cap = queue->cap * 2;
        ThreadPoolTask** new_tasks = realloc(queue->tasks, new_cap * sizeof(ThreadPoolTask*));
        if (!new_tasks) {
            pthread_mutex_unlock(&queue->mutex);
            LOG_ERROR("pending queue expand failed (cap:%u -> %u), errno:%d", 
                    queue->cap, new_cap, errno);
            return -1;
        }

        // 拷贝原有数据
        for (uint32_t i = 0; i < queue->size; i++) {
            new_tasks[i] = queue->tasks[(queue->head + i) % queue->cap];
        }
        free(queue->tasks);
        queue->tasks = new_tasks;
        queue->head = 0;
        queue->tail = queue->size;
        queue->cap = new_cap;
        LOG_INFO("pending queue expand success (cap:%u -> %u)", queue->cap/2, new_cap);
    }

    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;
    LOG_DEBUG("pending queue push task[%lu], size:%u/cap:%u", 
            task->task_id, queue->size, queue->cap);
    
    pthread_cond_signal(&queue->cond_has_task);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

/**
 * @brief 全局Pending任务队列出队
 */
static ThreadPoolTask* pending_queue_pop(PendingTaskQueue* queue) {
    if (!queue) {
        LOG_ERROR("pending_queue_pop invalid param");
        return NULL;
    }

    pthread_mutex_lock(&queue->mutex);
    while (queue->size == 0 && !queue->is_destroying) {
        pthread_cond_wait(&queue->cond_has_task, &queue->mutex);
    }

    if (queue->is_destroying || queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    ThreadPoolTask* task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;
    LOG_DEBUG("pending queue pop task[%lu], size:%u", 
            task->task_id, queue->size);
    
    pthread_mutex_unlock(&queue->mutex);
    return task;
}

/**
 * @brief 通知队列入队
 */
static int notify_queue_push(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle) {
        LOG_ERROR("notify_queue_push invalid param");
        return -1;
    }

    pthread_mutex_lock(&handle->global_mutex);
    if (handle->notify_queue_size >= handle->notify_queue_cap) {
        // 扩容通知队列（2倍）
        uint32_t new_cap = handle->notify_queue_cap * 2;
        NotifyItem* new_queue = realloc(handle->notify_queue, new_cap * sizeof(NotifyItem));
        if (!new_queue) {
            pthread_mutex_unlock(&handle->global_mutex);
            LOG_ERROR("notify queue expand failed (cap:%u -> %u), errno:%d", 
                    handle->notify_queue_cap, new_cap, errno);
            return -1;
        }
        handle->notify_queue = new_queue;
        handle->notify_queue_cap = new_cap;
        LOG_INFO("notify queue expand success (cap:%u -> %u)", new_cap/2, new_cap);
    }

    handle->notify_queue[handle->notify_queue_tail].type = notify_type;
    handle->notify_queue[handle->notify_queue_tail].data = data;
    handle->notify_queue_tail = (handle->notify_queue_tail + 1) % handle->notify_queue_cap;
    handle->notify_queue_size++;
    handle->has_notify = true;
    LOG_DEBUG("notify queue push type:%u, size:%u", notify_type, handle->notify_queue_size);

    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);
    return 0;
}

/**
 * @brief 通知队列出队
 */
static NotifyItem* notify_queue_pop(ThreadPoolHandle handle) {
    if (!handle) {
        LOG_ERROR("notify_queue_pop invalid param");
        return NULL;
    }

    pthread_mutex_lock(&handle->global_mutex);
    if (handle->notify_queue_size == 0) {
        handle->has_notify = false;
        pthread_mutex_unlock(&handle->global_mutex);
        return NULL;
    }

    NotifyItem* item = &handle->notify_queue[handle->notify_queue_head];
    handle->notify_queue_head = (handle->notify_queue_head + 1) % handle->notify_queue_cap;
    handle->notify_queue_size--;
    if (handle->notify_queue_size == 0) {
        handle->has_notify = false;
    }
    LOG_DEBUG("notify queue pop type:%u, size:%u", item->type, handle->notify_queue_size);

    pthread_mutex_unlock(&handle->global_mutex);
    return item;
}

// ====================== 线程选择辅助函数 ======================
/**
 * @brief 选择最优Worker线程
 * @param handle 线程池句柄
 * @return 最优worker索引（-1=失败）
 */
static int select_best_worker(ThreadPoolHandle handle) {
    if (!handle) {
        LOG_ERROR("select_best_worker invalid param");
        return -1;
    }

    int best_idx = -1;
    uint32_t min_queue_size = UINT32_MAX;
    bool found_idle = false;

    pthread_mutex_lock(&handle->global_mutex);
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        if (worker->state == WORKER_STATE_EXIT) {
            continue;
        }

        // 优先选择空闲线程
        if (worker->state == WORKER_STATE_IDLE) {
            best_idx = i;
            found_idle = true;
            break; // 空闲线程优先，直接选中
        }

        // 非批量任务：选队列最小的；批量任务：需遍历所有找最小
        if (!found_idle) {
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start3;", worker->worker_idx);
        pthread_mutex_lock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End3;", worker->worker_idx);
            uint32_t queue_size = worker->queue_size;
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start5;", worker->worker_idx);
            pthread_mutex_unlock(&worker->mutex);
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end5;", worker->worker_idx);
            
            if (queue_size < min_queue_size) {
                min_queue_size = queue_size;
                best_idx = i;
            }
        }
    }
    pthread_mutex_unlock(&handle->global_mutex);

    if (best_idx == -1) {
        LOG_ERROR("no available worker found");
        return -1;
    }

    LOG_DEBUG("select best worker[%d] (idle:%s, queue_size:%u)", 
            best_idx, found_idle ? "yes" : "no", min_queue_size);
    return best_idx;
}

// ====================== 任务完成处理 ======================
static void task_complete(ThreadPoolHandle handle, uint64_t task_id, bool success) {
    if (!handle) {
        LOG_ERROR("task_complete invalid param");
        return;
    }

    // 更新统计
    pthread_mutex_lock(&handle->stats_mutex);
    handle->running_tasks--;
    handle->completed_tasks++;
    LOG_DEBUG("task[%lu] completed, running:%u, completed:%u", 
            task_id, handle->running_tasks, handle->completed_tasks);
    
    // 触发所有任务完成条件
    if (handle->running_tasks == 0) {
        pthread_cond_signal(&handle->cond_all_done);
    }
    pthread_mutex_unlock(&handle->stats_mutex);

    // 执行回调
    if (handle->complete_cb) {
        LOG_DEBUG("call complete cb for task[%lu] (success:%s)", 
                task_id, success ? "yes" : "no");
        handle->complete_cb(task_id, success, handle->cb_user_data);
    }
}

// ====================== Worker线程处理Pending任务 ======================
static void worker_process_pending(ThreadPoolHandle handle, WorkerThread* worker) {
    if (!handle || !worker) {
        return;
    }

    // 仅当worker队列有空闲时拉取pending任务
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start4;", worker->worker_idx);
    pthread_mutex_lock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End4;", worker->worker_idx);
    bool has_space = (worker->queue_size < worker->queue_cap);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start6;", worker->worker_idx);
    pthread_mutex_unlock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end6;", worker->worker_idx);

    if (!has_space) {
        return;
    }

    ThreadPoolTask* task = pending_queue_pop(&handle->pending_queue);
    if (task) {
        if (worker_queue_push(worker, task) == 0) {
            // 唤醒worker处理新任务
            pthread_cond_signal(&worker->cond_task);
            LOG_DEBUG("worker[%d] pull task[%lu] from pending queue", 
                    worker->worker_idx, task->task_id);
        } else {
            // 推回pending队列（异常）
            pending_queue_push(&handle->pending_queue, task);
            LOG_WARN("worker[%d] push pending task[%lu] failed, push back to pending", 
                    worker->worker_idx, task->task_id);
        }
    }
}

// ====================== 线程实现 ======================
/**
 * @brief asyncPoll线程主函数（处理任务分配、通知、pending队列消费）
 */
static void* async_poll_thread(void* arg) {
    ThreadPoolHandle handle = (ThreadPoolHandle)arg;
    if (!handle) {
        LOG_ERROR("async_poll_thread invalid param");
        pthread_exit(NULL);
    }

    LOG_INFO("asyncPoll thread start (tid:%lu)", (unsigned long)pthread_self());

    // 等待线程池启动信号
    pthread_mutex_lock(&handle->start_mutex);
    while (!handle->is_started && !handle->is_destroying) {
        pthread_cond_wait(&handle->cond_start, &handle->start_mutex);
    }
    pthread_mutex_unlock(&handle->start_mutex);

    if (handle->is_destroying) {
        LOG_WARN("asyncPoll thread exit: pool is destroying");
        pthread_exit(NULL);
    }

    // 主循环：处理中断/通知
    while (!handle->is_destroying) {
        pthread_mutex_lock(&handle->global_mutex);
        // 等待中断信号（任务提交/通用通知）
        while (!handle->has_notify && !handle->is_destroying) {
            pthread_cond_wait(&handle->cond_interrupt, &handle->global_mutex);
        }
        pthread_mutex_unlock(&handle->global_mutex);

        if (handle->is_destroying) {
            break;
        }

        // 处理通知队列
        NotifyItem* notify = notify_queue_pop(handle);
        while (notify) {
            LOG_DEBUG("asyncPoll process notify type:%u, data:%p", notify->type, notify->data);

            // 类型0：任务提交（核心逻辑）
            if (notify->type == 0) {
                ThreadPoolTask* task = (ThreadPoolTask*)notify->data;
                if (task) {
                    // 选择最优worker
                    int worker_idx = select_best_worker(handle);
                    if (worker_idx >= 0) {
                        WorkerThread* worker = &handle->workers[worker_idx];
                        // 尝试入worker队列，失败则入pending队列
                        if (worker_queue_push(worker, task) != 0) {
                            pending_queue_push(&handle->pending_queue, task);
                            LOG_WARN("task[%lu] push to worker[%d] failed, add to pending queue", 
                                    task->task_id, worker_idx);
                        } else {
                            // 唤醒worker线程
                            pthread_cond_signal(&worker->cond_task);
                            LOG_DEBUG("asyncPoll notify worker[%d] process task[%lu]", 
                                    worker_idx, task->task_id);
                        }
                    } else {
                        // 无可用worker，入pending队列
                        pending_queue_push(&handle->pending_queue, task);
                        LOG_WARN("no worker for task[%lu], add to pending queue", task->task_id);
                    }
                }
            } else {
                // 自定义通知类型处理（可扩展）
                LOG_INFO("asyncPoll process custom notify type:%u, data:%p", 
                        notify->type, notify->data);
                // TODO: 扩展自定义通知处理逻辑
            }

            // 继续处理下一个通知
            notify = notify_queue_pop(handle);
        }

        // 消费pending队列：尝试将pending任务分配到空闲worker
        for (int i = 0; i < 64; i++) {
            WorkerThread* worker = &handle->workers[i];
            if (worker->state == WORKER_STATE_IDLE || worker->queue_size < worker->queue_cap) {
                worker_process_pending(handle, worker);
            }
        }
    }

    LOG_INFO("asyncPoll thread exit (tid:%lu)", (unsigned long)pthread_self());
    pthread_exit(NULL);
}

/**
 * @brief Worker线程主函数（执行任务）
 */
static void* worker_thread(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    if (!worker || !worker->pool) {
        LOG_ERROR("worker_thread invalid param");
        pthread_exit(NULL);
    }

    ThreadPoolHandle handle = worker->pool;
    LOG_INFO("worker[%d] thread init (tid:%lu, state:INIT)", 
            worker->worker_idx, (unsigned long)pthread_self());

    // 等待线程池启动信号
    pthread_mutex_lock(&handle->start_mutex);
    while (!handle->is_started && !handle->is_destroying) {
        pthread_cond_wait(&handle->cond_start, &handle->start_mutex);
    }
    pthread_mutex_unlock(&handle->start_mutex);

    // 设置初始状态为IDLE
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start5;", worker->worker_idx);
    pthread_mutex_lock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End5;", worker->worker_idx);
    worker->state = WORKER_STATE_IDLE;
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start7;", worker->worker_idx);
    pthread_mutex_unlock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end7;", worker->worker_idx);

    LOG_INFO("worker[%d] thread start (tid:%lu, state:IDLE)", 
            worker->worker_idx, (unsigned long)pthread_self());

    // 主循环：处理任务
    while (!handle->is_destroying) {
        LOG_INFO("[WZY] break1:worker[%d] thread init (tid:%lu, state:INIT)", worker->worker_idx, (unsigned long)pthread_self());
        ThreadPoolTask* task = NULL;

        // 等待任务通知
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start6;", worker->worker_idx);
        pthread_mutex_lock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End6;", worker->worker_idx);
        LOG_INFO("[WZY] break2:worker[%d] thread init (tid:%lu, state:INIT)", worker->worker_idx, (unsigned long)pthread_self());
        while (worker->queue_size == 0 && !handle->is_destroying && worker->state != WORKER_STATE_EXIT) {
            worker->state = WORKER_STATE_IDLE;
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }
        LOG_INFO("[WZY] break3:worker[%d] thread init (tid:%lu, state:INIT)", worker->worker_idx, (unsigned long)pthread_self());
        // 检查退出条件
        if (handle->is_destroying || worker->state == WORKER_STATE_EXIT) {
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start8;", worker->worker_idx);
            pthread_mutex_unlock(&worker->mutex);
            LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end8;", worker->worker_idx);
            break;
        }
        LOG_INFO("[WZY] break4:worker[%d] thread init (tid:%lu, state:INIT)", worker->worker_idx, (unsigned long)pthread_self());
        // 取出任务，设置为BUSY状态
        worker->state = WORKER_STATE_BUSY;
        task = worker_queue_pop(worker);
        LOG_INFO("[WZY] break5:worker[%d] thread init (tid:%lu, state:INIT)", worker->worker_idx, (unsigned long)pthread_self());
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start9;", worker->worker_idx);
        pthread_mutex_unlock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end9;", worker->worker_idx);

        // 执行任务
        if (task) {
            LOG_DEBUG("worker[%d] start process task[%lu]", worker->worker_idx, task->task_id);
            pthread_mutex_lock(&handle->stats_mutex);
            handle->running_tasks++;
            pthread_mutex_unlock(&handle->stats_mutex);

            // 执行任务函数
            bool success = true;
            task->task_func(task->task_arg);
            task->is_completed = true;

            // 任务完成处理
            task_complete(handle, task->task_id, success);
            
            // 释放任务内存（用户参数由用户管理，此处释放任务结构体）
            LOG_DEBUG("worker[%d] task[%lu] released", worker->worker_idx, task->task_id);
            free(task);

            // 尝试拉取pending队列任务
            worker_process_pending(handle, worker);
        }

        // 重置为IDLE状态
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start7;", worker->worker_idx);
        pthread_mutex_lock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End7;", worker->worker_idx);
        worker->state = WORKER_STATE_IDLE;
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start10;", worker->worker_idx);
        pthread_mutex_unlock(&worker->mutex);
        LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end10;", worker->worker_idx);
    }

    // 退出清理
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX Start8;", worker->worker_idx);
    pthread_mutex_lock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Add MUTEX End8;", worker->worker_idx);
    worker->state = WORKER_STATE_EXIT;
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX Start11;", worker->worker_idx);
    pthread_mutex_unlock(&worker->mutex);
    LOG_DEBUG("[WZY][MUTEX] WorkId = %d, Del MUTEX end12;", worker->worker_idx);
    LOG_INFO("worker[%d] thread exit (tid:%lu, state:EXIT)", 
            worker->worker_idx, (unsigned long)pthread_self());

    pthread_exit(NULL);
}

// ====================== 对外接口实现 ======================
/**
 * @brief 初始化线程池（仅初始化，不运行）
 */
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    if (worker_queue_cap < 2) {
        LOG_ERROR("worker queue cap must >=2 (input:%u)", worker_queue_cap);
        return NULL;
    }

    // 分配线程池结构体
    ThreadPoolHandle handle = (ThreadPoolHandle)calloc(1, sizeof(struct _ThreadPool));
    if (!handle) {
        LOG_ERROR("alloc thread pool failed, errno:%d", errno);
        return NULL;
    }

    // 初始化默认参数
    handle->is_initialized = false;
    handle->is_running = false;
    handle->is_destroying = false;
    handle->next_task_id = 1; // 任务ID从1开始
    handle->notify_queue_cap = 128; // 默认通知队列容量
    handle->is_started = false;

    // 初始化锁和条件变量
    int ret = 0;
    ret |= pthread_mutex_init(&handle->global_mutex, NULL);
    ret |= pthread_mutex_init(&handle->task_id_mutex, NULL);
    ret |= pthread_mutex_init(&handle->stats_mutex, NULL);
    ret |= pthread_mutex_init(&handle->start_mutex, NULL);
    ret |= pthread_cond_init(&handle->cond_interrupt, NULL);
    ret |= pthread_cond_init(&handle->cond_all_done, NULL);
    ret |= pthread_cond_init(&handle->cond_start, NULL);
    if (ret != 0) {
        LOG_ERROR("init global mutex/cond failed, ret:%d", ret);
        goto err_cleanup;
    }

    // 初始化通知队列
    handle->notify_queue = (NotifyItem*)calloc(handle->notify_queue_cap, sizeof(NotifyItem));
    if (!handle->notify_queue) {
        LOG_ERROR("alloc notify queue failed, errno:%d", errno);
        goto err_cleanup;
    }
    handle->notify_queue_head = 0;
    handle->notify_queue_tail = 0;
    handle->notify_queue_size = 0;

    // 初始化Pending队列
    PendingTaskQueue* pending = &handle->pending_queue;
    pending->cap = (pending_queue_cap == 0) ? 1024 : pending_queue_cap;
    pending->tasks = (ThreadPoolTask**)calloc(pending->cap, sizeof(ThreadPoolTask*));
    pending->size = 0;
    pending->head = 0;
    pending->tail = 0;
    pending->is_destroying = false;
    ret |= pthread_mutex_init(&pending->mutex, NULL);
    ret |= pthread_cond_init(&pending->cond_has_task, NULL);
    if (ret != 0) {
        LOG_ERROR("init pending queue mutex/cond failed, ret:%d", ret);
        goto err_cleanup;
    }

    // 初始化64个Worker线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        worker->worker_idx = i;
        worker->pool = handle;
        worker->state = WORKER_STATE_INIT;
        worker->queue_cap = worker_queue_cap;
        worker->queue_head = 0;
        worker->queue_tail = 0;
        worker->queue_size = 0;

        // 分配worker任务队列
        worker->task_queue = (ThreadPoolTask**)calloc(worker_queue_cap, sizeof(ThreadPoolTask*));
        if (!worker->task_queue) {
            LOG_ERROR("alloc worker[%d] queue failed, errno:%d", i, errno);
            goto err_cleanup;
        }

        // 初始化worker锁和条件变量
        ret |= pthread_mutex_init(&worker->mutex, NULL);
        ret |= pthread_cond_init(&worker->cond_task, NULL);
        if (ret != 0) {
            LOG_ERROR("init worker[%d] mutex/cond failed, ret:%d", i, ret);
            goto err_cleanup;
        }

        // 创建worker线程（仅初始化，不运行）
        ret = pthread_create(&worker->tid, NULL, worker_thread, worker);
        if (ret != 0) {
            LOG_ERROR("create worker[%d] thread failed, ret:%d", i, ret);
            goto err_cleanup;
        }
        LOG_DEBUG("worker[%d] thread created (tid:%lu)", i, (unsigned long)worker->tid);
    }

    // 创建asyncPoll线程（仅初始化，不运行）
    ret = pthread_create(&handle->async_poll_tid, NULL, async_poll_thread, handle);
    if (ret != 0) {
        LOG_ERROR("create asyncPoll thread failed, ret:%d", ret);
        goto err_cleanup;
    }
    LOG_DEBUG("asyncPoll thread created (tid:%lu)", (unsigned long)handle->async_poll_tid);

    handle->is_initialized = true;
    LOG_INFO("thread pool init success (64 workers, asyncPoll:1, worker queue cap:%u, pending cap:%u)",
            worker_queue_cap, pending->cap);
    return handle;

err_cleanup:
    // 清理已分配资源
    if (handle) {
        // 销毁worker相关资源
        for (int i = 0; i < 64; i++) {
            WorkerThread* worker = &handle->workers[i];
            if (worker->task_queue) free(worker->task_queue);
            pthread_mutex_destroy(&worker->mutex);
            pthread_cond_destroy(&worker->cond_task);
        }

        // 销毁pending队列
        if (handle->pending_queue.tasks) free(handle->pending_queue.tasks);
        pthread_mutex_destroy(&handle->pending_queue.mutex);
        pthread_cond_destroy(&handle->pending_queue.cond_has_task);

        // 销毁通知队列
        if (handle->notify_queue) free(handle->notify_queue);

        // 销毁全局锁/条件变量
        pthread_mutex_destroy(&handle->global_mutex);
        pthread_mutex_destroy(&handle->task_id_mutex);
        pthread_mutex_destroy(&handle->stats_mutex);
        pthread_mutex_destroy(&handle->start_mutex);
        pthread_cond_destroy(&handle->cond_interrupt);
        pthread_cond_destroy(&handle->cond_all_done);
        pthread_cond_destroy(&handle->cond_start);

        free(handle);
    }
    return NULL;
}

/**
 * @brief 启动线程池
 */
int thread_pool_start(ThreadPoolHandle handle) {
    if (!handle || !handle->is_initialized || handle->is_running) {
        LOG_ERROR("thread_pool_start invalid state (init:%s, running:%s)",
                handle ? (handle->is_initialized ? "yes" : "no") : "no",
                handle ? (handle->is_running ? "yes" : "no") : "no");
        return -1;
    }

    pthread_mutex_lock(&handle->start_mutex);
    handle->is_started = true;
    handle->is_running = true;
    pthread_cond_broadcast(&handle->cond_start); // 唤醒所有线程
    pthread_mutex_unlock(&handle->start_mutex);

    LOG_INFO("thread pool start success (asyncPoll tid:%lu, 64 workers)",
            (unsigned long)handle->async_poll_tid);
    return 0;
}

/**
 * @brief 提交单个任务
 */
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                 void (*task_func)(void* arg),
                                 void* task_arg,
                                 TaskCompleteCb complete_cb,
                                 void* user_data) {
    if (!handle || !handle->is_running || !task_func) {
        LOG_ERROR("submit task invalid param (running:%s)",
                handle ? (handle->is_running ? "yes" : "no") : "no");
        return 0;
    }

    // 生成唯一任务ID
    pthread_mutex_lock(&handle->task_id_mutex);
    uint64_t task_id = handle->next_task_id++;
    pthread_mutex_unlock(&handle->task_id_mutex);

    // 封装任务结构体
    ThreadPoolTask* task = (ThreadPoolTask*)calloc(1, sizeof(ThreadPoolTask));
    if (!task) {
        LOG_ERROR("alloc task[%lu] failed, errno:%d", task_id, errno);
        return 0;
    }
    task->task_id = task_id;
    task->task_func = task_func;
    task->task_arg = task_arg;
    task->is_completed = false;

    // 设置回调
    pthread_mutex_lock(&handle->global_mutex);
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;
    pthread_mutex_unlock(&handle->global_mutex);

    // 触发asyncPoll通知（类型0=任务提交）
    int ret = notify_queue_push(handle, 0, task);
    if (ret != 0) {
        LOG_ERROR("notify asyncPoll failed for task[%lu]", task_id);
        free(task);
        return 0;
    }

    LOG_INFO("submit task[%lu] success (arg:%p, cb:%p)",
            task_id, task_arg, complete_cb);
    return task_id;
}

/**
 * @brief 批量提交任务
 */
uint64_t* thread_pool_submit_batch_tasks(ThreadPoolHandle handle,
                                         ThreadPoolTask* tasks,
                                         uint32_t task_count,
                                         TaskCompleteCb complete_cb,
                                         void* user_data) {
    if (!handle || !handle->is_running || !tasks || task_count == 0) {
        LOG_ERROR("submit batch tasks invalid param (running:%s, count:%u)",
                handle ? (handle->is_running ? "yes" : "no") : "no", task_count);
        return NULL;
    }

    // 分配任务ID数组
    uint64_t* task_ids = (uint64_t*)calloc(task_count, sizeof(uint64_t));
    if (!task_ids) {
        LOG_ERROR("alloc batch task ids failed, errno:%d", errno);
        return NULL;
    }

    // 生成批量任务ID
    pthread_mutex_lock(&handle->task_id_mutex);
    for (uint32_t i = 0; i < task_count; i++) {
        task_ids[i] = handle->next_task_id++;
        tasks[i].task_id = task_ids[i];
        tasks[i].is_completed = false;
    }
    pthread_mutex_unlock(&handle->task_id_mutex);

    // 选择单个worker保证执行顺序
    int worker_idx = select_best_worker(handle);
    if (worker_idx < 0) {
        LOG_ERROR("select worker for batch tasks failed");
        free(task_ids);
        return NULL;
    }
    WorkerThread* worker = &handle->workers[worker_idx];

    // 设置回调
    pthread_mutex_lock(&handle->global_mutex);
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;
    pthread_mutex_unlock(&handle->global_mutex);

    // 批量入队到选中的worker
    bool batch_success = true;
    for (uint32_t i = 0; i < task_count; i++) {
        ThreadPoolTask* task = &tasks[i];
        if (worker_queue_push(worker, task) != 0) {
            // worker队列满，入pending队列
            pending_queue_push(&handle->pending_queue, task);
            LOG_WARN("batch task[%lu] push to worker[%d] failed, add to pending",
                    task->task_id, worker_idx);
        }
        LOG_DEBUG("batch task[%lu] submit to worker[%d]", task->task_id, worker_idx);
    }

    // 唤醒worker处理批量任务
    pthread_cond_signal(&worker->cond_task);

    if (batch_success) {
        LOG_INFO("submit batch tasks success (count:%u, worker:%d)",
                task_count, worker_idx);
        return task_ids;
    } else {
        LOG_ERROR("submit batch tasks failed (count:%u)", task_count);
        free(task_ids);
        return NULL;
    }
}

/**
 * @brief 通用通知asyncPoll接口
 */
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle || !handle->is_running) {
        LOG_ERROR("async_poll_notify invalid param (running:%s)",
                handle ? (handle->is_running ? "yes" : "no") : "no");
        return -1;
    }

    int ret = notify_queue_push(handle, notify_type, data);
    if (ret == 0) {
        LOG_INFO("asyncPoll notify success (type:%u, data:%p)", notify_type, data);
    } else {
        LOG_ERROR("asyncPoll notify failed (type:%u)", notify_type);
    }
    return ret;
}

/**
 * @brief 销毁线程池
 */
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (!handle) {
        LOG_ERROR("thread_pool_destroy invalid param");
        return;
    }

    LOG_INFO("thread pool destroy start (wait all tasks complete)");

    // 设置销毁标记
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    pthread_cond_broadcast(&handle->cond_interrupt); // 唤醒asyncPoll
    pthread_mutex_unlock(&handle->global_mutex);

    // 设置pending队列销毁标记
    pthread_mutex_lock(&handle->pending_queue.mutex);
    handle->pending_queue.is_destroying = true;
    pthread_cond_broadcast(&handle->pending_queue.cond_has_task);
    pthread_mutex_unlock(&handle->pending_queue.mutex);

    // 唤醒所有worker线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_cond_signal(&worker->cond_task);
    }

    // 等待所有任务完成
    pthread_mutex_lock(&handle->stats_mutex);
    while (handle->running_tasks > 0) {
        pthread_cond_wait(&handle->cond_all_done, &handle->stats_mutex);
    }
    pthread_mutex_unlock(&handle->stats_mutex);

    // 等待asyncPoll线程退出
    pthread_join(handle->async_poll_tid, NULL);
    LOG_DEBUG("asyncPoll thread joined (tid:%lu)", (unsigned long)handle->async_poll_tid);

    // 等待所有worker线程退出
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_join(worker->tid, NULL);
        LOG_DEBUG("worker[%d] thread joined (tid:%lu)", i, (unsigned long)worker->tid);
    }

    // 清理pending队列剩余任务
    ThreadPoolTask* task = NULL;
    while ((task = pending_queue_pop(&handle->pending_queue)) != NULL) {
        LOG_WARN("pending queue task[%lu] not executed, free", task->task_id);
        free(task);
    }

    // 清理worker队列剩余任务
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        ThreadPoolTask* w_task = NULL;
        while ((w_task = worker_queue_pop(worker)) != NULL) {
            LOG_WARN("worker[%d] task[%lu] not executed, free", i, w_task->task_id);
            free(w_task);
        }
    }

    // 释放内存&销毁锁/条件变量
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        free(worker->task_queue);
        pthread_mutex_destroy(&worker->mutex);
        pthread_cond_destroy(&worker->cond_task);
    }

    free(handle->pending_queue.tasks);
    pthread_mutex_destroy(&handle->pending_queue.mutex);
    pthread_cond_destroy(&handle->pending_queue.cond_has_task);

    free(handle->notify_queue);
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->stats_mutex);
    pthread_mutex_destroy(&handle->start_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);
    pthread_cond_destroy(&handle->cond_start);

    LOG_INFO("thread pool destroy success (completed tasks:%u)", handle->completed_tasks);
    free(handle);
}