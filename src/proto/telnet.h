#ifndef ND_TELNET_H
#define ND_TELNET_H

#include <stddef.h>

typedef struct nd_telnet nd_telnet;

nd_telnet *nd_telnet_new(int is_server);
void       nd_telnet_free(nd_telnet *t);
size_t     nd_telnet_feed(nd_telnet *t, void *conn,
                          const unsigned char *in, size_t len,
                          unsigned char *out, size_t cap);

#endif /* ND_TELNET_H */
