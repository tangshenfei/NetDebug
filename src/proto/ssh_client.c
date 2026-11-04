#include "proto/ssh_client.h"
#include "net/resolver.h"
#include "core/error.h"
#include "core/log.h"
#include "core/thread.h"
#include "core/util.h"
#include "platform/netio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef ND_HAVE_SSH
#include <libssh2.h>

#define SSH_AUTH_TIMEOUT_MS 120000  /* 等用户输密码最多 2 分钟 */

struct nd_ssh {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              sock;
    char             fingerprint[64];
    char             password[128];
    int              interactive;

    /* 交互式认证：认证线程在这里等终端送进来的密码 */
    nd_mutex         lock;
    nd_cond          cond;
    int              lock_init;
    char             inbuf[256];
    size_t           inlen;
    int              waiting;
    int              got;
    int              cancelled;

    void           (*out_cb)(void *ctx, const char *text, size_t len);
    void            *out_ctx;

    atomic_int       refs;
};

static void ssh_emit(nd_ssh *s, const char *text)
{
    if (s && s->out_cb && text)
        s->out_cb(s->out_ctx, text, strlen(text));
}

/* 等用户在终端里敲一行；返回字节数，超时/取消返回 0 */
static size_t auth_read_line(nd_ssh *s, char *out, size_t cap, int timeout_ms)
{
    int waited = 0;
    size_t n = 0;

    out[0] = '\0';
    nd_mutex_lock(&s->lock);
    s->inlen = 0;
    s->got = 0;
    s->waiting = 1;
    nd_mutex_unlock(&s->lock);

    nd_mutex_lock(&s->lock);
    while (!s->got && !s->cancelled && waited < timeout_ms) {
        if (nd_cond_timedwait(&s->cond, &s->lock, 250) != 0)
            waited += 250;
    }
    if (s->got && s->inlen > 0) {
        n = s->inlen < cap - 1 ? s->inlen : cap - 1;
        memcpy(out, s->inbuf, n);
        out[n] = '\0';
    }
    s->waiting = 0;
    s->inlen = 0;
    nd_mutex_unlock(&s->lock);
    return n;
}

static void kbd_callback(const char *name, int name_len, const char *instruction,
                         int instruction_len, int num_prompts,
                         const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                         LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
{
    nd_ssh *s = (nd_ssh *)(abstract ? *abstract : NULL);
    (void)name; (void)name_len; (void)instruction; (void)instruction_len;
    if (!s)
        return;
    for (int i = 0; i < num_prompts; i++) {
        char answer[256];
        if (s->interactive) {
            const char *p = (prompts && prompts[i].text) ? (const char *)prompts[i].text
                                                         : "Password: ";
            char shown[128];
            size_t pl = prompts ? prompts[i].length : 0;
            if (!pl)
                pl = strlen(p);
            if (pl > sizeof(shown) - 1)
                pl = sizeof(shown) - 1;
            memcpy(shown, p, pl);
            shown[pl] = '\0';
            ssh_emit(s, shown);
            auth_read_line(s, answer, sizeof(answer), SSH_AUTH_TIMEOUT_MS);
            ssh_emit(s, "\r\n");
        } else {
            nd_strlcpy(answer, s->password, sizeof(answer));
        }
        {
            size_t n = strlen(answer) + 1;
            responses[i].text = (char *)malloc(n);
            if (!responses[i].text)
                continue;
            memcpy(responses[i].text, answer, n);
            responses[i].length = (unsigned int)(n - 1);
        }
    }
}

static void set_err(nd_ssh *s, char *err, size_t errlen, const char *prefix)
{
    char *msg = NULL;
    int len = 0;
    libssh2_session_last_error(s ? s->session : NULL, &msg, &len, 0);
    snprintf(err, errlen, "%s: %s", prefix, msg && len ? msg : "SSH 错误");
}

nd_ssh *nd_ssh_get(nd_ssh *s)
{
    if (s)
        atomic_fetch_add(&s->refs, 1);
    return s;
}

void nd_ssh_put(nd_ssh *s)
{
    if (!s)
        return;
    if (atomic_fetch_sub(&s->refs, 1) != 1)
        return;
    /* 最后一个引用：这时已经没人用它了，可以安全收尾 */
    if (s->channel) {
        libssh2_channel_send_eof(s->channel);
        libssh2_channel_close(s->channel);
        libssh2_channel_free(s->channel);
        s->channel = NULL;
    }
    if (s->session) {
        libssh2_session_disconnect(s->session, "bye");
        libssh2_session_free(s->session);
        s->session = NULL;
    }
    if (s->sock >= 0) {
        nd_net_close(s->sock);
        s->sock = -1;
    }
    if (s->lock_init) {
        nd_cond_destroy(&s->cond);
        nd_mutex_destroy(&s->lock);
    }
    free(s);
}

void nd_ssh_cancel(nd_ssh *s)
{
    if (!s || !s->lock_init)
        return;
    nd_mutex_lock(&s->lock);
    s->cancelled = 1;
    nd_cond_broadcast(&s->cond);
    nd_mutex_unlock(&s->lock);
}

void nd_ssh_set_output(nd_ssh *s, void (*cb)(void *ctx, const char *text, size_t len),
                       void *ctx)
{
    if (!s)
        return;
    s->out_cb = cb;
    s->out_ctx = ctx;
}

nd_ssh *nd_ssh_open(const char *host, uint16_t port, nd_family family,
                    int timeout_ms, int *sock_out, char *err, size_t errlen)
{
    nd_addr addrs[16];
    size_t n = 0;
    int fd = -1;
    int sys = 0;
    nd_ssh *s;

    *sock_out = -1;
    if (nd_resolve(host, port, family, addrs, 16, &n, err, errlen) != 0)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        fd = nd_net_socket(addrs[i].family, SOCK_STREAM);
        if (fd < 0)
            continue;
        if (nd_net_connect(fd, &addrs[i], timeout_ms, &sys) == 0)
            break;
        nd_net_close(fd);
        fd = -1;
    }
    if (fd < 0) {
        snprintf(err, errlen, "连接 SSH 服务器失败");
        return NULL;
    }
    nd_net_set_timeouts(fd, 1000, 15000);
    nd_net_set_nodelay(fd);

    s = (nd_ssh *)calloc(1, sizeof(*s));
    if (!s) {
        nd_net_close(fd);
        snprintf(err, errlen, "内存不足");
        return NULL;
    }
    s->sock = fd;
    atomic_init(&s->refs, 1);
    if (nd_mutex_init(&s->lock) != 0 || nd_cond_init(&s->cond) != 0) {
        nd_net_close(fd);
        free(s);
        snprintf(err, errlen, "初始化同步对象失败");
        return NULL;
    }
    s->lock_init = 1;
    *sock_out = fd;
    return s;
}

int nd_ssh_handshake(nd_ssh *s, int compat, char *err, size_t errlen)
{
    unsigned char *hash;

    if (!s || s->sock < 0) {
        snprintf(err, errlen, "SSH 连接未建立");
        return -1;
    }
    if (libssh2_init(0) != 0) {
        snprintf(err, errlen, "libssh2 初始化失败");
        return -1;
    }
    s->session = libssh2_session_init();
    if (!s->session) {
        snprintf(err, errlen, "创建 SSH 会话失败");
        return -1;
    }
    libssh2_session_set_blocking(s->session, 1);
    /* 对端不说话（端口填错、不是 SSH 服务）时不要无限等 */
    libssh2_session_set_timeout(s->session, 20000);
    *libssh2_session_abstract(s->session) = s;
    if (compat) {
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_KEX,
            "diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,"
            "diffie-hellman-group-exchange-sha1");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_HOSTKEY, "ssh-rsa");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_CRYPT_CS,
                                    "aes128-cbc,aes192-cbc,aes256-cbc,aes128-ctr,aes256-ctr");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_CRYPT_SC,
                                    "aes128-cbc,aes192-cbc,aes256-cbc,aes128-ctr,aes256-ctr");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_MAC_CS, "hmac-sha1");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_MAC_SC, "hmac-sha1");
    } else {
        /* 默认只报后端确实实现的算法：WinCNG 不支持 chacha20，
         * 按 libssh2 的默认清单优先报 chacha 会在协商后被判为不支持；
         * group-exchange 在 WinCNG 下也容易协商失败，先给固定组 */
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_KEX,
                                    "diffie-hellman-group14-sha256,"
                                    "diffie-hellman-group16-sha512,"
                                    "diffie-hellman-group18-sha512,"
                                    "diffie-hellman-group-exchange-sha256");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_CRYPT_CS,
                                    "aes256-ctr,aes192-ctr,aes128-ctr,"
                                    "aes256-cbc,aes192-cbc,aes128-cbc");
        libssh2_session_method_pref(s->session, LIBSSH2_METHOD_CRYPT_SC,
                                    "aes256-ctr,aes192-ctr,aes128-ctr,"
                                    "aes256-cbc,aes192-cbc,aes128-cbc");
    }
    if (libssh2_session_handshake(s->session, s->sock) != 0) {
        set_err(s, err, errlen, "SSH 握手失败");
        return -1;
    }
    hash = (unsigned char *)libssh2_hostkey_hash(s->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash) {
        size_t pos = 0;
        for (int i = 0; i < 32 && pos + 3 < sizeof(s->fingerprint); i++)
            pos += (size_t)snprintf(s->fingerprint + pos, sizeof(s->fingerprint) - pos,
                                    "%02x", hash[i]);
    }
    return 0;
}

int nd_ssh_auth(nd_ssh *s, const char *user, const char *password,
                const char *key_path, int interactive, char *err, size_t errlen)
{
    if (!s || !s->session) {
        snprintf(err, errlen, "SSH 会话未建立");
        return -1;
    }
    nd_strlcpy(s->password, password ? password : "", sizeof(s->password));
    s->interactive = interactive;

    if (key_path && key_path[0]) {
        if (libssh2_userauth_publickey_fromfile(s->session, user, NULL, key_path,
                                                s->password[0] ? s->password : NULL) != 0) {
            set_err(s, err, errlen, "公钥认证失败");
            return -1;
        }
        return 0;
    }

    if (interactive) {
        char pw[256];
        size_t n;

        ssh_emit(s, "password: ");
        n = auth_read_line(s, pw, sizeof(pw), SSH_AUTH_TIMEOUT_MS);
        ssh_emit(s, "\r\n");
        if (n == 0) {
            snprintf(err, errlen, "未输入密码，连接取消");
            return -1;
        }
        nd_strlcpy(s->password, pw, sizeof(s->password));
        if (libssh2_userauth_password(s->session, user, pw) == 0)
            return 0;
        if (libssh2_userauth_keyboard_interactive(s->session, user, kbd_callback) == 0)
            return 0;
        set_err(s, err, errlen, "密码认证失败");
        return -1;
    }

    if (libssh2_userauth_password(s->session, user, s->password) != 0) {
        /* 尝试 keyboard-interactive */
        if (libssh2_userauth_keyboard_interactive(s->session, user, kbd_callback) != 0) {
            set_err(s, err, errlen, "密码认证失败");
            return -1;
        }
    }
    return 0;
}

int nd_ssh_shell(nd_ssh *s, int cols, int rows, char *err, size_t errlen)
{
    if (!s || !s->session) {
        snprintf(err, errlen, "SSH 会话未建立");
        return -1;
    }
    s->channel = libssh2_channel_open_session(s->session);
    if (!s->channel) {
        set_err(s, err, errlen, "打开 SSH 通道失败");
        return -1;
    }
    libssh2_channel_request_pty_ex(s->channel, "xterm-256color", 14, NULL, 0,
                                   cols > 0 ? cols : 120, rows > 0 ? rows : 32,
                                   0, 0);
    if (libssh2_channel_shell(s->channel) != 0) {
        set_err(s, err, errlen, "启动 shell 失败");
        return -1;
    }
    return 0;
}

int nd_ssh_read(nd_ssh *s, void *buf, size_t cap)
{
    ssize_t n;
    if (!s)
        return -1;
    if (!s->channel)
        return -2; /* 还在认证，读线程等着 */
    n = libssh2_channel_read(s->channel, (char *)buf, cap);
    if (n > 0)
        return (int)n;
    if (n == 0) {
        if (libssh2_channel_eof(s->channel))
            return 0;
        return -2;
    }
    if (n == LIBSSH2_ERROR_EAGAIN || n == LIBSSH2_ERROR_TIMEOUT)
        return -2;
    return -1;
}

int nd_ssh_write(nd_ssh *s, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t off = 0;

    if (!s)
        return -1;

    /* 认证阶段：把输入当作密码收进来，不写通道 */
    if (s->lock_init) {
        int waiting;
        nd_mutex_lock(&s->lock);
        waiting = s->waiting;
        if (waiting) {
            for (size_t i = 0; i < len; i++) {
                unsigned char ch = (unsigned char)p[i];
                if (ch == '\r' || ch == '\n') {
                    s->got = 1;
                    break;
                }
                if (ch == 0x7f || ch == 0x08) {
                    if (s->inlen > 0)
                        s->inlen--;
                } else if (ch >= 0x20 && s->inlen + 1 < sizeof(s->inbuf)) {
                    s->inbuf[s->inlen++] = (char)ch;
                }
            }
            nd_cond_broadcast(&s->cond);
        }
        nd_mutex_unlock(&s->lock);
        if (waiting)
            return 0;
    }

    if (!s->channel)
        return -1;
    while (off < len) {
        ssize_t n = libssh2_channel_write(s->channel, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == LIBSSH2_ERROR_EAGAIN || n == LIBSSH2_ERROR_TIMEOUT) {
            nd_sleep_ms(5);
            continue;
        }
        return -1;
    }
    return 0;
}

void nd_ssh_close(nd_ssh *s)
{
    if (!s)
        return;
    /* 只做「叫停」：可能还有认证线程在用 libssh2，真正的收尾放到最后一个引用释放时 */
    nd_ssh_cancel(s);
    if (s->sock >= 0)
        nd_net_shutdown(s->sock);   /* 唤醒阻塞在握手/读上的线程，socket 由 put 关闭 */
    nd_ssh_put(s);
}

const char *nd_ssh_fingerprint(nd_ssh *s)
{
    return s ? s->fingerprint : "";
}

#else /* !ND_HAVE_SSH */

struct nd_ssh { int dummy; };

nd_ssh *nd_ssh_open(const char *host, uint16_t port, nd_family family,
                    int timeout_ms, int *sock_out, char *err, size_t errlen)
{
    (void)host; (void)port; (void)family; (void)timeout_ms;
    *sock_out = -1;
    snprintf(err, errlen, "当前构建未启用 SSH（需要 libssh2）");
    return NULL;
}
int nd_ssh_handshake(nd_ssh *s, int compat, char *err, size_t errlen)
{ (void)s; (void)compat; snprintf(err, errlen, "SSH 未启用"); return -1; }
int nd_ssh_auth(nd_ssh *s, const char *user, const char *password,
                const char *key_path, int interactive, char *err, size_t errlen)
{ (void)s; (void)user; (void)password; (void)key_path; (void)interactive;
  snprintf(err, errlen, "SSH 未启用"); return -1; }
int nd_ssh_shell(nd_ssh *s, int cols, int rows, char *err, size_t errlen)
{ (void)s; (void)cols; (void)rows; snprintf(err, errlen, "SSH 未启用"); return -1; }
void nd_ssh_set_output(nd_ssh *s, void (*cb)(void *ctx, const char *text, size_t len),
                       void *ctx) { (void)s; (void)cb; (void)ctx; }
int nd_ssh_read(nd_ssh *s, void *buf, size_t cap) { (void)s; (void)buf; (void)cap; return -1; }
int nd_ssh_write(nd_ssh *s, const void *buf, size_t len) { (void)s; (void)buf; (void)len; return -1; }
void nd_ssh_close(nd_ssh *s) { (void)s; }
void nd_ssh_cancel(nd_ssh *s) { (void)s; }
nd_ssh *nd_ssh_get(nd_ssh *s) { return s; }
void nd_ssh_put(nd_ssh *s) { (void)s; }
const char *nd_ssh_fingerprint(nd_ssh *s) { (void)s; return ""; }

#endif
