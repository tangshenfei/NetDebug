#ifndef ND_TFTP_SERVER_H
#define ND_TFTP_SERVER_H

#include "net/addr.h"

#include <stddef.h>

typedef struct {
    char root[512];
    int  readonly;
    int  timeout_s;
} nd_tftp_cfg;

/* 处理一个 RRQ/WRQ 请求：内部创建独立线程和传输套接字，立即返回 */
int nd_tftp_server_start(int listen_fd, const nd_addr *local, const nd_addr *peer,
                         const unsigned char *pkt, size_t len, const nd_tftp_cfg *cfg);

#endif
