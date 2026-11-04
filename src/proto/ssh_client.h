#ifndef ND_SSH_CLIENT_H
#define ND_SSH_CLIENT_H

#include "net/planner.h"

#include <stddef.h>
#include <stdint.h>

typedef struct nd_ssh nd_ssh;

/* 分步建链，便于把耗时的握手/认证放到后台线程：
 * open → handshake → auth → shell。
 * 认证阶段如果 interactive=1 且没给密码，会通过输出回调提示用户，
 * 并等待其它线程用 nd_ssh_write 送进来的密码（回车结束）。 */

nd_ssh *nd_ssh_open(const char *host, uint16_t port, nd_family family,
                    int timeout_ms, int *sock_out, char *err, size_t errlen);
int  nd_ssh_handshake(nd_ssh *s, int compat, char *err, size_t errlen);
int  nd_ssh_auth(nd_ssh *s, const char *user, const char *password,
                 const char *key_path, int interactive, char *err, size_t errlen);
int  nd_ssh_shell(nd_ssh *s, int cols, int rows, char *err, size_t errlen);

/* 认证提示文字（例如 "password: "）通过这个回调送到终端；ctx 一般是对应 nd_conn */
void nd_ssh_set_output(nd_ssh *s, void (*cb)(void *ctx, const char *text, size_t len),
                       void *ctx);

int  nd_ssh_read(nd_ssh *s, void *buf, size_t cap);
int  nd_ssh_write(nd_ssh *s, const void *buf, size_t len);
void nd_ssh_close(nd_ssh *s);

/* 取消正在等待的交互式输入（关闭连接时调用），阻塞中的认证会立即返回 */
void nd_ssh_cancel(nd_ssh *s);

/* 引用计数：连接与后台认证线程各持一份，避免提前释放 */
nd_ssh *nd_ssh_get(nd_ssh *s);
void    nd_ssh_put(nd_ssh *s);

const char *nd_ssh_fingerprint(nd_ssh *s);

#endif
