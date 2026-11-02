#ifndef ND_SMB_SESSION_H
#define ND_SMB_SESSION_H

#include "core/thread.h"
#include "proto/smb2_client.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct nd_smb_sess {
    uint64_t id;
    char     host[256];
    char     share[128];
    char     user[64];
    char     domain[64];
    nd_smb2 *client;

    atomic_int refs;
    nd_mutex   lock;
    struct nd_smb_sess *next;
} nd_smb_sess;

nd_smb_sess *nd_smb_sess_new(const char *host, const char *share, const char *user,
                             const char *domain, nd_smb2 *client);
nd_smb_sess *nd_smb_sess_get(uint64_t id);
void         nd_smb_sess_put(nd_smb_sess *s);
void         nd_smb_sess_free(nd_smb_sess *s);
size_t       nd_smb_sess_list(uint64_t *ids, size_t cap);

#endif
