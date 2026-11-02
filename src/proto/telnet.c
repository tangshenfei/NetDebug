#include "proto/telnet.h"
#include "session/conn.h"
#include "core/log.h"

#include <stdlib.h>
#include <string.h>

#define T_IAC  255
#define T_DONT 254
#define T_DO   253
#define T_WONT 252
#define T_WILL 251
#define T_SB   250
#define T_SE   240

#define OPT_BINARY 0
#define OPT_ECHO   1
#define OPT_SGA    3
#define OPT_TTYPE  24
#define OPT_NAWS   31

enum {
    TS_DATA = 0,
    TS_IAC,
    TS_OPT,
    TS_SB,
    TS_SB_DATA,
    TS_SB_IAC
};

struct nd_telnet {
    int      is_server;
    int      state;
    int      verb;
    int      sb_option;
    unsigned char sb[512];
    size_t   sblen;
};

static void send_raw(nd_conn *c, const unsigned char *p, size_t n)
{
    nd_conn_send_raw(c, p, n);
}

static void send_cmd(nd_conn *c, unsigned char verb, unsigned char opt)
{
    unsigned char b[3] = {T_IAC, verb, opt};
    send_raw(c, b, sizeof(b));
}

static void send_sb_str(nd_conn *c, unsigned char opt, unsigned char code, const char *s)
{
    unsigned char buf[256];
    size_t n = 0;
    size_t slen = strlen(s);
    if (slen > sizeof(buf) - 6)
        slen = sizeof(buf) - 6;
    buf[n++] = T_IAC;
    buf[n++] = T_SB;
    buf[n++] = opt;
    buf[n++] = code;
    memcpy(buf + n, s, slen);
    n += slen;
    buf[n++] = T_IAC;
    buf[n++] = T_SE;
    send_raw(c, buf, n);
}

static void send_naws(nd_conn *c)
{
    unsigned char b[9];
    b[0] = T_IAC;
    b[1] = T_SB;
    b[2] = OPT_NAWS;
    b[3] = 0;
    b[4] = 80;
    b[5] = 0;
    b[6] = 24;
    b[7] = T_IAC;
    b[8] = T_SE;
    send_raw(c, b, sizeof(b));
}

static void handle_sb(nd_telnet *t, nd_conn *c)
{
    if (!t->is_server && t->sb_option == OPT_TTYPE && t->sblen >= 1 && t->sb[0] == 1) {
        /* TTYPE SEND -> IS */
        send_sb_str(c, OPT_TTYPE, 0, "XTERM-256COLOR");
    }
    /* 服务端收到 NAWS/TTYPE 只记录不处理；窗口大小解析留到后续版本 */
}

static void handle_do(nd_telnet *t, nd_conn *c, int opt)
{
    if (t->is_server) {
        if (opt == OPT_ECHO || opt == OPT_SGA || opt == OPT_BINARY)
            send_cmd(c, T_WILL, (unsigned char)opt);
        else
            send_cmd(c, T_WONT, (unsigned char)opt);
        return;
    }
    switch (opt) {
    case OPT_TTYPE:
        send_sb_str(c, OPT_TTYPE, 0, "XTERM-256COLOR");
        break;
    case OPT_NAWS:
        send_naws(c);
        break;
    case OPT_SGA:
    case OPT_BINARY:
        send_cmd(c, T_WILL, (unsigned char)opt);
        break;
    default:
        send_cmd(c, T_WONT, (unsigned char)opt);
        break;
    }
}

static void handle_will(nd_telnet *t, nd_conn *c, int opt)
{
    (void)t;
    if (t->is_server) {
        if (opt == OPT_NAWS || opt == OPT_TTYPE || opt == OPT_SGA || opt == OPT_BINARY)
            return; /* 接受 */
        send_cmd(c, T_DONT, (unsigned char)opt);
        return;
    }
    if (opt == OPT_ECHO || opt == OPT_SGA || opt == OPT_BINARY)
        return; /* 接受，不做本地回显 */
    send_cmd(c, T_DONT, (unsigned char)opt);
}

nd_telnet *nd_telnet_new(int is_server)
{
    nd_telnet *t = (nd_telnet *)calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->is_server = is_server;
    t->state = TS_DATA;
    return t;
}

void nd_telnet_free(nd_telnet *t)
{
    free(t);
}

size_t nd_telnet_feed(nd_telnet *t, void *vconn,
                      const unsigned char *in, size_t len,
                      unsigned char *out, size_t cap)
{
    nd_conn *c = (nd_conn *)vconn;
    size_t o = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char b = in[i];
        switch (t->state) {
        case TS_DATA:
            if (b == T_IAC) {
                t->state = TS_IAC;
            } else if (o < cap) {
                out[o++] = b;
            }
            break;
        case TS_IAC:
            if (b == T_IAC) {
                if (o < cap)
                    out[o++] = T_IAC; /* 转义的字面 255 */
                t->state = TS_DATA;
            } else if (b == T_WILL || b == T_WONT || b == T_DO || b == T_DONT) {
                t->verb = b;
                t->state = TS_OPT;
            } else if (b == T_SB) {
                t->state = TS_SB;
            } else {
                t->state = TS_DATA; /* NOP / GA 等 */
            }
            break;
        case TS_OPT:
            if (t->verb == T_DO)
                handle_do(t, c, b);
            else if (t->verb == T_WILL)
                handle_will(t, c, b);
            else if (t->verb == T_WONT)
                send_cmd(c, T_DONT, b);
            else if (t->verb == T_DONT)
                send_cmd(c, T_WONT, b);
            t->state = TS_DATA;
            break;
        case TS_SB:
            t->sb_option = b;
            t->sblen = 0;
            t->state = TS_SB_DATA;
            break;
        case TS_SB_DATA:
            if (b == T_IAC) {
                t->state = TS_SB_IAC;
            } else if (t->sblen < sizeof(t->sb)) {
                t->sb[t->sblen++] = b;
            }
            break;
        case TS_SB_IAC:
            if (b == T_SE) {
                handle_sb(t, c);
                t->state = TS_DATA;
            } else if (b == T_IAC) {
                if (t->sblen < sizeof(t->sb))
                    t->sb[t->sblen++] = T_IAC;
                t->state = TS_SB_DATA;
            } else {
                t->state = TS_SB_DATA;
            }
            break;
        default:
            t->state = TS_DATA;
            break;
        }
    }
    return o;
}

void nd_conn_enable_telnet(nd_conn *c, int is_server)
{
    nd_telnet *t;
    unsigned char init[24];
    size_t n = 0;

    if (!c || c->telnet)
        return;
    t = nd_telnet_new(is_server);
    if (!t)
        return;
    c->telnet = t;

    if (is_server) {
        /* WILL ECHO, WILL SGA, DO NAWS, DO TTYPE */
        init[n++] = T_IAC; init[n++] = T_WILL; init[n++] = OPT_ECHO;
        init[n++] = T_IAC; init[n++] = T_WILL; init[n++] = OPT_SGA;
        init[n++] = T_IAC; init[n++] = T_DO;   init[n++] = OPT_NAWS;
        init[n++] = T_IAC; init[n++] = T_DO;   init[n++] = OPT_TTYPE;
    } else {
        /* WILL TTYPE, WILL NAWS, WILL SGA, DO SGA, DO ECHO */
        init[n++] = T_IAC; init[n++] = T_WILL; init[n++] = OPT_TTYPE;
        init[n++] = T_IAC; init[n++] = T_WILL; init[n++] = OPT_NAWS;
        init[n++] = T_IAC; init[n++] = T_WILL; init[n++] = OPT_SGA;
        init[n++] = T_IAC; init[n++] = T_DO;   init[n++] = OPT_SGA;
        init[n++] = T_IAC; init[n++] = T_DO;   init[n++] = OPT_ECHO;
    }
    send_raw(c, init, n);
    ND_LOGI("telnet %s negotiation sent on conn %llu",
            is_server ? "server" : "client", (unsigned long long)c->id);
}
