#include "os_transport.h"
#include <stdio.h>
#include <string.h>

// 全局状态：标记是否初始化
static int g_transport_inited = 0;

int os_transport_init(uint32_t config) {
    if (g_transport_inited) {
        fprintf(stderr, "os_transport: 已初始化，无需重复调用\n");
        return -1;
    }

    // 模拟初始化逻辑（可替换为实际的socket/串口/管道初始化）
    g_transport_inited = 1;
    printf("os_transport: 初始化成功（配置=0x%08x）\n", config);
    return 0;
}

ssize_t os_transport_send(const void *buf, size_t len) {
    if (!g_transport_inited) {
        fprintf(stderr, "os_transport: 未初始化，请先调用os_transport_init\n");
        return -1;
    }
    if (buf == NULL || len == 0) {
        fprintf(stderr, "os_transport: 发送参数无效（buf=NULL或len=0）\n");
        return -1;
    }

    // 模拟发送逻辑（可替换为send/write等系统调用）
    printf("os_transport: 发送 %zu 字节数据\n", len);
    return (ssize_t)len; // 模拟全部发送成功
}

ssize_t os_transport_recv(void *buf, size_t len) {
    if (!g_transport_inited) {
        fprintf(stderr, "os_transport: 未初始化，请先调用os_transport_init\n");
        return -1;
    }
    if (buf == NULL || len == 0) {
        fprintf(stderr, "os_transport: 接收参数无效（buf=NULL或len=0）\n");
        return -1;
    }

    // 模拟接收逻辑（可替换为recv/read等系统调用）
    memset(buf, 0xAA, len); // 填充测试数据
    printf("os_transport: 接收 %zu 字节数据\n", len);
    return (ssize_t)len; // 模拟全部接收成功
}

void os_transport_destroy(void) {
    if (!g_transport_inited) {
        return;
    }

    // 模拟资源释放逻辑
    g_transport_inited = 0;
    printf("os_transport: 资源销毁成功\n");
}