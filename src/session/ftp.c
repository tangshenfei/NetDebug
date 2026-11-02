#include "session/ftp.h"
#include "core/thread.h"
#include "core/util.h"

#include <stdlib.h>
#include <string.h>

static nd_mutex g_lock;
static int g_ready = 0;
static nd_ftp_sess *g_list = NULL;
static uint64_t g_next = 1;

static void init(void)
{
    if (!g_ready) {
        nd_mutex_init(&g_lock);
        g_ready = 1;
    }
}

nd_ftp_sess *nd_ftp_sess_new(const char *host, uint16_t port, const char *user,
                             nd_family family, nd_ftp *client)
{
    nd_ftp_sess *s;
    init();
    s = (nd_ftp_sess *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    nd_strlcpy(s->host, host, sizeof(s->host));
    s->port = port;
    nd_strlcpy(s->user, user ? user : "anonymous", sizeof(s->user));
    s->family = family;
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

nd_ftp_sess *nd_ftp_sess_get(uint64_t id)
{
    nd_ftp_sess *p;
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

void nd_ftp_sess_put(nd_ftp_sess *s)
{
    if (!s)
        return;
    if (atomic_fetch_sub(&s->refs, 1) == 1) {
        nd_mutex_destroy(&s->lock);
        free(s);
    }
}

void nd_ftp_sess_free(nd_ftp_sess *s)
{
    if (!s)
        return;
    nd_mutex_lock(&g_lock);
    {
        nd_ftp_sess **pp = &g_list;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    nd_mutex_unlock(&g_lock);
    if (s->client) {
        nd_ftp_close(s->client);
        s->client = NULL;
    }
    nd_ftp_sess_put(s);
}

size_t nd_ftp_sess_list(uint64_t *ids, size_t cap)
{
    nd_ftp_sess *p;
    size_t n = 0;
    init();
    nd_mutex_lock(&g_lock);
    for (p = g_list; p && n < cap; p = p->next)
        ids[n++] = p->id;
    nd_mutex_unlock(&g_lock);
    return n;
}
