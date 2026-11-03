#ifndef ND_SERVICE_H
#define ND_SERVICE_H

#include "core/thread.h"
#include "net/planner.h"
#include "session/conn.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct nd_service {
    uint64_t      id;
    char          name[64];
    char          proto[16];
    nd_bind_spec  bind;
    nd_plan       plan;

    nd_thread     th_accept;
    atomic_int    refs;
    atomic_bool   running;
    atomic_bool   stopping;

    nd_mutex      lock;
    nd_conn     **conns;
    size_t        n_conns;
    size_t        cap_conns;
    size_t        max_conns;
    int           echo;

    /* FTP 服务端配置 */
    char          ftp_root[512];
    int           ftp_readonly;
    int           ftp_anonymous;
    char          ftp_user[64];
    char          ftp_pass[64];
    uint16_t      pasv_min;
    uint16_t      pasv_max;
    char          ftp_advertise[64];

    /* TFTP 服务端配置 */
    char          tftp_root[512];
    int           tftp_readonly;
    int           tftp_timeout;

    /* HTTP 服务端：静态文件根目录；echo=1 时返回请求内容。 */
    char          http_root[512];
    atomic_int    http_active;
    atomic_uint_fast64_t http_requests;

    /* 指标：本次启动时刻（单调时钟，0=未运行）与峰值并发 */
    atomic_uint_fast64_t started_ms;
    atomic_int    peak_conns;

    char          last_error[256];
    struct nd_service *next;
} nd_service;

nd_service *nd_service_new(const char *name, const char *proto,
                           const nd_bind_spec *bind, int echo);
int   nd_service_start(nd_service *s, char *err, size_t errlen);
void  nd_service_stop(nd_service *s);
void  nd_service_free(nd_service *s); /* 停止并从注册表移除 */

nd_service *nd_service_get(uint64_t id);
void        nd_service_put(nd_service *s);

size_t nd_service_list(uint64_t *ids, size_t cap);
const char *nd_service_proto(const nd_service *s);
int   nd_service_running(const nd_service *s);
size_t nd_service_conn_count(nd_service *s);

void  nd_service_detach_conn(uint64_t service_id, nd_conn *c);

#endif /* ND_SERVICE_H */
