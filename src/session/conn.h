#ifndef ND_CONN_H
#define ND_CONN_H

#include "net/addr.h"
#include "core/thread.h"
#include "platform/serial.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct nd_conn {
    uint64_t       id;
    uint64_t       service_id;   /* 0 = 客户端会话 */
    int            fd;
    char           proto[16];
    int            echo;
    int            datagram;    /* UDP：sendto 到 last_peer */
    int            owns_fd;     /* fd 由本连接关闭（服务端共享套接字为 0） */
    nd_serial     *serial;      /* 串口连接：fd = -1，读写走 serial */
    void          *ssh;         /* nd_ssh*，SSH 通道 */
    void          *telnet;      /* nd_telnet*，非空时 rx 做 IAC 过滤 */

    nd_mutex       send_lock;
    nd_mutex       log_lock;
    nd_thread      th_rx;
    int            started;

    atomic_int     refs;
    atomic_bool    closing;
    atomic_bool    closed;

    uint64_t       rx_bytes;
    uint64_t       tx_bytes;
    uint64_t       started_ms;
    uint64_t       last_rx_ms;

    nd_addr        peer;
    nd_addr        local;
    nd_addr        last_peer;   /* UDP 最近一次对端 */

    FILE          *log;
    struct nd_conn *next;
} nd_conn;

nd_conn *nd_conn_new(int fd, const nd_addr *peer, const nd_addr *local,
                     uint64_t service_id, const char *proto, int echo);
int      nd_conn_start(nd_conn *c);
int      nd_conn_send(nd_conn *c, const void *data, size_t len);
int      nd_conn_send_raw(nd_conn *c, const void *data, size_t len); /* 不发 WS/日志，用于协商 */
void     nd_conn_close(nd_conn *c);

/* Telnet：把连接切换为 IAC 过滤模式，并发送初始协商 */
void     nd_conn_enable_telnet(nd_conn *c, int is_server);

/* UDP */
void     nd_conn_set_datagram(nd_conn *c, int on, int owns_fd);
void     nd_conn_set_peer(nd_conn *c, const nd_addr *peer);
/* 服务端 UDP 收到数据报后调用：更新对端、统计、广播、日志、可选回显 */
void     nd_conn_note_rx(nd_conn *c, const void *data, size_t n, const nd_addr *peer);

nd_conn *nd_conn_get(uint64_t id);
void     nd_conn_put(nd_conn *c);
size_t   nd_conn_list(uint64_t service_id, uint64_t *ids, size_t cap);
int      nd_conn_is_closed(nd_conn *c);

#endif /* ND_CONN_H */
