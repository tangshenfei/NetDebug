#ifndef ND_SERIAL_H
#define ND_SERIAL_H

#include <stddef.h>

typedef struct nd_serial nd_serial;

typedef struct {
    char port[64];
    char desc[128];
} nd_serial_port;

int  nd_serial_list(nd_serial_port *out, size_t cap, size_t *n);

/* parity: 0 无, 1 奇, 2 偶；stop_bits: 1/2；flow: 0 无, 1 RTS/CTS, 2 XON/XOFF */
nd_serial *nd_serial_open(const char *port, int baud, int data_bits,
                          int parity, int stop_bits, int flow,
                          int dtr, int rts, char *err, size_t errlen);
void nd_serial_close(nd_serial *s);
void nd_serial_cancel(nd_serial *s);

/* >0 字节数，-1 错误，-2 超时 */
int  nd_serial_read(nd_serial *s, void *buf, size_t cap, int timeout_ms);
int  nd_serial_write(nd_serial *s, const void *buf, size_t len);

int  nd_serial_set_dtr(nd_serial *s, int on);
int  nd_serial_set_rts(nd_serial *s, int on);
const char *nd_serial_port_name(nd_serial *s);

#endif /* ND_SERIAL_H */
