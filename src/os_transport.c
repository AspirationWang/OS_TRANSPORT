#include "os_transport_internal.h"
#include "os_transport_thread_pool_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 全局初始化状态
static int g_inited = 0;

static int alloc_task_group(task_group_t **task_group_out, uint64_t task_num, size_t task_arg_size)
{
    task_group_t *task_group = NULL;

    if (!task_group_out || task_num == 0 || task_arg_size == 0) {
        return -1;
    }

    task_group = calloc(1, sizeof(task_group_t));
    if (!task_group) {
        return -1;
    }

    task_group->tasks = calloc(task_num, sizeof(ThreadPoolTask));
    task_group->task_args = calloc(task_num, task_arg_size);
    if (!task_group->tasks || !task_group->task_args) {
        free(task_group->task_args);
        free(task_group->tasks);
        free(task_group);
        return -1;
    }
    task_group->task_num = task_num;

    *task_group_out = task_group;
    return 0;
}

static int init_task_sync(task_sync_t **sync_out)
{
    task_sync_t *sync = NULL;
    int ret;

    if (!sync_out) {
        return -1;
    }

    sync = calloc(1, sizeof(task_sync_t));
    if (!sync) {
        return -1;
    }

    ret = pthread_mutex_init(&sync->mutex, NULL);
    if (ret != 0) {
        free(sync);
        return -1;
    }

    ret = pthread_cond_init(&sync->cond, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&sync->mutex);
        free(sync);
        return -1;
    }

    *sync_out = sync;
    return 0;
}

static void free_task_group_resource(task_sync_t *sync)
{
    task_group_t *task_group;

    if (!sync || !sync->task_group) {
        return;
    }

    task_group = sync->task_group;
    free(task_group->task_args);
    free(task_group->tasks);
    free(task_group);
    sync->task_group = NULL;
}

static void free_sync_owned_resources(task_sync_t *sync)
{
    if (!sync) {
        return;
    }

    free_task_group_resource(sync);
    free(sync->chunks);
    sync->chunks = NULL;
    pthread_mutex_destroy(&sync->mutex);
    pthread_cond_destroy(&sync->cond);
    free(sync);
}

uint32_t wait_for_task_complete(task_sync_t *sync_handle)
{
    if (!sync_handle) {
        return -1;
    }

    pthread_mutex_lock(&sync_handle->mutex);
    while (!sync_handle->request_completed) {
        pthread_cond_wait(&sync_handle->cond, &sync_handle->mutex);
    }
    pthread_mutex_unlock(&sync_handle->mutex);
    if (sync_handle->completed_tasks != sync_handle->total_tasks) {
        return -1;
    }
    return 0;
}

static void mark_task_group_completed(task_sync_t *sync, bool task_success)
{
    if (!sync) {
        return;
    }

    // 如果当前task执行失败，直接标记整个请求完成，唤醒等待线程，并不要求后续task执行完成，避免死锁
    if (!task_success) {
        pthread_mutex_lock(&sync->mutex);
        sync->request_completed = 1;
        pthread_cond_signal(&sync->cond);
        pthread_mutex_unlock(&sync->mutex);
        return;
    }

    // 否则正常更新完成数，等待所有task完成后再唤醒等待线程
    pthread_mutex_lock(&sync->mutex);
    sync->completed_tasks++;
    if (sync->completed_tasks == sync->total_tasks) {
        sync->request_completed = 1;
        pthread_cond_signal(&sync->cond);
    }
    pthread_mutex_unlock(&sync->mutex);
}

// 更新jfc信息并绑定poll线程，确保poll线程能够正确识别和处理事件
static int32_t update_jfc_for_poll(urma_jfce_t *jfce, urma_jfc_t *jfc, bool urma_event_mode,
                                   ThreadPoolHandle pool)
{
    pool->urmaInfo.jfce = jfce;
    pool->urmaInfo.jfc = jfc;
    pool->urmaInfo.urma_event_mode = urma_event_mode;
    return 0;
}

static int validate_send_input(void *handle, struct urma_jetty_info *jetty_info,
                               struct buffer_info *local_src, struct buffer_info *remote_dst,
                               uint32_t len, task_sync_t **ret_sync_handle)
{
    if (!handle || !jetty_info || !local_src || !remote_dst || !ret_sync_handle || len == 0) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }
    return 0;
}

static int validate_recv_input(void *handle, struct buffer_info *host_src,
                               device_info_t *device_dst, uint32_t len,
                               task_sync_t **ret_sync_handle)
{
    if (!handle || !host_src || !device_dst || !ret_sync_handle || len == 0) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }
    return 0;
}

static urma_write_info_t build_write_info(struct urma_jetty_info *jetty_info,
                                          struct buffer_info *local_src,
                                          struct buffer_info *remote_dst, uint32_t server_key,
                                          uint32_t client_key)
{
    urma_write_info_t write_info = {.jfs = jetty_info->jfs,
                                    .jetty = jetty_info->jetty,
                                    .target_jfr = jetty_info->tjetty,
                                    .dst_tseg = remote_dst->tseg,
                                    .src_tseg = local_src->tseg,
                                    .flag.value = 0,
                                    .user_ctx_server = server_key,
                                    .user_ctx_client = client_key};
    return write_info;
}

uint32_t common_split_chunks(uint64_t src_addr, uint64_t dst_addr, uint32_t len,
                             struct chunk_info **ret_chunks, uint64_t *ret_chunk_num)
{
    size_t remain_len = len;
    size_t chunks_num;
    struct chunk_info *chunks;

    chunks_num = (remain_len + DEFAULT_CHUNK_SIZE - 1) / DEFAULT_CHUNK_SIZE;
    chunks = (struct chunk_info *)malloc(sizeof(struct chunk_info) * chunks_num);
    if (!chunks) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }

    for (size_t i = 0; i < chunks_num; i++) {
        chunks[i].src = src_addr + i * DEFAULT_CHUNK_SIZE;
        chunks[i].dst = dst_addr + i * DEFAULT_CHUNK_SIZE;
        chunks[i].len = (remain_len - i * DEFAULT_CHUNK_SIZE) > DEFAULT_CHUNK_SIZE
                            ? DEFAULT_CHUNK_SIZE
                            : (remain_len - i * DEFAULT_CHUNK_SIZE);
    }
    *ret_chunks = chunks;
    *ret_chunk_num = chunks_num;
    return 0;
}

// 发送数据时切分chunk
uint32_t send_split_chunks(struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t len, struct chunk_info **ret_chunks, uint64_t *ret_chunk_num)
{
    uint64_t src_addr;
    uint64_t dst_addr;

    if (!local_src || !remote_dst || len == 0) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    src_addr = local_src->addr;
    dst_addr = remote_dst->addr;
    return common_split_chunks(src_addr, dst_addr, len, ret_chunks, ret_chunk_num);
}

// 接收数据时切分chunk
uint32_t recv_split_chunks(struct buffer_info *host, device_info_t *device, uint32_t len,
                           struct chunk_info **ret_chunks, uint64_t *ret_chunk_num)
{
    uint64_t src_addr;
    uint64_t dst_addr;

    if (!host || !device || len == 0) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    src_addr = host->addr;
    dst_addr = (uint64_t)(uintptr_t)device->dst;
    return common_split_chunks(src_addr, dst_addr, len, ret_chunks, ret_chunk_num);
}

void construct_send_task_arg(send_task_arg_t *arg, urma_write_info_t write_info,
                             struct chunk_info *chunk_info, uint64_t chunk_id, bool is_last_chunk,
                             task_sync_t *sync)
{
    // 显式构造每个位域字段，避免隐式保留旧值
    os_transport_user_data_t user_data_server = {0};
    os_transport_user_data_t user_data_client = {0};

    user_data_server.bs.request_id = write_info.user_ctx_server;   // 将server_key作为request_id传入
    user_data_server.bs.chunk_type = is_last_chunk ? LAST_CHUNK : MIDDLE_CHUNK;
    user_data_server.bs.chunk_id = chunk_id;
    user_data_server.bs.chunk_size = chunk_info->len;

    user_data_client.bs.request_id = write_info.user_ctx_client;   // 将client_key作为request_id传入
    user_data_client.bs.chunk_type = is_last_chunk ? LAST_CHUNK : MIDDLE_CHUNK;
    user_data_client.bs.chunk_id = chunk_id;
    user_data_client.bs.chunk_size = chunk_info->len;

    arg->write_info = write_info;
    arg->write_info.user_ctx_server = user_data_server.user_ctx;
    arg->write_info.user_ctx_client = user_data_client.user_ctx;
    arg->chunk_info = chunk_info;
    arg->is_last_chunk = is_last_chunk;

    // 同组所有task共享一个同步对象，便于主线程等待整组完成
    arg->sync = sync;
}

void construct_recv_task_arg(recv_task_arg_t *arg, urma_recv_info_t recv_info,
                             struct chunk_info *chunk_info, bool is_last_chunk, task_sync_t *sync)
{
    memset(arg, 0, sizeof(*arg));
    arg->recv_info = recv_info;
    arg->chunk_info = chunk_info;
    arg->is_last_chunk = is_last_chunk;
    arg->sync = sync;
}

// 构建供worker取用的task信息
ThreadPoolTask construct_worker_task(uint64_t task_id, uint32_t request_id,
                                     int (*task_func)(void *), void *task_arg)
{
    ThreadPoolTask task;
    memset(&task, 0, sizeof(task));
    task.task_id = task_id;
    task.request_id = request_id;
    task.task_func = task_func;
    task.task_arg = task_arg;
    task.is_completed = false;
    task.free_task_self = false;
    return task;
}

int do_send_chunk_for_worker(urma_write_info_t write_info, struct chunk_info *chunk_info)
{
    return (int)urma_write_with_notify(write_info, chunk_info);
}

int do_recv_chunk_for_worker(urma_recv_info_t recv_info, struct chunk_info *chunk_info)
{
    void *host_buf = (void *)(uintptr_t)chunk_info->src;
    void *device_buf = (void *)(uintptr_t)chunk_info->dst;
    cudaStream_t stream = recv_info.device_info.stream;
    return cudaMemcpyAsync(device_buf, host_buf, chunk_info->len, cudaMemcpyHostToDevice, stream);
}

// worker线程执行的send任务函数，负责发送chunk
int send_task_worker_func(void *arg)
{
    int ret = 0;

    send_task_arg_t *send_task_arg = (send_task_arg_t *)arg;
    ret = do_send_chunk_for_worker(send_task_arg->write_info, send_task_arg->chunk_info);
    mark_task_group_completed(send_task_arg->sync, ret == 0 ? true : false);
    return ret;
}

// worker线程执行的recv任务函数，负责H2D操作
int recv_task_worker_func(void *arg)
{
    int ret = 0;
    recv_task_arg_t *recv_task_arg = (recv_task_arg_t *)arg;
    ret = do_recv_chunk_for_worker(recv_task_arg->recv_info, recv_task_arg->chunk_info);
    mark_task_group_completed(recv_task_arg->sync, ret == 0 ? true : false);
    return ret;
}

static int register_send_tasks(os_transport_handle_t *ost_handle, struct chunk_info *chunks,
                               uint64_t chunk_num, int (*task_func)(void *), urma_info_t urma_info,
                               task_sync_t *sync)
{
    uint64_t task_num = chunk_num - 1;
    uint64_t *task_ids = NULL;
    task_group_t *task_group = NULL;
    send_task_arg_t *task_args = NULL;

    if (chunk_num < 2) {
        fprintf(stderr, "os_transport: chunk数量非法\n");
        return -1;
    }

    if (alloc_task_group(&task_group, task_num, sizeof(send_task_arg_t)) != 0) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    task_args = (send_task_arg_t *)task_group->task_args;

    sync->total_tasks = task_num;
    for (uint64_t i = 0; i < task_num; i++) {
        uint64_t chunk_idx = i + 1;
        bool is_last_chunk = (chunk_idx == chunk_num - 1);
        uint32_t request_id = (uint32_t)(urma_info.write_info.user_ctx_server);

        construct_send_task_arg(&task_args[i],
                                urma_info.write_info,
                                &chunks[chunk_idx],
                                chunk_idx,
                                is_last_chunk,
                                sync);
        task_group->tasks[i] =
            construct_worker_task(chunk_idx, request_id, task_func, &task_args[i]);
    }

    task_ids = thread_pool_submit_batch_tasks(
        ost_handle->thread_pool, task_group->tasks, task_num, NULL, NULL, NULL, NULL);
    if (!task_ids) {
        fprintf(stderr, "os_transport: 任务提交失败\n");
        free(task_group->task_args);
        free(task_group->tasks);
        free(task_group);
        return -1;
    }

    free(task_ids);
    sync->task_group = task_group;
    return 0;
}

static int register_recv_tasks(os_transport_handle_t *ost_handle, struct chunk_info *chunks,
                               uint64_t chunk_num, int (*task_func)(void *), urma_info_t urma_info,
                               task_sync_t *sync)
{
    uint64_t *task_ids = NULL;
    task_group_t *task_group = NULL;
    recv_task_arg_t *task_args = NULL;

    if (alloc_task_group(&task_group, chunk_num, sizeof(recv_task_arg_t)) != 0) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    task_args = (recv_task_arg_t *)task_group->task_args;

    sync->total_tasks = chunk_num;
    for (uint64_t i = 0; i < chunk_num; i++) {
        bool is_last_chunk = (i == chunk_num - 1);
        uint32_t request_id = (uint32_t)(urma_info.recv_info.request_id);
        construct_recv_task_arg(
            &task_args[i], urma_info.recv_info, &chunks[i], is_last_chunk, sync);
        task_group->tasks[i] = construct_worker_task(i, request_id, task_func, &task_args[i]);
    }

    task_ids = thread_pool_submit_batch_tasks(
        ost_handle->thread_pool, task_group->tasks, chunk_num, NULL, NULL, NULL, NULL);
    if (!task_ids) {
        fprintf(stderr, "os_transport: recv任务提交失败\n");
        free(task_group->task_args);
        free(task_group->tasks);
        free(task_group);
        return -1;
    }

    free(task_ids);
    sync->task_group = task_group;
    return 0;
}

// 构造并注册所有task，sync_handle用于与主函数同步
uint32_t construct_and_register_worker_task(os_transport_handle_t *ost_handle,
                                            struct chunk_info *chunks, uint64_t chunk_num,
                                            task_type_t type, int (*task_func)(void *),
                                            urma_info_t urma_info, task_sync_t **sync_handle)
{
    task_sync_t *sync = NULL;
    int ret = -1;

    if (!ost_handle || !chunks || !sync_handle || chunk_num == 0) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    *sync_handle = NULL;

    if (init_task_sync(&sync) != 0) {
        fprintf(stderr, "os_transport: 同步对象初始化失败\n");
        return -1;
    }

    if (type == SEND_TASK) {
        ret = register_send_tasks(ost_handle, chunks, chunk_num, task_func, urma_info, sync);
    } else if (type == RECV_TASK) {
        ret = register_recv_tasks(ost_handle, chunks, chunk_num, task_func, urma_info, sync);
    } else {
        fprintf(stderr, "os_transport: 任务类型错误\n");
        ret = -1;
    }

    if (ret != 0) {
        pthread_mutex_destroy(&sync->mutex);
        pthread_cond_destroy(&sync->cond);
        free(sync);
        return -1;
    }

    *sync_handle = sync;
    return 0;
}

static int register_tasks_and_bind_chunks(os_transport_handle_t *ost_handle,
                                          struct chunk_info *chunks, uint64_t chunk_num,
                                          task_type_t type, int (*task_func)(void *),
                                          urma_info_t urma_info, task_sync_t **sync_handle)
{
    task_sync_t *sync = NULL;
    int ret;

    if (!sync_handle) {
        return -1;
    }

    ret = construct_and_register_worker_task(
        ost_handle, chunks, chunk_num, type, task_func, urma_info, &sync);
    if (ret != 0) {
        return -1;
    }

    sync->chunks = chunks;
    *sync_handle = sync;
    return 0;
}

static int send_single_chunk(struct urma_jetty_info *jetty_info, struct buffer_info *local_src,
                             struct buffer_info *remote_dst, uint32_t len, uint32_t server_key,
                             uint32_t client_key)
{
    urma_write_info_t write_info =
        build_write_info(jetty_info, local_src, remote_dst, server_key, client_key);
    struct chunk_info chunk = {.src = local_src[0].addr, .dst = remote_dst[0].addr, .len = len};
    return (urma_write_with_notify(write_info, &chunk) == URMA_SUCCESS) ? 0 : -1;
}

uint32_t os_transport_reg_jfc(urma_jfce_t *jfce, urma_jfc_t *jfc, void *handle)
{
    os_transport_handle_t *ost_handle;

    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }
    if (!handle) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }

    ost_handle = (os_transport_handle_t *)handle;
    // 初始化完成，poll线程已拉起，更新jfc，绑定poll
    if (update_jfc_for_poll(jfce, jfc, ost_handle->urma_event_mode, ost_handle->thread_pool) != 0) {
        fprintf(stderr, "os_transport: JFC更新失败\n");
        return -1;
    }

    printf("os_transport: JFC注册成功\n");
    return 0;
}

uint32_t os_transport_init(urma_context_t *urma_ctx, os_transport_cfg_t *ost_cfg, void **handle)
{
    os_transport_handle_t *ost_handle;

    if (!ost_cfg || !handle) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    if (g_inited) {
        fprintf(stderr, "os_transport: 已初始化\n");
        return -1;
    }

    ost_handle = malloc(sizeof(os_transport_handle_t));
    if (!ost_handle) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    memset(ost_handle, 0, sizeof(os_transport_handle_t));

    ost_handle->urma_ctx = urma_ctx;
    ost_handle->worker_thread_num = ost_cfg->worker_thread_num;
    ost_handle->urma_event_mode = ost_cfg->urma_event_mode;

    // 初始化线程池
    // worker_queue_cap: 每个Worker的任务队列容量; pending_queue_cap: 0表示使用默认值1024
    ost_handle->thread_pool = thread_pool_init(ost_cfg->worker_thread_num, 0);
    if (!ost_handle->thread_pool) {
        fprintf(stderr, "os_transport: 线程池初始化失败\n");
        free(ost_handle);
        return -1;
    }
    if (thread_pool_start(ost_handle->thread_pool) != 0) {
        fprintf(stderr, "os_transport: 线程池启动失败\n");
        thread_pool_destroy(ost_handle->thread_pool);
        ost_handle->thread_pool = NULL;
        free(ost_handle);
        return -1;
    }

    g_inited = 1;
    // 先置为已初始化，再注册jfc
    if (os_transport_reg_jfc(ost_cfg->jfce, ost_cfg->jfc, (void *)ost_handle) != 0) {
        fprintf(stderr, "os_transport: JFC注册失败\n");
        g_inited = 0;
        thread_pool_destroy(ost_handle->thread_pool);
        ost_handle->thread_pool = NULL;
        free(ost_handle);
        return -1;
    }
    *handle = (void *)ost_handle;
    return 0;
}

/*
 * 发送数据的函数实现
 * 1. 如果数据长度小于等于DEFAULT_CHUNK_SIZE，则直接发送；
 * 2. 如果数据长度大于DEFAULT_CHUNK_SIZE，则拆分为多个chunk，每个chunk的大小不超过DEFAULT_CHUNK_SIZE
 * 3.
 * 将剩余chunk注册为对应task，最后一个chunk使用的回调函数负责唤醒os_transport_send的线程继续执行。
 * 4. 手动发送第一个chunk，触发notify机制，后续chunk的发送由对应的worker线程完成。
 * 5. os_transport_send的线程等待所有chunk发送完成后返回。
 */
uint32_t os_transport_send(void *handle, struct urma_jetty_info *jetty_info,
                           struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t len, uint32_t server_key, uint32_t client_key,
                           task_sync_t **ret_sync_handle)
{
    urma_write_info_t write_info;
    urma_info_t urma_info;
    os_transport_handle_t *ost_handle = (os_transport_handle_t *)handle;
    struct chunk_info *chunks;
    uint64_t chunks_num;
    task_sync_t *sync_handle = NULL;
    uint32_t ret = -1;

    if (ret_sync_handle) {
        *ret_sync_handle = NULL;
    }

    if (validate_send_input(handle, jetty_info, local_src, remote_dst, len, ret_sync_handle) != 0) {
        return ret;
    }

    if (len <= DEFAULT_CHUNK_SIZE) {
        return send_single_chunk(jetty_info, local_src, remote_dst, len, server_key, client_key);
    }

    if (send_split_chunks(local_src, remote_dst, len, &chunks, &chunks_num) != 0) {
        return ret;
    }

    write_info = build_write_info(jetty_info, local_src, remote_dst, server_key, client_key);
    memset(&urma_info, 0, sizeof(urma_info));
    urma_info.write_info = write_info;

    if (register_tasks_and_bind_chunks(ost_handle,
                                       chunks,
                                       chunks_num,
                                       SEND_TASK,
                                       send_task_worker_func,
                                       urma_info,
                                       &sync_handle) != 0) {
        free(chunks);
        return ret;
    }
    *ret_sync_handle = sync_handle;
    if (urma_write_with_notify(write_info, &chunks[0]) != URMA_SUCCESS) {
        // 如果第一个chunk发送失败，应该直接标记整个请求完成，唤醒等待线程，并不要求后续task执行完成，避免死锁
        pthread_mutex_lock(&sync_handle->mutex);
        sync_handle->request_completed = 1;
        pthread_cond_signal(&sync_handle->cond);
        pthread_mutex_unlock(&sync_handle->mutex);
        return -1;
    }

    return 0;
}

uint32_t os_transport_recv(void *handle, struct buffer_info *host_src, device_info_t *device_dst,
                           uint32_t len, uint32_t client_key, task_sync_t **ret_sync_handle)
{
    urma_info_t urma_info = {0};
    os_transport_handle_t *ost_handle = (os_transport_handle_t *)handle;
    struct chunk_info *chunks;
    uint64_t chunks_num;
    task_sync_t *sync_handle = NULL;

    if (ret_sync_handle) {
        *ret_sync_handle = NULL;
    }

    if (validate_recv_input(handle, host_src, device_dst, len, ret_sync_handle) != 0) {
        return -1;
    }

    if (recv_split_chunks(host_src, device_dst, len, &chunks, &chunks_num) != 0) {
        return -1;
    }

    urma_info.recv_info = (urma_recv_info_t){.device_info = *device_dst, .request_id = client_key};

    if (register_tasks_and_bind_chunks(ost_handle,
                                       chunks,
                                       chunks_num,
                                       RECV_TASK,
                                       recv_task_worker_func,
                                       urma_info,
                                       &sync_handle) != 0) {
        free(chunks);
        return -1;
    }

    *ret_sync_handle = sync_handle;
    return 0;
}

uint32_t wait_and_free_sync(void *handle, task_sync_t *sync_handle)
{
    uint32_t completed_success = 0;
    os_transport_handle_t *ost_handle = (os_transport_handle_t *)handle;
    task_group_t *task_group;

    if (!ost_handle || !sync_handle) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    task_group = sync_handle->task_group;
    if (!task_group) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }

    completed_success = wait_for_task_complete(sync_handle);

    if (completed_success != 0) {
        uint32_t request_id = task_group->tasks[0].request_id;
        thread_pool_cancel_tasks_by_req(ost_handle->thread_pool, request_id);
    }
    free_sync_owned_resources(sync_handle);
    return completed_success;
}

uint32_t os_transport_destroy(void *handle)
{
    os_transport_handle_t *ost_handle;

    if (!handle) {
        fprintf(stderr, "os_transport: 参数非法\n");
        return -1;
    }
    ost_handle = (os_transport_handle_t *)handle;
    if (!g_inited) {
        return -1;
    }

    // 销毁线程池
    if (ost_handle->thread_pool) {
        thread_pool_destroy(ost_handle->thread_pool);
        ost_handle->thread_pool = NULL;
    }

    g_inited = 0;
    printf("os_transport: 资源销毁成功\n");
    free(ost_handle);
    return 0;
}
