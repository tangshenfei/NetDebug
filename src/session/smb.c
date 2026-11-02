#include "session/smb.h"
#include "core/util.h"

#include <stdlib.h>
#include <string.h>

static nd_mutex g_lock;
static int g_ready = 0;
static nd_smb_sess *g_list = NULL;
static uint64_t g_next = 1;

static void init(void)
{
    if (!g_ready) {
        nd_mutex_init(&g_lock);
        g_ready = 1;
    }
}

nd_smb_sess *nd_smb_sess_new(const char *host, const char *share, const char *user,
                             const char *domain, nd_smb2 *client)
{
    nd_smb_sess *s;
    init();
    s = (nd_smb_sess *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    nd_strlcpy(s->host, host, sizeof(s->host));
    nd_strlcpy(s->share, share, sizeof(s->share));
    nd_strlcpy(s->user, user ? user : "", sizeof(s->user));
    nd_strlcpy(s->domain, domain ? domain : "", sizeof(s->domain));
    s->client = client;
    atomic_init(&s->refs, 1);
    nd_mutex_init(&s->lock);
    nd_mutex_lock(&g_lock);
    s->id = g_next++;
    s->next = g_list;
    g_list = s;
    nd_mutex_unlock(&g_lock);
    return s;
}

nd_smb_sess *nd_smb_sess_get(uint64_t id)
{
    nd_smb_sess *p;
    init();
    nd_mutex_lock(&g_lock);
    for (p = g_list; p; p = p->next) {
        if (p->id == id) {
            atomic_fetch_add(&p->refs, 1);
            break;
        }
    }
    nd_mutex_unlock(&g_lock);
    return p;
}

void nd_smb_sess_put(nd_smb_sess *s)
{
    if (!s)
        return;
    if (atomic_fetch_sub(&s->refs, 1) == 1) {
        nd_mutex_destroy(&s->lock);
        free(s);
    }
}

void nd_smb_sess_free(nd_smb_sess *s)
{
    if (!s)
        return;
    nd_mutex_lock(&g_lock);
    {
        nd_smb_sess **pp = &g_list;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    nd_mutex_unlock(&g_lock);
    if (s->client) {
        nd_smb2_close(s->client);
        s->client = NULL;
    }
    nd_smb_sess_put(s);
}

size_t nd_smb_sess_list(uint64_t *ids, size_t cap)
{
    nd_smb_sess *p;
    size_t n = 0;
    init();
    nd_mutex_lock(&g_lock);
    for (p = g_list; p && n < cap; p = p->next)
        ids[n++] = p->id;
    nd_mutex_unlock(&g_lock);
    return n;
}
