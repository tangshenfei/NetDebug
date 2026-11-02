#ifndef ND_SSH_CLIENT_H
#define ND_SSH_CLIENT_H

#include "net/planner.h"

#include <stddef.h>
#include <stdint.h>

typedef struct nd_ssh nd_ssh;

nd_ssh *nd_ssh_connect(const char *host, uint16_t port, nd_family family,
                       const char *user, const char *password,
                       const char *key_path, int compat,
                       int timeout_ms, int *sock_out,
                       char *err, size_t errlen);
int  nd_ssh_shell(nd_ssh *s, int cols, int rows, char *err, size_t errlen);
int  nd_ssh_read(nd_ssh *s, void *buf, size_t cap);
int  nd_ssh_write(nd_ssh *s, const void *buf, size_t len);
void nd_ssh_close(nd_ssh *s);
const char *nd_ssh_fingerprint(nd_ssh *s);

#endif
