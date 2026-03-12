#include "os_transport.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// 全局初始化状态
static int g_inited = 0;
static ThreadPoolHandle g_thread_pool;

uint32_t os_transport_init(urma_context_t* urma_ctx, os_transport_cfg_t* ost_cfg, void** handle)
{
    if (g_inited) {
        fprintf(stderr, "os_transport: 已初始化\n");
        return -1;
    }
    os_transport_handle_t *ost_handle = malloc(sizeof(os_transport_handle_t));
    if (!ost_handle) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    memset(ost_handle, 0, sizeof(os_transport_handle_t));
    
    ost_handle->urma_ctx = urma_ctx;
    ost_handle->ost_cfg = ost_cfg;
    

    // 初始化线程池
    // worker_queue_cap: 每个Worker的任务队列容量; pending_queue_cap: 0表示使用默认值1024
    ost_handle->thread_pool = thread_pool_init(ost_cfg->worker_thread_num, 0);
    if (!ost_handle->thread_pool) {
        fprintf(stderr, "os_transport: 线程池初始化失败\n");
        return -1;
    }
    if (thread_pool_start(ost_handle->thread_pool) != 0) {
        fprintf(stderr, "os_transport: 线程池启动失败\n");
        thread_pool_destroy(ost_handle->thread_pool);
        ost_handle->thread_pool = NULL;
        return -1;
    }

    *handle = (void*)ost_handle;
    g_inited = 1;

    return 0;
}

static int32_t update_jfc_for_poll(urma_jfce_t* jfce, urma_jfc_t* jfc, os_transport_handle_t* ost_handle)
{
    // 根据实际情况更新jfc的相关信息，以便poll线程能够正确处理事件
    return 0;
}

uint32_t os_transport_reg_jfc(urma_jfce_t* jfce, urma_jfc_t* jfc, void* handle)
{
    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }
    os_transport_handle_t *ost_handle = (os_transport_handle_t*)handle;
    // 初始化完成，poll线程已拉起，更新jfc，绑定poll
    update_jfc_for_poll(jfce, jfc, ost_handle);

    printf("os_transport: JFC注册成功\n");
    return 0;
}

urma_status_t urma_write_with_notify(urma_jfs_t* jfs, urma_target_jetty_t* target_jfr,
                                     urma_target_seg_t* dst_tseg, urma_target_seg_t* src_tseg,
                                     uint64_t dst, uint64_t src, uint32_t len,
                                     urma_jfs_wr_flag_t flag, uint64_t user_ctx)
{
    return URMA_SUCCESS;
}

// 构造send任务的函数参数
send_task_arg_t* construct_send_task_arg(urma_write_info_t write_info, struct chunk_info *chunk_info, uint64_t chunk_id,
                                         bool is_last_chunk, task_sync_t* sync)
{
    send_task_arg_t* arg = malloc(sizeof(send_task_arg_t));
    if (!arg) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return NULL;
    }
    // 先获取request_id等信息构造user_ctx，后续根据chunk_id等信息更新user_ctx的具体内容
    os_transport_user_data_t user_data = {0};
    user_data.user_ctx = write_info.user_ctx;
    user_data.bs.chunk_type = is_last_chunk ? LAST_CHUNK : MIDDLE_CHUNK;
    user_data.bs.chunk_id = chunk_id;
    user_data.bs.chunk_size = chunk_info->len;

    arg->write_info = write_info;
    arg->write_info.user_ctx = user_data.user_ctx;
    arg->chunk_info = chunk_info;
    arg->is_last_chunk = is_last_chunk;

    if (is_last_chunk) {
        // 最后一个chunk的task需要负责唤醒os_transport_send的线程继续执行，因此需要初始化同步信息
        arg->sync = sync;
        pthread_mutex_init(&arg->sync->mutex, NULL);
        pthread_cond_init(&arg->sync->cond, NULL);
        arg->sync->request_completed = 0;
    } else {
        // 非最后一个chunk的task不需要同步信息
        arg->sync = NULL;
    }
    return arg;
}

// 构建供worker取用的task信息
ThreadPoolTask construct_worker_task(uint64_t task_id, void (*task_func)(void*),
                                     void* task_arg)
{
    ThreadPoolTask task;
    task.task_id = task_id;
    task.task_func = task_func;
    task.task_arg = task_arg;
    task.is_completed = false;
    return task;
}

// 构造并注册所有task，sync_handle用于与主函数同步
uint32_t construct_and_register_worker_task(os_transport_handle_t *ost_handle,
                                            struct chunk_info *chunks, uint64_t chunk_num,
                                            task_type_t type, void (*task_func)(void *),
                                            task_sync_t **sync_handle, urma_info_t urma_info)
{
    task_sync_t* sync;
    sync = malloc(sizeof(task_sync_t));
    if (!sync) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    *sync_handle = sync;
    if (type == SEND_TASK) {
        ThreadPoolTask* tasks = malloc(chunk_num * sizeof(ThreadPoolTask));
        // 从chunk[1]开始构造send类型的task并注册到线程池，chunk[0]由主线程发送
        for (uint64_t i = 1; i < chunk_num; i++) {
            if (!tasks) {
                fprintf(stderr, "os_transport: 内存分配失败\n");
                return -1;
            }
            // 这里需要根据实际情况构造send_task_arg，例如：write_info，chunk_id，is_last_chunk和sync等参数
            bool is_last_chunk = (i == chunk_num - 1) ? true : false;
            send_task_arg_t *send_task_arg =
                construct_send_task_arg(urma_info.write_info, &chunks[i], i, is_last_chunk, sync);
            if (!send_task_arg) {
                fprintf(stderr, "os_transport: 任务参数构造失败\n");
                // 这里可以根据实际情况选择返回一个错误的task或者直接退出
                return -1;
            }
            tasks[i] = construct_worker_task(i, task_func, send_task_arg);
        }
        // 批量提交任务到线程池
        thread_pool_submit_batch_tasks(ost_handle->thread_pool, tasks, chunk_num - 1, NULL, NULL);
    } else if (type == RECV_TASK) {
        // 构造recv类型的task，类似于send类型的task
    } else {
        fprintf(stderr, "os_transport: 任务类型错误\n");
        return -1;
    }

    return 0;
}

void do_send_chunk_for_worker(urma_write_info_t write_info)
{
    // 这里可以调用实际的发送函数来发送数据，例如：
    // urma_write_with_notify(g_urma_ctx, jetty_info, remote_dst, local_src, dst, src,
    // local_src[0].len, flag, user_ctx);
}

// worker线程执行的任务函数，负责发送chunk
void send_task_worker_func(void* arg)
{
    send_task_arg_t* send_task_arg = (send_task_arg_t*)arg;
    do_send_chunk_for_worker(send_task_arg->write_info);
    // 如果是最后一个分片，则唤醒os_transport_send的线程继续执行
    if (!send_task_arg->is_last_chunk) {
        free(send_task_arg);
        return;
    }
    task_sync_t* sync = send_task_arg->sync;
    pthread_mutex_lock(&sync->mutex);
    sync->request_completed = 1;
    pthread_cond_signal(&sync->cond);
    pthread_mutex_unlock(&sync->mutex);
    free(send_task_arg);
}

// 切分chunk的函数，负责将数据切分为多个chunk
// 返回切分后的chunk数量，retchunks用于返回切分后的chunk数组
uint64_t split_chunks(struct buffer_info* local_src, struct buffer_info* remote_dst, uint32_t len, struct chunk_info** retchunks)
{
    size_t total_len = len;
    struct chunk_info *chunks;
    size_t chunks_num = (total_len + DEFAULT_CHUNK_SIZE - 1) / DEFAULT_CHUNK_SIZE;
    chunks = (struct chunk_info *)malloc(sizeof(struct chunk_info) * chunks_num);
    if (!chunks) {
        fprintf(stderr, "os_transport: 内存分配失败\n");
        return -1;
    }
    for (size_t i = 0; i < chunks_num; i++) {
        chunks[i].src = local_src[0].addr + i * DEFAULT_CHUNK_SIZE;
        chunks[i].dst = remote_dst[0].addr + i * DEFAULT_CHUNK_SIZE;
        chunks[i].len = (total_len - i * DEFAULT_CHUNK_SIZE) > DEFAULT_CHUNK_SIZE
                            ? DEFAULT_CHUNK_SIZE
                            : (total_len - i * DEFAULT_CHUNK_SIZE);
    }
    *retchunks = chunks;
    return chunks_num;
}


void wait_for_task_complete(task_sync_t* sync_handle)
{
    pthread_mutex_lock(&sync_handle->mutex);
    while (!sync_handle->request_completed) {
        pthread_cond_wait(&sync_handle->cond, &sync_handle->mutex);
    }
    pthread_mutex_unlock(&sync_handle->mutex);
}

/*
 * 发送数据的函数实现
 * 1. 如果数据长度小于等于DEFAULT_CHUNK_SIZE，则直接发送；
 * 2. 如果数据长度大于DEFAULT_CHUNK_SIZE，则拆分为多个chunk，每个chunk的大小不超过DEFAULT_CHUNK_SIZE
 * 3. 将剩余chunk注册为对应task，最后一个chunk使用的回调函数负责唤醒os_transport_send的线程继续执行。
 * 4. 手动发送第一个chunk，触发notify机制，后续chunk的发送由对应的worker线程完成。
 * 5. os_transport_send的线程等待所有chunk发送完成后返回。
 */
uint32_t os_transport_send(void *handle, struct urma_jetty_info *jetty_info,
                           struct buffer_info *local_src, struct buffer_info *remote_dst,
                           uint32_t len, uint32_t request_key)
{
    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }

    os_transport_handle_t *ost_handle = (os_transport_handle_t *)handle;
    // 若源数据长度小于等于DEFAULT_CHUNK_SIZE，则直接发送；否则需要将数据拆分为多个chunk进行发送
    if (len <= DEFAULT_CHUNK_SIZE) {
        // 这里可以调用实际的发送函数来发送数据，例如：
        // urma_write_with_notify(g_urma_ctx, jetty_info, remote_dst, local_src, dst, src,
        // local_src[0].len, flag, user_ctx);
        return 0;
    }

    // 将源地址中的数据拆分为多个chunk，每个chunk的大小不超过DEFAULT_CHUNK_SIZE
    struct chunk_info* chunks;
    uint64_t chunks_num = split_chunks(local_src, remote_dst, len, &chunks);
    if (chunks_num == (uint64_t)-1) {
        return -1;
    }

    urma_info_t urma_info;
    // 构造send类型的task并注册，注意最后一个chunk的task需要负责唤醒os_transport_send的线程继续执行
    urma_info.write_info = (urma_write_info_t){
        .jfs = jetty_info->jfs,
        .target_jfr = jetty_info->tjetty,
        .dst_tseg = &remote_dst->tseg,
        .src_tseg = &local_src->tseg,
        .flag = 0, // 根据实际情况设置flag
        .user_ctx = request_key // 可以将request_key作为user_ctx传入，后续在每个chunk中分别设置具体信息
    };

    // 构造task并注册，注意最后一个chunk的task需要负责唤醒os_transport_send的线程继续执行
    task_sync_t* sync_handle;
    construct_and_register_worker_task(
        ost_handle, chunks, chunks_num, SEND_TASK, send_task_worker_func, &sync_handle, urma_info);

    // 手动发送第一个chunk
    // urma_write_with_notify(g_urma_ctx, jetty_info, remote_dst, local_src, dst, src,
    // local_src[0].len, flag, user_ctx);

    wait_for_task_complete(sync_handle);

    free(chunks);
    pthread_mutex_destroy(&sync_handle->mutex);
    pthread_cond_destroy(&sync_handle->cond);
    free(sync_handle);
    return 0;
}

void do_recv_chunk_for_worker(recv_info_t recv_info)
{
    // 这里可以调用实际的H2D传输函数来接收数据
}

// worker线程执行的任务函数，负责接收chunk（H2D操作）
void recv_task_worker_func(void* arg)
{
    recv_task_arg_t* recv_task_arg = (recv_task_arg_t*)arg;
    do_recv_chunk_for_worker(recv_task_arg->recv_info);
    // 如果是最后一个分片，则唤醒os_transport_recv的线程继续执行
    if (!recv_task_arg->is_last_chunk) {
        free(recv_task_arg);
        return;
    }
    task_sync_t* sync = recv_task_arg->sync;
    pthread_mutex_lock(&sync->mutex);
    sync->request_completed = 1;
    pthread_cond_signal(&sync->cond);
    pthread_mutex_unlock(&sync->mutex);
    free(recv_task_arg);
}

/*
 * 接收数据的函数实现
 * 1. 接收函数的主要操作是构建和注册task，task的执行函数负责在被poll线程唤醒后执行H2D操作。
 * 2. 等待所有task完成后返回。
 */
uint32_t os_transport_recv(void* handle, struct buffer_info* host_src,
                           struct buffer_info* device_dst, uint32_t buffer_num, uint32_t client_key)
{
    if (!g_inited) {
        fprintf(stderr, "os_transport: 未初始化\n");
        return -1;
    }
    if (buffer_num != 1) {
        fprintf(stderr, "os_transport: 目前仅支持1个buffer的接收\n");
        return -1;
    }

    // 将host源数据拆分为多个chunk
    struct buffer_info* chunks;
    uint64_t chunks_num = split_chunks(host_src, len, &chunks);
    if (chunks_num == (uint64_t)-1) {
        return -1;
    }

    // 构造recv类型的task并注册
    task_sync_t* sync_handle;
    construct_and_register_worker_task(chunks_num, RECV_TASK, recv_task_worker_func, &sync_handle);
    // 等待所有task完成后返回
    wait_for_task_complete(sync_handle);

    free(chunks);
    pthread_mutex_destroy(&sync_handle->mutex);
    pthread_cond_destroy(&sync_handle->cond);
    free(sync_handle);
    return 0;
}

uint32_t os_transport_destroy(void* handle)
{
    os_transport_handle_t *ost_handle = (os_transport_handle_t*)handle;
    if (!g_inited) return -1;

    // 销毁线程池
    if (ost_handle->thread_pool) {
        thread_pool_destroy(ost_handle->thread_pool);
        ost_handle->thread_pool = NULL;
    }

    g_inited = 0;
    printf("os_transport: 资源销毁成功\n");
    return 0;
}