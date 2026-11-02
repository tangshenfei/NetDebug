#ifndef ND_SESSION_H
#define ND_SESSION_H

#include "core/thread.h"
#include "net/addr.h"
#include "net/planner.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ND_SESS_IDLE = 0,
    ND_SESS_CONNECTING = 1,
    ND_SESS_OPEN = 2,
    ND_SESS_ERROR = 3
} nd_session_state;

typedef struct nd_session {
    uint64_t     id;
    char         name[64];
    char         host[256];
    uint16_t     port;
    nd_family    family;
    int          transport;   /* 0 = TCP，1 = UDP */
    int          telnet;      /* 1 = Telnet（TCP + IAC 协商） */
    int          serial;      /* 1 = 串口会话 */
    char         serial_port[64];
    int          baud;
    int          data_bits;
    int          parity;
    int          stop_bits;
    int          flow;
    int          dtr;
    int          rts;

    int          ssh;
    char         ssh_user[64];
    char         ssh_pass[128];
    char         ssh_key[512];
    int          ssh_compat;
    int          ssh_cols;
    int          ssh_rows;
    char         ssh_fp[80];
    char         iface_id[80];
    char         src_addr[80];

    uint64_t     conn_id;
    atomic_int   refs;
    atomic_int   state;
    atomic_int   busy;
    nd_mutex     lock;
    char         last_error[256];
    uint64_t     created_ms;
    struct nd_session *next;
} nd_session;

nd_session *nd_session_new(const char *name, const char *host, uint16_t port,
                           nd_family family, const char *iface_id,
                           const char *src_addr, int transport, int telnet);
int  nd_session_connect(nd_session *s, uint64_t *conn_id, char *err, size_t errlen);
void nd_session_disconnect(nd_session *s);
void nd_session_sync(nd_session *s);
void nd_session_free(nd_session *s);

nd_session *nd_session_get(uint64_t id);
void         nd_session_put(nd_session *s);
size_t       nd_session_list(uint64_t *ids, size_t cap);
int          nd_session_get_state(nd_session *s);
uint64_t     nd_session_get_conn(nd_session *s);

#endif /* ND_SESSION_H */
