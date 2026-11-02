#ifndef ND_FTP_SESSION_H
#define ND_FTP_SESSION_H

#include "core/thread.h"
#include "net/planner.h"
#include "proto/ftp_client.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct nd_ftp_sess {
    uint64_t     id;
    char         host[256];
    uint16_t     port;
    char         user[64];
    nd_family    family;
    char         pwd[1024];
    nd_ftp      *client;

    atomic_int   refs;
    nd_mutex     lock;
    struct nd_ftp_sess *next;
} nd_ftp_sess;

nd_ftp_sess *nd_ftp_sess_new(const char *host, uint16_t port, const char *user,
                             nd_family family, nd_ftp *client);
nd_ftp_sess *nd_ftp_sess_get(uint64_t id);
void         nd_ftp_sess_put(nd_ftp_sess *s);
void         nd_ftp_sess_free(nd_ftp_sess *s);
size_t       nd_ftp_sess_list(uint64_t *ids, size_t cap);

#endif /* ND_FTP_SESSION_H */
