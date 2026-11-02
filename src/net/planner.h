#ifndef ND_PLANNER_H
#define ND_PLANNER_H

#include "net/addr.h"
#include "net/iface.h"

#include <stddef.h>
#include <stdint.h>

#define ND_MAX_LISTEN_SOCKS 64

typedef enum {
    ND_FAM_V4 = 1,
    ND_FAM_V6 = 2,
    ND_FAM_DUAL = 3
} nd_family;

typedef enum {
    ND_BIND_ANY = 0,
    ND_BIND_IFACE = 1,
    ND_BIND_ADDR = 2
} nd_bind_kind;

typedef struct nd_bind_spec {
    nd_bind_kind kind;
    nd_family    family;
    uint16_t     port;       /* 0 = 系统分配 */
    int          type;       /* SOCK_STREAM / SOCK_DGRAM，0 = STREAM */
    char         iface_id[80];
    nd_addr      addr;
} nd_bind_spec;

typedef struct nd_plan_slot {
    int     fd;              /* 未绑定时为 -1 */
    nd_addr local;
    int     err;             /* 0 或 ND_E_* */
    int     sys_errno;
} nd_plan_slot;

typedef struct nd_plan {
    nd_bind_spec spec;
    size_t       n;
    nd_plan_slot slot[ND_MAX_LISTEN_SOCKS];
    int          degraded;      /* 部分地址失败，服务仍可用 */
    int          fatal;
    int          partial_family;/* 双栈只有一族有地址 */
    int          err;
} nd_plan;

const char *nd_family_name(nd_family f);

/* do_bind=1：真正 bind（并按 type 决定是否 listen），成功后 slot[].fd 有效。
 * do_bind=0：只做展开与校验，fd 全为 -1。 */
int  nd_plan_build(const nd_bind_spec *spec, const nd_iface *ifs, size_t n_ifs,
                   int do_bind, nd_plan *out);
void nd_plan_close(nd_plan *p);
void nd_plan_format(const nd_plan *p, char *buf, size_t cap);

#endif /* ND_PLANNER_H */
