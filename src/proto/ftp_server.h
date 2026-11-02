#ifndef ND_FTP_SERVER_H
#define ND_FTP_SERVER_H

#include "net/addr.h"

#include <stdint.h>

typedef struct {
    char     root[512];
    int      readonly;
    int      anonymous;
    char     user[64];
    char     pass[64];
    uint16_t pasv_min;
    uint16_t pasv_max;
    char     advertise[64];
} nd_ftp_srv_cfg;

/* 接管 fd，创建独立线程处理控制连接；返回 0 成功 */
int nd_ftp_server_start(int fd, const nd_addr *peer, const nd_addr *local,
                        const nd_ftp_srv_cfg *cfg);

#endif
