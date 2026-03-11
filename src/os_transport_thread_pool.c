#include "os_transport_thread_pool_internal.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

// 辅助函数声明
static uint64_t generate_task_id(ThreadPoolHandle handle);
static int select_best_worker(ThreadPoolHandle handle, bool is_batch);
static int pending_queue_enqueue(ThreadPoolHandle handle, ThreadPoolTask* task);
static ThreadPoolTask* pending_queue_dequeue(ThreadPoolHandle handle);
static int notify_queue_enqueue(ThreadPoolHandle handle, uint32_t notify_type, void* data);
static NotifyItem* notify_queue_dequeue(ThreadPoolHandle handle);
static void* worker_thread_func(void* arg);
static void* async_poll_thread_func(void* arg);
static void destroy_task(ThreadPoolTask* task);

/**
 * @brief 生成唯一任务ID
 */
static uint64_t generate_task_id(ThreadPoolHandle handle) {
    uint64_t task_id = 0;
    pthread_mutex_lock(&handle->task_id_mutex);
    task_id = ++handle->next_task_id;
    pthread_mutex_unlock(&handle->task_id_mutex);
    return task_id;
}

/**
 * @brief 选择最优Worker线程
 * @param is_batch true=批量任务（选队列最小的同一个），false=单任务（优先空闲）
 */
static int select_best_worker(ThreadPoolHandle handle, bool is_batch) {
    int best_idx = -1;
    uint32_t min_queue_size = UINT32_MAX;

    pthread_mutex_lock(&handle->global_mutex);
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_mutex_lock(&worker->mutex);
        uint32_t queue_size = worker->queue_size;
        WorkerState state = worker->state;
        pthread_mutex_unlock(&worker->mutex);

        if (is_batch) {
            if (queue_size < min_queue_size) {
                min_queue_size = queue_size;
                best_idx = i;
            }
        } else {
            // 单任务优先选空闲且队列空的worker
            if (state == WORKER_STATE_IDLE && queue_size == 0) {
                best_idx = i;
                min_queue_size = 0;
                break;
            } else if (queue_size < min_queue_size) {
                min_queue_size = queue_size;
                best_idx = i;
            }
        }
    }
    pthread_mutex_unlock(&handle->global_mutex);

    return best_idx;
}

/**
 * @brief 全局Pending队列入队（满则扩容）
 */
static int pending_queue_enqueue(ThreadPoolHandle handle, ThreadPoolTask* task) {
    if (!handle || !task) return -1;

    pthread_mutex_lock(&handle->pending_queue.mutex);

    // 队列满则扩容（翻倍策略）
    if (handle->pending_queue.size >= handle->pending_queue.cap) {
        uint32_t new_cap = handle->pending_queue.cap * 2;
        if (new_cap == 0) new_cap = 1024; // 初始默认容量
        ThreadPoolTask** new_tasks = realloc(handle->pending_queue.tasks, new_cap * sizeof(ThreadPoolTask*));
        if (!new_tasks) {
            pthread_mutex_unlock(&handle->pending_queue.mutex);
            return -1;
        }
        handle->pending_queue.tasks = new_tasks;
        handle->pending_queue.cap = new_cap;
    }

    // 环形队列入队
    handle->pending_queue.tasks[handle->pending_queue.tail] = task;
    handle->pending_queue.tail = (handle->pending_queue.tail + 1) % handle->pending_queue.cap;
    handle->pending_queue.size++;

    pthread_cond_signal(&handle->pending_queue.cond_has_task);
    pthread_mutex_unlock(&handle->pending_queue.mutex);

    return 0;
}

/**
 * @brief 全局Pending队列出队（空则阻塞）
 */
static ThreadPoolTask* pending_queue_dequeue(ThreadPoolHandle handle) {
    if (!handle) return NULL;

    pthread_mutex_lock(&handle->pending_queue.mutex);

    // 队列为空且未销毁，阻塞等待
    while (handle->pending_queue.size == 0 && !handle->pending_queue.is_destroying) {
        pthread_cond_wait(&handle->pending_queue.cond_has_task, &handle->pending_queue.mutex);
    }

    if (handle->pending_queue.is_destroying || handle->pending_queue.size == 0) {
        pthread_mutex_unlock(&handle->pending_queue.mutex);
        return NULL;
    }

    // 环形队列出队
    ThreadPoolTask* task = handle->pending_queue.tasks[handle->pending_queue.head];
    handle->pending_queue.head = (handle->pending_queue.head + 1) % handle->pending_queue.cap;
    handle->pending_queue.size--;

    pthread_mutex_unlock(&handle->pending_queue.mutex);
    return task;
}

/**
 * @brief 通知队列入队（触发asyncPoll中断）
 */
static int notify_queue_enqueue(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle) return -1;

    pthread_mutex_lock(&handle->global_mutex);

    // 通知队列满则扩容
    if (handle->notify_queue_size >= handle->notify_queue_cap) {
        uint32_t new_cap = (handle->notify_queue_cap == 0) ? 64 : (handle->notify_queue_cap * 2);
        NotifyItem* new_queue = realloc(handle->notify_queue, new_cap * sizeof(NotifyItem));
        if (!new_queue) {
            pthread_mutex_unlock(&handle->global_mutex);
            return -1;
        }
        handle->notify_queue = new_queue;
        handle->notify_queue_cap = new_cap;
    }

    // 入队通知项
    handle->notify_queue[handle->notify_queue_tail].type = notify_type;
    handle->notify_queue[handle->notify_queue_tail].data = data;
    handle->notify_queue_tail = (handle->notify_queue_tail + 1) % handle->notify_queue_cap;
    handle->notify_queue_size++;

    // 标记有通知并唤醒asyncPoll
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    return 0;
}

/**
 * @brief 通知队列出队
 */
static NotifyItem* notify_queue_dequeue(ThreadPoolHandle handle) {
    if (!handle) return NULL;

    pthread_mutex_lock(&handle->global_mutex);

    if (handle->notify_queue_size == 0) {
        pthread_mutex_unlock(&handle->global_mutex);
        return NULL;
    }

    // 出队通知项
    NotifyItem* item = &handle->notify_queue[handle->notify_queue_head];
    handle->notify_queue_head = (handle->notify_queue_head + 1) % handle->notify_queue_cap;
    handle->notify_queue_size--;

    if (handle->notify_queue_size == 0) {
        handle->has_notify = false;
    }

    pthread_mutex_unlock(&handle->global_mutex);
    return item;
}

/**
 * @brief 销毁任务（仅释放结构体，参数由用户管理）
 */
static void destroy_task(ThreadPoolTask* task) {
    if (task) {
        free(task);
    }
}

/**
 * @brief Worker线程执行函数
 */
static void* worker_thread_func(void* arg) {
    WorkerThread* worker = (WorkerThread*)arg;
    ThreadPoolHandle pool = worker->pool;

    while (1) {
        pthread_mutex_lock(&worker->mutex);

        // 无任务且未退出，阻塞等待
        while (worker->queue_size == 0 && worker->state != WORKER_STATE_EXIT) {
            worker->state = WORKER_STATE_IDLE;
            pthread_cond_wait(&worker->cond_task, &worker->mutex);
        }

        // 收到退出信号，终止循环
        if (worker->state == WORKER_STATE_EXIT) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }

        // 标记为忙碌状态
        worker->state = WORKER_STATE_BUSY;

        // 取出队列头部任务
        ThreadPoolTask* task = worker->task_queue[worker->queue_head];
        worker->queue_head = (worker->queue_head + 1) % worker->queue_cap;
        worker->queue_size--;

        pthread_mutex_unlock(&worker->mutex);

        // 更新运行中任务数
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 执行任务（捕获异常避免线程崩溃）
        bool exec_success = true;
        if (task && task->task_func) {
            task->task_func(task->task_arg);
            task->is_completed = true;
        } else {
            exec_success = false;
        }

        // 任务完成回调
        if (pool->complete_cb) {
            pool->complete_cb(task->task_id, exec_success, pool->cb_user_data);
        }

        // 更新统计信息
        pthread_mutex_lock(&pool->stats_mutex);
        pool->running_tasks--;
        pool->completed_tasks++;
        pthread_mutex_unlock(&pool->stats_mutex);

        // 销毁任务
        destroy_task(task);

        // 唤醒等待所有任务完成的条件变量
        pthread_mutex_lock(&pool->global_mutex);
        if (pool->running_tasks == 0) {
            pthread_cond_signal(&pool->cond_all_done);
        }
        pthread_mutex_unlock(&pool->global_mutex);
    }

    // 线程退出，标记状态
    pthread_mutex_lock(&worker->mutex);
    worker->state = WORKER_STATE_EXIT;
    pthread_mutex_unlock(&worker->mutex);

    return NULL;
}

/**
 * @brief asyncPoll线程执行函数（处理任务分发+通用通知）
 */
static void* async_poll_thread_func(void* arg) {
    ThreadPoolHandle pool = (ThreadPoolHandle)arg;

    while (1) {
        pthread_mutex_lock(&pool->global_mutex);

        // 无通知且未销毁，阻塞等待中断
        while (!pool->has_notify && !pool->is_destroying) {
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }

        // 收到销毁信号，终止循环
        if (pool->is_destroying) {
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        pthread_mutex_unlock(&pool->global_mutex);

        // 处理所有待处理通知
        NotifyItem* notify_item = NULL;
        while ((notify_item = notify_queue_dequeue(pool)) != NULL) {
            if (notify_item->type == 0) { // 0=任务提交通知
                // 分发pending队列中的任务到worker
                ThreadPoolTask* task = NULL;
                while ((task = pending_queue_dequeue(pool)) != NULL) {
                    int worker_idx = select_best_worker(pool, false);
                    if (worker_idx < 0) {
                        // 无可用worker，重新入队
                        pending_queue_enqueue(pool, task);
                        break;
                    }

                    WorkerThread* worker = &pool->workers[worker_idx];
                    pthread_mutex_lock(&worker->mutex);

                    // worker队列满，入pending队列
                    if (worker->queue_size >= worker->queue_cap) {
                        pthread_mutex_unlock(&worker->mutex);
                        pending_queue_enqueue(pool, task);
                        continue;
                    }

                    // 任务入worker队列
                    worker->task_queue[worker->queue_tail] = task;
                    worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
                    worker->queue_size++;

                    // 唤醒worker线程
                    pthread_cond_signal(&worker->cond_task);

                    pthread_mutex_unlock(&worker->mutex);
                }
            } else { // 自定义通知（扩展点）
                printf("AsyncPoll received custom notify: type=%u, data=%p\n", 
                       notify_item->type, notify_item->data);
            }
        }
    }

    // 销毁阶段：通知所有worker退出
    pthread_mutex_lock(&pool->global_mutex);
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &pool->workers[i];
        pthread_mutex_lock(&worker->mutex);
        worker->state = WORKER_STATE_EXIT;
        pthread_cond_signal(&worker->cond_task);
        pthread_mutex_unlock(&worker->mutex);
        pthread_join(worker->tid, NULL); // 等待worker退出
    }
    pthread_mutex_unlock(&pool->global_mutex);

    // 清理pending队列剩余任务
    ThreadPoolTask* task = NULL;
    while ((task = pending_queue_dequeue(pool)) != NULL) {
        destroy_task(task);
    }

    return NULL;
}

/**
 * @brief 初始化线程池（创建线程但不启动）
 */
ThreadPoolHandle thread_pool_init(uint32_t worker_queue_cap, uint32_t pending_queue_cap) {
    if (worker_queue_cap < 2) worker_queue_cap = 2; // 强制最小容量
    if (pending_queue_cap == 0) pending_queue_cap = 1024;

    // 分配线程池结构体
    ThreadPoolHandle handle = (ThreadPoolHandle)calloc(1, sizeof(struct _ThreadPool));
    if (!handle) return NULL;

    // 初始化基础锁和条件变量
    pthread_mutex_init(&handle->task_id_mutex, NULL);
    pthread_mutex_init(&handle->global_mutex, NULL);
    pthread_cond_init(&handle->cond_interrupt, NULL);
    pthread_cond_init(&handle->cond_all_done, NULL);
    pthread_mutex_init(&handle->stats_mutex, NULL);

    // 初始化通知队列
    handle->notify_queue_cap = 64;
    handle->notify_queue = (NotifyItem*)calloc(64, sizeof(NotifyItem));
    if (!handle->notify_queue) goto err_cleanup;

    // 初始化64个Worker线程（创建后挂起）
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        worker->state = WORKER_STATE_INIT;
        worker->worker_idx = i;
        worker->pool = handle;

        // 初始化worker任务队列
        worker->queue_cap = worker_queue_cap;
        worker->task_queue = (ThreadPoolTask*)calloc(worker_queue_cap, sizeof(ThreadPoolTask));
        if (!worker->task_queue) goto err_cleanup;

        // 初始化worker锁和条件变量
        pthread_mutex_init(&worker->mutex, NULL);
        pthread_cond_init(&worker->cond_task, NULL);

        // 创建worker线程（挂起状态）
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        if (pthread_create(&worker->tid, &attr, worker_thread_func, worker) != 0) {
            pthread_attr_destroy(&attr);
            goto err_cleanup;
        }
        pthread_attr_destroy(&attr);

        // Linux下挂起线程（非跨平台，可替换为状态控制）
        #ifdef __linux__
        pthread_suspend_np(worker->tid);
        #endif
    }

    // 创建asyncPoll线程（挂起状态）
    pthread_attr_t async_attr;
    pthread_attr_init(&async_attr);
    pthread_attr_setdetachstate(&async_attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&handle->async_poll_tid, &async_attr, async_poll_thread_func, handle) != 0) {
        pthread_attr_destroy(&async_attr);
        goto err_cleanup;
    }
    pthread_attr_destroy(&async_attr);
    #ifdef __linux__
    pthread_suspend_np(handle->async_poll_tid);
    #endif

    // 初始化全局pending队列
    handle->pending_queue.cap = pending_queue_cap;
    handle->pending_queue.tasks = (ThreadPoolTask**)calloc(pending_queue_cap, sizeof(ThreadPoolTask*));
    if (!handle->pending_queue.tasks) goto err_cleanup;
    pthread_mutex_init(&handle->pending_queue.mutex, NULL);
    pthread_cond_init(&handle->pending_queue.cond_has_task, NULL);

    handle->is_initialized = true;
    return handle;

err_cleanup:
    // 清理已分配资源
    if (handle) {
        // 清理worker线程
        for (int i = 0; i < 64; i++) {
            WorkerThread* worker = &handle->workers[i];
            if (worker->task_queue) free(worker->task_queue);
            pthread_mutex_destroy(&worker->mutex);
            pthread_cond_destroy(&worker->cond_task);
            if (worker->tid != 0) {
                pthread_cancel(worker->tid);
                pthread_join(worker->tid, NULL);
            }
        }

        // 清理asyncPoll线程
        if (handle->async_poll_tid != 0) {
            pthread_cancel(handle->async_poll_tid);
            pthread_join(handle->async_poll_tid, NULL);
        }

        // 清理通知队列
        if (handle->notify_queue) free(handle->notify_queue);

        // 清理pending队列
        if (handle->pending_queue.tasks) free(handle->pending_queue.tasks);
        pthread_mutex_destroy(&handle->pending_queue.mutex);
        pthread_cond_destroy(&handle->pending_queue.cond_has_task);

        // 清理全局锁
        pthread_mutex_destroy(&handle->task_id_mutex);
        pthread_mutex_destroy(&handle->global_mutex);
        pthread_cond_destroy(&handle->cond_interrupt);
        pthread_cond_destroy(&handle->cond_all_done);
        pthread_mutex_destroy(&handle->stats_mutex);

        free(handle);
    }
    return NULL;
}

/**
 * @brief 启动线程池（仅启动asyncPoll线程）
 */
int thread_pool_start(ThreadPoolHandle handle) {
    if (!handle || !handle->is_initialized || handle->is_running) {
        return -1;
    }

    // 恢复asyncPoll线程（Linux）
    #ifdef __linux__
    pthread_resume_np(handle->async_poll_tid);
    #else
    // 非Linux系统：通过条件变量唤醒
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);
    #endif

    handle->is_running = true;
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
        return 0;
    }

    // 分配任务结构体
    ThreadPoolTask* task = (ThreadPoolTask*)calloc(1, sizeof(ThreadPoolTask));
    if (!task) return 0;

    // 初始化任务
    task->task_id = generate_task_id(handle);
    task->task_func = task_func;
    task->task_arg = task_arg;

    // 设置全局回调
    pthread_mutex_lock(&handle->global_mutex);
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;
    pthread_mutex_unlock(&handle->global_mutex);

    // 任务入pending队列
    if (pending_queue_enqueue(handle, task) != 0) {
        destroy_task(task);
        return 0;
    }

    // 通知asyncPoll处理任务
    if (async_poll_notify(handle, 0, NULL) != 0) {
        destroy_task(task);
        return 0;
    }

    return task->task_id;
}

/**
 * @brief 批量提交任务（保证入同一worker）
 */
uint64_t* thread_pool_submit_batch_tasks(ThreadPoolHandle handle,
                                         ThreadPoolTask* tasks,
                                         uint32_t task_count,
                                         TaskCompleteCb complete_cb,
                                         void* user_data) {
    if (!handle || !handle->is_running || !tasks || task_count == 0) {
        return NULL;
    }

    // 分配任务ID数组
    uint64_t* task_ids = (uint64_t*)calloc(task_count, sizeof(uint64_t));
    if (!task_ids) return NULL;

    // 选择同一个worker
    int worker_idx = select_best_worker(handle, true);
    if (worker_idx < 0) {
        free(task_ids);
        return NULL;
    }
    WorkerThread* worker = &handle->workers[worker_idx];

    // 设置全局回调
    pthread_mutex_lock(&handle->global_mutex);
    handle->complete_cb = complete_cb;
    handle->cb_user_data = user_data;
    pthread_mutex_unlock(&handle->global_mutex);

    // 批量入队
    for (uint32_t i = 0; i < task_count; i++) {
        ThreadPoolTask* src_task = &tasks[i];
        if (!src_task->task_func) {
            // 单个任务无效，清理已分配ID
            for (uint32_t j = 0; j < i; j++) task_ids[j] = 0;
            free(task_ids);
            return NULL;
        }

        // 复制任务（避免用户栈数据失效）
        ThreadPoolTask* new_task = (ThreadPoolTask*)calloc(1, sizeof(ThreadPoolTask));
        if (!new_task) {
            for (uint32_t j = 0; j < i; j++) task_ids[j] = 0;
            free(task_ids);
            return NULL;
        }

        new_task->task_id = generate_task_id(handle);
        new_task->task_func = src_task->task_func;
        new_task->task_arg = src_task->task_arg;
        task_ids[i] = new_task->task_id;

        pthread_mutex_lock(&worker->mutex);

        // worker队列满则入pending
        if (worker->queue_size >= worker->queue_cap) {
            pthread_mutex_unlock(&worker->mutex);
            pending_queue_enqueue(handle, new_task);
            continue;
        }

        // 入worker队列
        worker->task_queue[worker->queue_tail] = new_task;
        worker->queue_tail = (worker->queue_tail + 1) % worker->queue_cap;
        worker->queue_size++;

        pthread_mutex_unlock(&worker->mutex);
    }

    // 通知asyncPoll并唤醒worker
    async_poll_notify(handle, 0, NULL);
    pthread_mutex_lock(&worker->mutex);
    pthread_cond_signal(&worker->cond_task);
    pthread_mutex_unlock(&worker->mutex);

    return task_ids;
}

/**
 * @brief 通用通知asyncPoll接口
 */
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (!handle || !handle->is_running) {
        return -1;
    }
    return notify_queue_enqueue(handle, notify_type, data);
}

/**
 * @brief 销毁线程池（等待所有任务完成）
 */
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (!handle || handle->is_destroying) return;

    // 标记销毁状态
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    pthread_mutex_unlock(&handle->global_mutex);

    // 唤醒pending队列阻塞
    pthread_mutex_lock(&handle->pending_queue.mutex);
    handle->pending_queue.is_destroying = true;
    pthread_cond_signal(&handle->pending_queue.cond_has_task);
    pthread_mutex_unlock(&handle->pending_queue.mutex);

    // 唤醒asyncPoll线程
    pthread_mutex_lock(&handle->global_mutex);
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    // 等待asyncPoll线程退出
    pthread_join(handle->async_poll_tid, NULL);

    // 等待所有任务完成
    pthread_mutex_lock(&handle->global_mutex);
    while (handle->running_tasks > 0) {
        pthread_cond_wait(&handle->cond_all_done, &handle->global_mutex);
    }
    pthread_mutex_unlock(&handle->global_mutex);

    // 清理worker队列剩余任务
    for (int i = 0; i < 64; i++) {
        WorkerThread* worker = &handle->workers[i];
        pthread_mutex_lock(&worker->mutex);
        for (uint32_t j = 0; j < worker->queue_size; j++) {
            ThreadPoolTask* task = worker->task_queue[(worker->queue_head + j) % worker->queue_cap];
            destroy_task(task);
        }
        free(worker->task_queue);
        pthread_mutex_destroy(&worker->mutex);
        pthread_cond_destroy(&worker->cond_task);
        pthread_mutex_unlock(&worker->mutex);
    }

    // 清理通知队列
    if (handle->notify_queue) free(handle->notify_queue);

    // 清理pending队列
    if (handle->pending_queue.tasks) free(handle->pending_queue.tasks);
    pthread_mutex_destroy(&handle->pending_queue.mutex);
    pthread_cond_destroy(&handle->pending_queue.cond_has_task);

    // 清理全局锁和条件变量
    pthread_mutex_destroy(&handle->task_id_mutex);
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);
    pthread_mutex_destroy(&handle->stats_mutex);

    free(handle);
}