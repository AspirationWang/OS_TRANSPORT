#include "os_transport_thread_pool_internal.h"
#include "os_transport_thread_pool.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

// 初始化单个任务队列
static int task_queue_init(TaskQueue* queue, uint32_t cap) {
    if (queue == NULL || cap == 0) {
        return -1;
    }

    queue->tasks = (ThreadPoolTask*)calloc(cap, sizeof(ThreadPoolTask));
    if (queue->tasks == NULL) {
        TRANSPORT_LOG("ERROR", "malloc task queue failed, cap=%d", cap);
        return -1;
    }

    queue->cap = cap;
    queue->size = 0;
    queue->head = 0;
    queue->tail = 0;

    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&queue->mutex, NULL) != 0 ||
        pthread_cond_init(&queue->cond_not_empty, NULL) != 0 ||
        pthread_cond_init(&queue->cond_not_full, NULL) != 0) {
        TRANSPORT_LOG("ERROR", "init mutex/cond failed, errno=%d", errno);
        free(queue->tasks);
        return -1;
    }

    return 0;
}

// 销毁单个任务队列
static void task_queue_destroy(TaskQueue* queue) {
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    // 释放任务数组
    if (queue->tasks != NULL) {
        free(queue->tasks);
        queue->tasks = NULL;
    }
    pthread_mutex_unlock(&queue->mutex);

    // 销毁锁和条件变量
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond_not_empty);
    pthread_cond_destroy(&queue->cond_not_full);

    memset(queue, 0, sizeof(TaskQueue));
}

// 向队列中添加任务（内部函数，已加锁）
static int task_queue_push(TaskQueue* queue, const ThreadPoolTask* task) {
    if (queue->size >= queue->cap) {
        return -1; // 队列满
    }

    memcpy(&queue->tasks[queue->tail], task, sizeof(ThreadPoolTask));
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;

    // 通知等待的线程：队列非空
    pthread_cond_signal(&queue->cond_not_empty);
    return 0;
}

// 从队列中取出任务（内部函数，队列为空时阻塞）
static int task_queue_pop(TaskQueue* queue, ThreadPoolTask* task) {
    pthread_mutex_lock(&queue->mutex);

    // 队列为空时，阻塞等待
    while (queue->size == 0) {
        pthread_cond_wait(&queue->cond_not_empty, &queue->mutex);
    }

    // 取出队头任务
    memcpy(task, &queue->tasks[queue->head], sizeof(ThreadPoolTask));
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;

    // 通知等待的提交任务：队列非满
    pthread_cond_signal(&queue->cond_not_full);
    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

// 处理缓存队列（核心：队列空闲时，将缓存任务移入正式队列）
void process_cache_queue(ThreadPoolHandle handle, ThreadType type) {
    if (handle == NULL || type >= THREAD_TYPE_MAX) {
        return;
    }

    TaskQueue* formal_queue = &handle->task_queues[type];
    TaskQueue* cache_queue = &handle->task_cache_queues[type];

    pthread_mutex_lock(&cache_queue->mutex);
    // 缓存队列为空，直接返回
    if (cache_queue->size == 0) {
        pthread_mutex_unlock(&cache_queue->mutex);
        return;
    }

    // 尝试将缓存任务移入正式队列
    pthread_mutex_lock(&formal_queue->mutex);
    while (cache_queue->size > 0 && formal_queue->size < formal_queue->cap) {
        ThreadPoolTask task;
        // 从缓存队列取任务
        memcpy(&task, &cache_queue->tasks[cache_queue->head], sizeof(ThreadPoolTask));
        cache_queue->head = (cache_queue->head + 1) % cache_queue->cap;
        cache_queue->size--;

        // 移入正式队列
        memcpy(&formal_queue->tasks[formal_queue->tail], &task, sizeof(ThreadPoolTask));
        formal_queue->tail = (formal_queue->tail + 1) % formal_queue->cap;
        formal_queue->size++;

        // 通知正式队列的线程：有新任务
        pthread_cond_signal(&formal_queue->cond_not_empty);
    }
    pthread_mutex_unlock(&formal_queue->mutex);
    pthread_mutex_unlock(&cache_queue->mutex);
}

// 线程工作函数（不同类型线程共用，根据type消费对应队列）
static void* thread_worker_func(void* arg) {
    ThreadPoolHandle handle = (ThreadPoolHandle)arg;
    ThreadType type = (ThreadType)(uintptr_t)pthread_getspecific(pthread_key_create(NULL, NULL));

    if (type >= THREAD_TYPE_MAX) {
        TRANSPORT_LOG("ERROR", "invalid thread type: %d", type);
        pthread_exit(NULL);
        return NULL;
    }

    ThreadPoolTask task;
    while (1) {
        // 先处理缓存队列（将缓存任务移入正式队列）
        process_cache_queue(handle, type);

        // 从正式队列取任务（阻塞）
        task_queue_pop(&handle->task_queues[type], &task);

        // 线程池已停止，退出
        pthread_mutex_lock(&handle->global_mutex);
        if (!handle->is_running) {
            pthread_mutex_unlock(&handle->global_mutex);
            break;
        }
        pthread_mutex_unlock(&handle->global_mutex);

        // 执行任务回调
        if (task.callback != NULL) {
            task.callback(task.arg);
            task.is_processed = 1;
        }
    }

    TRANSPORT_LOG("INFO", "thread type %d exit", type);
    pthread_exit(NULL);
    return NULL;
}

// 初始化线程池
ThreadPoolHandle thread_pool_init(uint32_t thread_nums[THREAD_TYPE_MAX], uint32_t queue_cap) {
    // 1. 分配线程池内存
    ThreadPoolHandle handle = (ThreadPoolHandle)calloc(1, sizeof(struct _ThreadPool));
    if (handle == NULL) {
        TRANSPORT_LOG("ERROR", "malloc thread pool failed");
        return NULL;
    }

    // 2. 初始化全局锁
    if (pthread_mutex_init(&handle->global_mutex, NULL) != 0) {
        TRANSPORT_LOG("ERROR", "init global mutex failed");
        free(handle);
        return NULL;
    }

    // 3. 标记线程池运行状态
    handle->is_running = true;

    // 4. 初始化每种线程的正式队列 + 缓存队列
    for (int i = 0; i < THREAD_TYPE_MAX; i++) {
        // 正式队列容量=queue_cap，缓存队列容量=queue_cap*2（避免缓存溢出）
        if (task_queue_init(&handle->task_queues[i], queue_cap) != 0 ||
            task_queue_init(&handle->task_cache_queues[i], queue_cap * 2) != 0) {
            TRANSPORT_LOG("ERROR", "init task queue for type %d failed", i);
            thread_pool_destroy(handle);
            return NULL;
        }
        handle->thread_nums[i] = thread_nums[i];
    }

    // 5. 创建每种类型的线程
    for (int type = 0; type < THREAD_TYPE_MAX; type++) {
        for (int i = 0; i < handle->thread_nums[type]; i++) {
            // 设置线程私有数据（标记线程类型）
            pthread_key_t key;
            pthread_key_create(&key, NULL);
            pthread_setspecific(key, (void*)(uintptr_t)type);

            // 创建线程
            if (pthread_create(&handle->threads[type][i], NULL, thread_worker_func, handle) != 0) {
                TRANSPORT_LOG("ERROR", "create thread type %d, num %d failed", type, i);
                thread_pool_destroy(handle);
                return NULL;
            }
            TRANSPORT_LOG("INFO", "create thread type %d, num %d success", type, i);
        }
    }

    return handle;
}

// 提交任务（核心：队列满时存入缓存，不丢弃）
int thread_pool_submit_task(ThreadPoolHandle handle, ThreadType type,
                            ThreadPoolTaskCallback callback, void* arg) {
    if (handle == NULL || type >= THREAD_TYPE_MAX || callback == NULL) {
        TRANSPORT_LOG("ERROR", "invalid param for submit task");
        return -1;
    }

    pthread_mutex_lock(&handle->global_mutex);
    if (!handle->is_running) {
        pthread_mutex_unlock(&handle->global_mutex);
        TRANSPORT_LOG("ERROR", "thread pool is not running");
        return -1;
    }
    pthread_mutex_unlock(&handle->global_mutex);

    // 构造任务
    ThreadPoolTask task = {
        .type = type,
        .callback = callback,
        .arg = arg,
        .is_processed = 0
    };

    TaskQueue* formal_queue = &handle->task_queues[type];
    TaskQueue* cache_queue = &handle->task_cache_queues[type];

    // 第一步：尝试提交到正式队列
    pthread_mutex_lock(&formal_queue->mutex);
    int ret = task_queue_push(formal_queue, &task);
    if (ret == 0) {
        pthread_mutex_unlock(&formal_queue->mutex);
        TRANSPORT_LOG("INFO", "submit task to formal queue type %d success", type);
        return 0;
    }
    pthread_mutex_unlock(&formal_queue->mutex);

    // 第二步：正式队列满，提交到缓存队列
    pthread_mutex_lock(&cache_queue->mutex);
    ret = task_queue_push(cache_queue, &task);
    if (ret != 0) {
        pthread_mutex_unlock(&cache_queue->mutex);
        TRANSPORT_LOG("ERROR", "cache queue type %d is full, task submit failed (this should not happen!)", type);
        return -1;
    }
    pthread_mutex_unlock(&cache_queue->mutex);
    TRANSPORT_LOG("INFO", "formal queue type %d full, task cached", type);

    return 0;
}

// 销毁线程池（处理完所有任务后退出）
void thread_pool_destroy(ThreadPoolHandle handle) {
    if (handle == NULL) {
        return;
    }

    // 1. 标记线程池停止运行
    pthread_mutex_lock(&handle->global_mutex);
    handle->is_running = false;
    pthread_mutex_unlock(&handle->global_mutex);

    // 2. 唤醒所有阻塞的线程（让线程退出）
    for (int type = 0; type < THREAD_TYPE_MAX; type++) {
        pthread_mutex_lock(&handle->task_queues[type].mutex);
        pthread_cond_broadcast(&handle->task_queues[type].cond_not_empty);
        pthread_mutex_unlock(&handle->task_queues[type].mutex);

        // 等待所有线程退出
        for (int i = 0; i < handle->thread_nums[type]; i++) {
            pthread_join(handle->threads[type][i], NULL);
        }
    }

    // 3. 销毁所有任务队列（正式+缓存）
    for (int type = 0; type < THREAD_TYPE_MAX; type++) {
        task_queue_destroy(&handle->task_queues[type]);
        task_queue_destroy(&handle->task_cache_queues[type]);
    }

    // 4. 销毁全局锁
    pthread_mutex_destroy(&handle->global_mutex);

    // 5. 释放线程池内存
    free(handle);
    TRANSPORT_LOG("INFO", "thread pool destroyed");
}