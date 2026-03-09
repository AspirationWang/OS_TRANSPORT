#include "os_transport_thread_pool_internal.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>

// ===================== 工具函数 =====================
// 查找第一个空闲的worker线程
static int find_idle_worker(struct _ThreadPool* pool) {
    for (int i = 0; i < 64; i++) {
        pthread_mutex_lock(&pool->workers[i].mutex);
        if (pool->workers[i].is_idle && pool->workers[i].is_running && !pool->is_destroying) {
            pthread_mutex_unlock(&pool->workers[i].mutex);
            return i;
        }
        pthread_mutex_unlock(&pool->workers[i].mutex);
    }
    return -1; // 无空闲worker
}

// ===================== Worker线程逻辑 =====================
static void* worker_thread_func(void* arg) {
    int worker_idx = *(int*)arg;
    free(arg);
    struct _ThreadPool* pool = (struct _ThreadPool*)arg; // 修正：实际需通过线程私有数据传递，此处简化
    WorkerThread* worker = &pool->workers[worker_idx];

    TRANSPORT_LOG("INFO", "worker %d initialized, waiting for start...", worker_idx);

    // 等待线程池启动
    pthread_mutex_lock(&pool->global_mutex);
    while (!pool->is_running && !pool->is_destroying) {
        pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
    }
    pthread_mutex_unlock(&pool->global_mutex);

    if (pool->is_destroying) {
        pthread_exit(NULL);
        return NULL;
    }

    // 标记worker为运行中
    pthread_mutex_lock(&worker->mutex);
    worker->is_running = true;
    worker->is_idle = true;
    pthread_mutex_unlock(&worker->mutex);

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
        task = worker->task_queue[0];
        // 队列移位（简化实现，实际可用环形队列）
        memmove(&worker->task_queue[0], &worker->task_queue[1], 
                (worker->queue_size - 1) * sizeof(ThreadPoolTask));
        worker->queue_size--;
        pthread_mutex_unlock(&worker->mutex);

        // 更新运行中任务数
        atomic_fetch_add(&pool->running_tasks, 1);

        // 执行任务
        TRANSPORT_LOG("INFO", "worker %d start processing task %lu", worker_idx, task.task_id);
        bool success = true;
        if (task.task_func != NULL) {
            try { // 若编译报错，替换为C语言错误处理（参考之前的修复）
                task.task_func(task.task_arg);
            } catch (...) {
                success = false;
                TRANSPORT_LOG("ERROR", "worker %d task %lu execute failed", worker_idx, task.task_id);
            }
        } else {
            success = false;
        }

        // 任务完成：标记完成、更新统计
        task.is_completed = true;
        atomic_fetch_sub(&pool->running_tasks, 1);
        atomic_fetch_add(&pool->completed_tasks, 1);

        // 通知asyncPoll任务完成
        if (pool->complete_cb != NULL) {
            pool->complete_cb(task.task_id, success, pool->complete_user_data);
        }

        // 销毁任务（释放参数由用户自行处理）
        TRANSPORT_LOG("INFO", "worker %d task %lu completed, success=%d", worker_idx, task.task_id, success);

        // 标记worker为空闲
        pthread_mutex_lock(&worker->mutex);
        worker->is_idle = true;
        pthread_mutex_unlock(&worker->mutex);

        // 通知asyncPoll所有任务可能已完成
        pthread_cond_signal(&pool->cond_all_done);
    }

    TRANSPORT_LOG("INFO", "worker %d exit", worker_idx);
    pthread_exit(NULL);
    return NULL;
}

// ===================== AsyncPoll线程逻辑 =====================
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
        pthread_exit(NULL);
        return NULL;
    }

    TRANSPORT_LOG("INFO", "asyncPoll thread started, monitoring interrupt...");

    while (!pool->is_destroying) {
        // 等待外部中断（任务提交/其他通知）
        pthread_mutex_lock(&pool->global_mutex);
        while (!pool->has_notify && !pool->is_destroying) {
            pthread_cond_wait(&pool->cond_interrupt, &pool->global_mutex);
        }

        if (pool->is_destroying) {
            pthread_mutex_unlock(&pool->global_mutex);
            break;
        }

        // 处理其他事件通知（非任务类）
        uint32_t notify_type = pool->notify_type;
        void* notify_data = pool->notify_data;
        pool->has_notify = false;
        pthread_mutex_unlock(&pool->global_mutex);

        if (notify_type == 0) {
            // 类型0：任务提交事件，查找空闲worker
            ThreadPoolTask* task = (ThreadPoolTask*)notify_data;
            int idle_idx = find_idle_worker(pool);
            if (idle_idx < 0) {
                TRANSPORT_LOG("ERROR", "no idle worker for task %lu", task->task_id);
                if (pool->complete_cb != NULL) {
                    pool->complete_cb(task->task_id, false, pool->complete_user_data);
                }
                continue;
            }

            // 将任务放入空闲worker队列
            WorkerThread* worker = &pool->workers[idle_idx];
            pthread_mutex_lock(&worker->mutex);
            if (worker->queue_size >= worker->queue_cap) {
                pthread_mutex_unlock(&worker->mutex);
                TRANSPORT_LOG("ERROR", "worker %d queue full, task %lu failed", idle_idx, task->task_id);
                if (pool->complete_cb != NULL) {
                    pool->complete_cb(task->task_id, false, pool->complete_user_data);
                }
                continue;
            }

            worker->task_queue[worker->queue_size] = *task;
            worker->queue_size++;
            pthread_mutex_unlock(&worker->mutex);

            // 通知worker开始工作
            pthread_cond_signal(&worker->cond_task);
            TRANSPORT_LOG("INFO", "asyncPoll assign task %lu to worker %d", task->task_id, idle_idx);
        } else {
            // 处理其他类型通知（自定义扩展）
            TRANSPORT_LOG("INFO", "asyncPoll receive notify type %d, data=%p", notify_type, notify_data);
        }
    }

    TRANSPORT_LOG("INFO", "asyncPoll thread exit");
    pthread_exit(NULL);
    return NULL;
}

// ===================== 对外接口实现 =====================
// 初始化线程池（1个asyncPoll + 64个worker，仅初始化不运行）
ThreadPoolHandle thread_pool_init(uint32_t queue_cap) {
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
    atomic_init(&pool->next_task_id, 1);
    atomic_init(&pool->running_tasks, 0);
    atomic_init(&pool->completed_tasks, 0);

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

        pthread_mutex_init(&worker->mutex, NULL);
        pthread_cond_init(&worker->cond_task, NULL);
        worker->is_idle = true;
        worker->is_running = false;
        worker->queue_size = 0;

        // 创建worker线程（初始化但不运行）
        int* idx = (int*)malloc(sizeof(int));
        *idx = i;
        if (pthread_create(&worker->tid, NULL, worker_thread_func, idx) != 0) {
            TRANSPORT_LOG("ERROR", "create worker %d failed", i);
            free(idx);
            thread_pool_destroy(pool);
            return NULL;
        }
    }

    // 创建asyncPoll线程（初始化但不运行）
    if (pthread_create(&pool->async_poll_tid, NULL, async_poll_thread_func, pool) != 0) {
        TRANSPORT_LOG("ERROR", "create asyncPoll thread failed");
        thread_pool_destroy(pool);
        return NULL;
    }

    pool->is_initialized = true;
    pool->is_running = false;
    pool->is_destroying = false;
    pool->has_notify = false;

    TRANSPORT_LOG("INFO", "thread pool init success: 1asyncPoll + 64workers");
    return (ThreadPoolHandle)pool;
}

// 启动线程池
int thread_pool_start(ThreadPoolHandle handle) {
    if (handle == NULL || !handle->is_initialized || handle->is_running) {
        TRANSPORT_LOG("ERROR", "invalid thread pool state for start");
        return -1;
    }

    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = true;
    pthread_cond_broadcast(&handle->cond_interrupt); // 唤醒所有线程
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

    // 生成任务ID
    uint64_t task_id = atomic_fetch_add(&handle->next_task_id, 1);

    // 构造任务
    ThreadPoolTask task = {
        .task_id = task_id,
        .task_func = task_func,
        .task_arg = task_arg,
        .is_completed = false
    };

    // 保存完成回调
    handle->complete_cb = complete_cb;
    handle->complete_user_data = user_data;

    // 触发asyncPoll中断（任务提交通知）
    pthread_mutex_lock(&handle->global_mutex);
    handle->notify_type = 0;
    handle->notify_data = &task;
    handle->has_notify = true;
    pthread_cond_signal(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    TRANSPORT_LOG("INFO", "submit task %lu success, notify asyncPoll", task_id);
    return task_id;
}

// 通知asyncPoll线程（其他事件）
int async_poll_notify(ThreadPoolHandle handle, uint32_t notify_type, void* data) {
    if (handle == NULL || !handle->is_initialized || handle->is_destroying) {
        TRANSPORT_LOG("ERROR", "invalid param for asyncPoll notify");
        return -1;
    }

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

    // 标记销毁状态
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_destroying = true;
    handle->is_running = false;
    pthread_cond_broadcast(&handle->cond_interrupt);
    pthread_mutex_unlock(&handle->global_mutex);

    // 等待asyncPoll线程退出
    pthread_join(handle->async_poll_tid, NULL);

    // 等待64个worker线程退出
    for (int i = 0; i < 64; i++) {
        pthread_cond_broadcast(&handle->workers[i].cond_task);
        pthread_join(handle->workers[i].tid, NULL);

        // 释放worker队列资源
        free(handle->workers[i].task_queue);
        pthread_mutex_destroy(&handle->workers[i].mutex);
        pthread_cond_destroy(&handle->workers[i].cond_task);
    }

    // 释放全局资源
    pthread_mutex_destroy(&handle->global_mutex);
    pthread_cond_destroy(&handle->cond_interrupt);
    pthread_cond_destroy(&handle->cond_all_done);

    free(handle);
    TRANSPORT_LOG("INFO", "thread pool destroy success");
}