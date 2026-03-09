#include "os_transport_thread_pool_internal.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>

// ===================== 工具函数 =====================
// 查找任务数最少的worker（优先空闲，无空闲找最少任务）
static int find_least_task_worker(struct _ThreadPool* pool) {
    int target_idx = -1;
    uint32_t min_task_count = UINT32_MAX;

    for (int i = 0; i < 64; i++) {
        pthread_mutex_lock(&pool->workers[i].mutex);
        // 跳过已销毁/未运行的worker
        if (!pool->workers[i].is_running || pool->is_destroying) {
            pthread_mutex_unlock(&pool->workers[i].mutex);
            continue;
        }

        // 优先选择空闲worker（队列空）
        if (pool->workers[i].is_idle && pool->workers[i].queue_size == 0) {
            target_idx = i;
            pthread_mutex_unlock(&pool->workers[i].mutex);
            return target_idx;
        }

        // 记录任务数最少的worker
        if (pool->workers[i].queue_size < min_task_count) {
            min_task_count = pool->workers[i].queue_size;
            target_idx = i;
        }
        pthread_mutex_unlock(&pool->workers[i].mutex);
    }

    return target_idx; // 返回任务数最少的worker（-1=无可用）
}

// 环形队列入队（worker队列）
static int worker_queue_push(WorkerThread* worker, const ThreadPoolTask* task) {
    if (worker == NULL || task == NULL || worker->queue_size >= worker->queue_cap) {
        return -1;
    }

    worker->task_queue[worker->queue_tail] = *task;
    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
    worker->queue_size++;
    return 0;
}

// 环形队列出队（worker队列）
static int worker_queue_pop(WorkerThread* worker, ThreadPoolTask* task) {
    if (worker == NULL || task == NULL || worker->queue_size == 0) {
        return -1;
    }

    *task = worker->task_queue[worker->queue_head];
    worker->queue_head = (worker->queue_head + 1) % worker->queue_cap;
    worker->queue_size--;
    return 0;
}

// ===================== Pending队列操作（核心补充） =====================
// 初始化pending队列
static int pending_queue_init(PendingTaskQueue* queue, uint32_t init_cap) {
    if (queue == NULL || init_cap == 0) {
        init_cap = 1024; // 默认初始容量
    }

    queue->tasks = (ThreadPoolTask**)calloc(init_cap, sizeof(ThreadPoolTask*));
    if (queue->tasks == NULL) {
        return -1;
    }

    queue->cap = init_cap;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond_pending, NULL);
    return 0;
}

// pending队列动态扩容（翻倍）
static int pending_queue_resize(PendingTaskQueue* queue) {
    if (queue == NULL) return -1;

    uint32_t new_cap = queue->cap * 2;
    ThreadPoolTask** new_tasks = (ThreadPoolTask**)calloc(new_cap, sizeof(ThreadPoolTask*));
    if (new_tasks == NULL) {
        return -1;
    }

    // 拷贝原有任务到新队列
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

    TRANSPORT_LOG("INFO", "pending queue resize: %d → %d", queue->cap/2, queue->cap);
    return 0;
}

// pending队列入队（缓存任务）
static int pending_queue_push(PendingTaskQueue* queue, ThreadPoolTask* task) {
    if (queue == NULL || task == NULL) return -1;

    pthread_mutex_lock(&queue->mutex);
    // 队列满则扩容
    while (queue->size >= queue->cap) {
        pthread_mutex_unlock(&queue->mutex);
        if (pending_queue_resize(queue) != 0) {
            TRANSPORT_LOG("ERROR", "pending queue resize failed");
            return -1;
        }
        pthread_mutex_lock(&queue->mutex);
    }

    queue->tasks[queue->tail] = task;
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;
    pthread_cond_signal(&queue->cond_pending); // 通知asyncPoll有pending任务
    pthread_mutex_unlock(&queue->mutex);

    TRANSPORT_LOG("INFO", "task %lu added to pending queue (size=%d)", task->task_id, queue->size);
    return 0;
}

// pending队列出队
static ThreadPoolTask* pending_queue_pop(PendingTaskQueue* queue) {
    if (queue == NULL) return NULL;

    pthread_mutex_lock(&queue->mutex);
    while (queue->size == 0) {
        pthread_cond_wait(&queue->cond_pending, &queue->mutex);
        if (queue->size == 0) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
    }

    ThreadPoolTask* task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;
    pthread_mutex_unlock(&queue->mutex);

    return task;
}

// 销毁pending队列
static void pending_queue_destroy(PendingTaskQueue* queue) {
    if (queue == NULL) return;

    pthread_mutex_lock(&queue->mutex);
    // 释放所有pending任务
    for (uint32_t i = 0; i < queue->size; i++) {
        uint32_t idx = (queue->head + i) % queue->cap;
        free(queue->tasks[idx]);
    }
    free(queue->tasks);
    pthread_mutex_unlock(&queue->mutex);

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_pending);
}

// ===================== Worker线程逻辑（无变化） =====================
static void* worker_thread_func(void* arg) {
    WorkerThreadArg* worker_arg = (WorkerThreadArg*)arg;
    struct _ThreadPool* pool = worker_arg->pool;
    int worker_idx = worker_arg->worker_idx;
    free(worker_arg);

    WorkerThread* worker = &pool->workers[worker_idx];
    TRANSPORT_LOG("INFO", "worker %d initialized, waiting for start...", worker_idx);

    // 等待线程池启动
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    if (pool->is_destroying) {
        TRANSPORT_LOG("INFO", "worker %d exit (destroying)", worker_idx);
        pthread_exit(NULL);
        return NULL;
    }

    // 标记worker为运行中+空闲
    pthread_mutex_lock(&worker->mutex);
    worker->is_running = true;
    worker->is_idle = true;
    pthread_mutex_unlock(&worker->mutex);

    TRANSPORT_LOG("INFO", "worker %d started, waiting for task...", worker_idx);

    ThreadPoolTask task;
    while (!pool->is_destroying) {
        // 等待任务通知
        pthread_mutex_lock(&worker->mutex);
        while (worker->queue_size == 0 && !pool->is_destroying) {
            worker->is_idle = true;
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }

        if (pool->is_destroying) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        // 取出任务，标记为忙
        worker->is_idle = false;
        int ret = worker_queue_pop(worker, &task);
        pthread_mutex_unlock(&worker->mutex);

        if (ret != 0) {
            TRANSPORT_LOG("WARN", "worker %d pop task failed", worker_idx);
            continue;
        }

        // 更新运行中任务数
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 执行任务
        TRANSPORT_LOG("INFO", "worker %d start processing task %lu", worker_idx, task.task_id);
        bool success = true;
        if (task.task_func != NULL) {
            task.task_func(task.task_arg);
        } else {
            success = false;
            TRANSPORT_LOG("ERROR", "worker %d task %lu has no execute function", worker_idx, task.task_id);
        }

        // 任务完成：更新统计
        task.is_completed = true;
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks--;
        pool->completed_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 通知外部
        if (pool->complete_cb != NULL) {
            pool->complete_cb(task.task_id, success, pool->complete_user_data);
        }

        // 销毁任务
        TRANSPORT_LOG("INFO", "worker %d task %lu completed, success=%d", worker_idx, task.task_id, success);

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

    TRANSPORT_LOG("INFO", "worker %d exit", worker_idx);
    pthread_exit(NULL);
    return NULL;
}

// ===================== AsyncPoll线程逻辑（核心优化） =====================
static void* async_poll_thread_func(void* arg) {
    struct _ThreadPool* pool = (struct _ThreadPool*)arg;
    TRANSPORT_LOG("INFO", "asyncPoll thread initialized, waiting for start...");

    // 等待线程池启动
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    if (pool->is_destroying) {
        TRANSPORT_LOG("INFO", "asyncPoll thread exit (destroying)");
        pthread_exit(NULL);
        return NULL;
    }

    TRANSPORT_LOG("INFO", "asyncPoll thread started, monitoring interrupt...");

    while (!pool->is_destroying) {
        // 第一步：处理pending队列（优先分发缓存的任务）
        while (1) {
            ThreadPoolTask* pending_task = pending_queue_pop(&pool->pending_queue);
            if (pending_task == NULL) break;

            // 找任务数最少的worker
            int target_idx = find_least_task_worker(pool);
            if (target_idx < 0) {
                TRANSPORT_LOG("WARN", "no available worker, re-add task %lu to pending", pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }

            // 尝试将pending任务放入worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, pending_task);
            pthread_mutex_unlock(&worker->mutex);

            if (ret == 0) {
                // 放入成功，通知worker
                pthread_cond_signal(&worker->cond_task);
                TRANSPORT_LOG("INFO", "asyncPoll assign pending task %lu to worker %d", pending_task->task_id, target_idx);
                free(pending_task);
            } else {
                // worker队列仍满，重新加入pending
                TRANSPORT_LOG("WARN", "worker %d queue full, re-add task %lu to pending", target_idx, pending_task->task_id);
                pending_queue_push(&pool->pending_queue, pending_task);
                break;
            }
        }

        // 第二步：处理外部通知（任务提交/其他事件）
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

        // 处理任务提交通知
        if (notify_type == 0) {
            ThreadPoolTask* task = (ThreadPoolTask*)notify_data;
            if (task == NULL) {
                TRANSPORT_LOG("ERROR", "asyncPoll receive null task");
                continue;
            }

            // 找任务数最少的worker
            int target_idx = find_least_task_worker(pool);
            if (target_idx < 0) {
                TRANSPORT_LOG("WARN", "no available worker, add task %lu to pending", task->task_id);
                pending_queue_push(&pool->pending_queue, task);
                continue;
            }

            // 尝试放入worker队列
            WorkerThread* worker = &pool->workers[target_idx];
            pthread_mutex_lock(&worker->mutex);
            int ret = worker_queue_push(worker, task);
            pthread_mutex_unlock(&worker->mutex);

            if (ret == 0) {
                // 放入成功，通知worker
                pthread_cond_signal(&worker->cond_task);
                TRANSPORT_LOG("INFO", "asyncPoll assign task %lu to worker %d (task count=%d)", 
                             task->task_id, target_idx, worker->queue_size);
                free(task);
            } else {
                // worker队列满，加入pending
                TRANSPORT_LOG("WARN", "worker %d queue full, add task %lu to pending", target_idx, task->task_id);
                pending_queue_push(&pool->pending_queue, task);
            }
        } else {
            // 处理其他类型通知
            TRANSPORT_LOG("INFO", "asyncPoll receive notify type %d, data=%p", notify_type, notify_data);
        }
    }

    // 退出前处理剩余pending任务（可选：通知失败）
    TRANSPORT_LOG("INFO", "asyncPoll exit, processing remaining pending tasks...");
    pthread_mutex_lock(&pool->pending_queue.mutex);
    for (uint32_t i = 0; i < pool->pending_queue.size; i++) {
        uint32_t idx = (pool->pending_queue.head + i) % pool->pending_queue.cap;
        ThreadPoolTask* task = pool->pending_queue.tasks[idx];
        if (pool->complete_cb != NULL) {
            pool->complete_cb(task->task_id, false, pool->complete_user_data);
        }
        free(task);
    }
    pthread_mutex_unlock(&pool->pending_queue.mutex);

    TRANSPORT_LOG("INFO", "asyncPoll thread exit");
    pthread_exit(NULL);
    return NULL;
}

// ===================== 对外接口实现（新增pending参数） =====================
// 初始化线程池（新增pending队列初始化）
ThreadPoolHandle thread_pool_init(uint32_t queue_cap, uint32_t pending_cap) {
    if (queue_cap == 0) {
        TRANSPORT_LOG("ERROR", "queue cap cannot be 0");
        return NULL;
    }

    // 分配线程池内存
    struct _ThreadPool* pool = (struct _ThreadPool*)calloc(1, sizeof(struct _ThreadPool));
    if (pool == NULL) {
        TRANSPORT_LOG("ERROR", "malloc thread pool failed");
        return NULL;
    }

    // 初始化全局同步原语
    pthread_mutex_init(&pool->global_mutex, NULL);
    pthread_cond_init(&pool->cond_interrupt, NULL);
    pthread_cond_init(&pool->cond_all_done, NULL);

    // 初始化任务ID生成器
    pool->next_task_id = 1;
    pthread_mutex_init(&pool->task_id_mutex, NULL);

    // 初始化统计变量
    pool->running_tasks = 0;
    pool->completed_tasks = 0;
    pthread_mutex_init(&pool->stats_mutex, NULL);

    // 初始化pending队列（核心补充）
    if (pending_queue_init(&pool->pending_queue, pending_cap) != 0) {
        TRANSPORT_LOG("ERROR", "init pending queue failed");
        thread_pool_destroy(pool);
        return NULL;
    }

    // 初始化64个worker线程
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        worker->queue_cap = queue_cap;
        worker->task_queue = (ThreadPoolTask*)calloc(queue_cap, sizeof(ThreadPoolTask));
        if (worker->task_queue == NULL) {
            TRANSPORT_LOG("ERROR", "malloc worker %d queue failed", i);
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

        // 创建worker线程
        if (pthread_create(&worker->tid, NULL, worker_thread_func, worker_arg) != 0) {
            TRANSPORT_LOG("ERROR", "create worker %d failed", i);
            free(worker_arg);
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    // 创建asyncPoll线程
    if (pthread_create(&pool->async_poll_tid, NULL, async_poll_thread_func, pool) != 0) {
        TRANSPORT_LOG("ERROR", "create asyncPoll thread failed");
        thread_pool_destroy(pool);
        return NULL;
    }

    // 标记初始化完成
    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;
    pool->has_notify = false;
    pool->complete_cb = NULL;
    pool->complete_user_data = NULL;

    TRANSPORT_LOG("INFO", "thread pool init success: 1asyncPoll + 64workers (pending cap=%d)", pool->pending_queue.cap);
    return (ThreadPoolHandle)pool;
}

// 启动线程池
int thread_pool_start(ThreadPoolHandle handle) {
    if (handle == NULL || !handle->is_initialized || handle->is_running) {
        TRANSPORT_LOG("ERROR", "invalid thread pool state for start");
        return -1;
    }

    // 标记运行状态，唤醒所有线程
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = true;
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    TRANSPORT_LOG("INFO", "thread pool start success");
    return 0;
}

// 外部提交任务
uint64_t thread_pool_submit_task(ThreadPoolHandle handle,
                                 void (*task_func)(void* arg),
                                 void* task_arg,
                                 TaskCompleteCallback complete_cb,
                                 void* user_data) {
    if (handle == NULL || !handle->is_initialized || handle->is_destroying || task_func == NULL) {
        TRANSPORT_LOG("ERROR", "invalid param for submit task");
        return 0;
    }

    // 生成唯一任务ID
    pthread_mutex_lock(&handle->task_id_mutex);
    uint64_t task_id = handle->next_task_id++;
    pthread_mutex_unlock(&handle->task_id_mutex);

    // 构造任务
    ThreadPoolTask* task = (ThreadPoolTask*)malloc(sizeof(ThreadPoolTask));
    if (task == NULL) {
        TRANSPORT_LOG("ERROR", "malloc task %lu failed", task_id);
        return 0;
    }
    task->task_id = task_id;
    task->task_func = task_func;
    task->task_arg = task_arg;
    task->is_completed = false;

    // 保存完成回调
    handle->complete_cb = complete_cb;
    handle->complete_user_data = user_data;

    // 触发asyncPoll中断
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = 0;
    handle->notify_data = task;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    TRANSPORT_LOG("INFO", "submit task %lu success, notify asyncPoll", task_id);
    return task_id;
}

// 通知asyncPoll线程
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (handle == NULL || !handle->is_initialized || handle->is_destroying) {
        TRANSPORT_LOG("ERROR", "invalid param for asyncPoll notify");
        return -1;
    }

    // 触发asyncPoll中断
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = notify_type;
    handle->notify_data = data;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    TRANSPORT_LOG("INFO", "notify asyncPoll type %d, data=%p", notify_type, data);
    return 0;
}

// 销毁线程池
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (handle == NULL) return;

    TRANSPORT_LOG("INFO", "thread pool destroying...");

    // 标记销毁状态，唤醒所有线程
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    handle->has_notify = true;
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_cond_broadcast(&handle->pending_queue.cond_pending); // 唤醒pending队列等待
    pthread_mutex_unlock(&handle->global_mutex);

    // 等待asyncPoll线程退出
    pthread_join(handle->async_poll_tid, NULL);

    // 等待64个worker线程退出并清理资源
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_cond_broadcast(&worker->cond_task);
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
    TRANSPORT_LOG("INFO", "thread pool destroy success");
}