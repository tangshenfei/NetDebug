#ifndef ND_NETIO_H
#define ND_NETIO_H

#include "net/addr.h"

#include <stddef.h>

/* 返回值约定：
 *   send_all: 0 成功，-1 失败
 *   recv_some: >0 字节数，0 对端关闭，-1 错误，-2 超时
 */

typedef struct nd_pollfd {
    int   fd;
    short events;
    short revents;
} nd_pollfd;

#define ND_POLLIN  1
#define ND_POLLOUT 2

int  nd_net_socket(int family, int type);
int  nd_net_set_reuse(int fd);
int  nd_net_set_v6only(int fd, int on);
int  nd_net_set_nodelay(int fd);
int  nd_net_set_nonblock(int fd, int on);
int  nd_net_set_timeouts(int fd, int rx_ms, int tx_ms);

int  nd_net_bind(int fd, const nd_addr *a);
int  nd_net_listen(int fd, int backlog);
int  nd_net_accept(int fd, nd_addr *peer, nd_addr *local);
int  nd_net_getsockname(int fd, nd_addr *out);
int  nd_net_getpeername(int fd, nd_addr *out);
int  nd_net_connect(int fd, const nd_addr *a, int timeout_ms, int *sys_err);

int  nd_net_send_all(int fd, const void *data, size_t len, int timeout_ms);
int  nd_net_recv_some(int fd, void *buf, size_t cap, int timeout_ms);
int  nd_net_sendto(int fd, const void *data, size_t len, const nd_addr *to, int timeout_ms);
int  nd_net_recvfrom(int fd, void *buf, size_t cap, nd_addr *peer, int timeout_ms);

int  nd_net_poll(nd_pollfd *fds, size_t n, int timeout_ms);

int  nd_net_shutdown(int fd);
void nd_net_close(int fd);
int  nd_net_last_error(void);

#endif /* ND_NETIO_H */
