#ifndef OS_TRANSPORT_H
#define OS_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * @brief 传输层初始化函数
 * @param config 配置参数（0=默认配置）
 * @return 0=成功，非0=失败
 */
int os_transport_init(uint32_t config);

/**
 * @brief 发送数据
 * @param buf 数据缓冲区（非NULL）
 * @param len 数据长度（>0）
 * @return 实际发送字节数，-1=失败
 */
ssize_t os_transport_send(const void *buf, size_t len);

/**
 * @brief 接收数据
 * @param buf 接收缓冲区（非NULL）
 * @param len 缓冲区长度（>0）
 * @return 实际接收字节数，-1=失败
 */
ssize_t os_transport_recv(void *buf, size_t len);

/**
 * @brief 销毁传输层资源
 */
void os_transport_destroy(void);

#endif // OS_TRANSPORT_H