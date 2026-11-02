#include "platform/netio.h"
#include "core/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define ND_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define ND_TIMEOUT(e)   ((e) == WSAETIMEDOUT || (e) == WSAEWOULDBLOCK)
#else
#define ND_WOULDBLOCK(e) ((e) == EINPROGRESS || (e) == EWOULDBLOCK || (e) == EAGAIN)
#define ND_TIMEOUT(e)   ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == ETIMEDOUT)
#endif

int nd_net_socket(int family, int type)
{
#ifdef _WIN32
    SOCKET s = socket(family, type, 0);
    if (s == INVALID_SOCKET)
        return -1;
    return (int)s;
#else
    return socket(family, type, 0);
#endif
}

int nd_net_set_reuse(int fd)
{
    int one = 1;
#ifdef _WIN32
    return setsockopt((SOCKET)fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

int nd_net_set_v6only(int fd, int on)
{
    int v = on ? 1 : 0;
    return setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v, sizeof(v));
}

int nd_net_set_nodelay(int fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

int nd_net_set_nonblock(int fd, int on)
{
#ifdef _WIN32
    u_long v = on ? 1 : 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &v);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (on)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
#endif
}

int nd_net_set_timeouts(int fd, int rx_ms, int tx_ms)
{
#ifdef _WIN32
    DWORD tv;
    if (rx_ms > 0) {
        tv = (DWORD)rx_ms;
        if (setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv)) < 0)
            return -1;
    }
    if (tx_ms > 0) {
        tv = (DWORD)tx_ms;
        if (setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv)) < 0)
            return -1;
    }
#else
    struct timeval tv;
    if (rx_ms > 0) {
        tv.tv_sec = rx_ms / 1000;
        tv.tv_usec = (rx_ms % 1000) * 1000;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            return -1;
    }
    if (tx_ms > 0) {
        tv.tv_sec = tx_ms / 1000;
        tv.tv_usec = (tx_ms % 1000) * 1000;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
            return -1;
    }
#endif
    return 0;
}

int nd_net_bind(int fd, const nd_addr *a)
{
    struct sockaddr_storage ss;
    int salen = 0;
    if (nd_addr_to_sockaddr(a, &ss, &salen) != 0)
        return -1;
    return bind(fd, (const struct sockaddr *)&ss, (socklen_t)salen);
}

int nd_net_listen(int fd, int backlog)
{
    return listen(fd, backlog);
}

int nd_net_accept(int fd, nd_addr *peer, nd_addr *local)
{
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    int c;
#ifdef _WIN32
    c = (int)accept((SOCKET)fd, (struct sockaddr *)&ss, &slen);
#else
    c = accept(fd, (struct sockaddr *)&ss, &slen);
#endif
    if (c < 0)
        return -1;
    nd_net_set_nonblock(c, 0); /* 监听套接字是非阻塞的，连接必须恢复阻塞 */
    if (peer)
        nd_addr_from_sockaddr(peer, (const struct sockaddr *)&ss, (int)slen);
    if (local)
        nd_net_getsockname(c, local);
    nd_net_set_nodelay(c);
    return c;
}

int nd_net_getsockname(int fd, nd_addr *out)
{
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0)
        return -1;
    return nd_addr_from_sockaddr(out, (const struct sockaddr *)&ss, (int)slen);
}

int nd_net_getpeername(int fd, nd_addr *out)
{
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &slen) != 0)
        return -1;
    return nd_addr_from_sockaddr(out, (const struct sockaddr *)&ss, (int)slen);
}

int nd_net_connect(int fd, const nd_addr *a, int timeout_ms, int *sys_err)
{
    struct sockaddr_storage ss;
    int salen = 0;
    int rc;

    if (nd_addr_to_sockaddr(a, &ss, &salen) != 0)
        return -1;

    if (nd_net_set_nonblock(fd, 1) != 0)
        return -1;

    rc = connect(fd, (const struct sockaddr *)&ss, (socklen_t)salen);
    if (rc != 0) {
        int e = nd_net_last_error();
        if (!ND_WOULDBLOCK(e)) {
            if (sys_err)
                *sys_err = e;
            return -1;
        }
    } else {
        nd_net_set_nonblock(fd, 0);
        return 0;
    }

    {
        nd_pollfd p;
        p.fd = fd;
        p.events = ND_POLLOUT;
        p.revents = 0;
        rc = nd_net_poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            if (sys_err)
                *sys_err = rc == 0 ? 0 : nd_net_last_error();
            return -1;
        }
        {
            int err = 0;
            socklen_t elen = (socklen_t)sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err != 0) {
                if (sys_err)
                    *sys_err = err ? err : nd_net_last_error();
                return -1;
            }
        }
    }

    nd_net_set_nonblock(fd, 0);
    return 0;
}

int nd_net_send_all(int fd, const void *data, size_t len, int timeout_ms)
{
    const char *p = (const char *)data;
    size_t sent = 0;
    (void)timeout_ms;
    while (sent < len) {
        int n = send(fd, p + sent, (int)(len - sent), 0);
        if (n <= 0) {
            int e = nd_net_last_error();
            if (ND_TIMEOUT(e))
                return -1;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int nd_net_recv_some(int fd, void *buf, size_t cap, int timeout_ms)
{
    int n;
    (void)timeout_ms;
#ifdef _WIN32
    n = recv((SOCKET)fd, (char *)buf, (int)cap, 0);
#else
    n = (int)recv(fd, buf, cap, 0);
#endif
    if (n > 0)
        return n;
    if (n == 0)
        return 0;
    {
        int e = nd_net_last_error();
        if (ND_TIMEOUT(e))
            return -2;
    }
    return -1;
}

int nd_net_sendto(int fd, const void *data, size_t len, const nd_addr *to, int timeout_ms)
{
    struct sockaddr_storage ss;
    int salen = 0;
    int n;
    (void)timeout_ms;
    if (nd_addr_to_sockaddr(to, &ss, &salen) != 0)
        return -1;
    n = (int)sendto(fd, (const char *)data, (int)len, 0,
                    (const struct sockaddr *)&ss, (socklen_t)salen);
    return n == (int)len ? 0 : -1;
}

int nd_net_recvfrom(int fd, void *buf, size_t cap, nd_addr *peer, int timeout_ms)
{
    struct sockaddr_storage ss;
    socklen_t slen = (socklen_t)sizeof(ss);
    int n;
    (void)timeout_ms;
    n = (int)recvfrom(fd, (char *)buf, (int)cap, 0,
                      (struct sockaddr *)&ss, &slen);
    if (n >= 0) {
        if (peer)
            nd_addr_from_sockaddr(peer, (const struct sockaddr *)&ss, (int)slen);
        return n;
    }
    {
        int e = nd_net_last_error();
        if (ND_TIMEOUT(e))
            return -2;
    }
    return -1;
}

int nd_net_poll(nd_pollfd *fds, size_t n, int timeout_ms)
{
#ifdef _WIN32
    WSAPOLLFD stackbuf[64];
    WSAPOLLFD *p = stackbuf;
    int rc;
    if (n > 64) {
        p = (WSAPOLLFD *)malloc(sizeof(WSAPOLLFD) * n);
        if (!p)
            return -1;
    }
    for (size_t i = 0; i < n; i++) {
        p[i].fd = (SOCKET)fds[i].fd;
        p[i].events = 0;
        if (fds[i].events & ND_POLLIN)
            p[i].events |= POLLRDNORM;
        if (fds[i].events & ND_POLLOUT)
            p[i].events |= POLLWRNORM;
        p[i].revents = 0;
    }
    rc = WSAPoll(p, (ULONG)n, timeout_ms);
    for (size_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (p[i].revents & (POLLRDNORM | POLLRDBAND))
            fds[i].revents |= ND_POLLIN;
        if (p[i].revents & POLLWRNORM)
            fds[i].revents |= ND_POLLOUT;
        if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL))
            fds[i].revents |= ND_POLLIN;
    }
    if (p != stackbuf)
        free(p);
    return rc;
#else
    struct pollfd stackbuf[64];
    struct pollfd *p = stackbuf;
    int rc;
    if (n > 64) {
        p = (struct pollfd *)malloc(sizeof(struct pollfd) * n);
        if (!p)
            return -1;
    }
    for (size_t i = 0; i < n; i++) {
        p[i].fd = fds[i].fd;
        p[i].events = 0;
        if (fds[i].events & ND_POLLIN)
            p[i].events |= POLLIN;
        if (fds[i].events & ND_POLLOUT)
            p[i].events |= POLLOUT;
        p[i].revents = 0;
    }
    rc = poll(p, (nfds_t)n, timeout_ms);
    for (size_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (p[i].revents & POLLIN)
            fds[i].revents |= ND_POLLIN;
        if (p[i].revents & POLLOUT)
            fds[i].revents |= ND_POLLOUT;
        if (p[i].revents & POLLERR)
            fds[i].revents |= ND_POLLIN;
    }
    if (p != stackbuf)
        free(p);
    return rc;
#endif
}

int nd_net_shutdown(int fd)
{
#ifdef _WIN32
    return shutdown((SOCKET)fd, SD_BOTH);
#else
    return shutdown(fd, SHUT_RDWR);
#endif
}

void nd_net_close(int fd)
{
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

int nd_net_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
