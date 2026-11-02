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

#ifdef ND_HAVE_SSH
#include <libssh2.h>

struct nd_ssh {
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    int              sock;
    char             fingerprint[64];
    char             password[128];
};

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
        size_t n = strlen(s->password) + 1;
        responses[i].text = (char *)malloc(n);
        if (!responses[i].text)
            continue;
        memcpy(responses[i].text, s->password, n);
        responses[i].length = (unsigned int)(n - 1);
    }
    (void)prompts;
}

static void set_err(nd_ssh *s, char *err, size_t errlen, const char *prefix)
{
    char *msg = NULL;
    int len = 0;
    libssh2_session_last_error(s ? s->session : NULL, &msg, &len, 0);
    snprintf(err, errlen, "%s: %s", prefix, msg && len ? msg : "SSH 错误");
}

nd_ssh *nd_ssh_connect(const char *host, uint16_t port, nd_family family,
                       const char *user, const char *password,
                       const char *key_path, int compat,
                       int timeout_ms, int *sock_out,
                       char *err, size_t errlen)
{
    nd_addr addrs[16];
    size_t n = 0;
    int fd = -1;
    int sys = 0;
    nd_ssh *s;
    unsigned char *hash;

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
        return NULL;
    }
    s->sock = fd;
    nd_strlcpy(s->password, password ? password : "", sizeof(s->password));

    if (libssh2_init(0) != 0) {
        snprintf(err, errlen, "libssh2 初始化失败");
        goto fail;
    }
    s->session = libssh2_session_init();
    if (!s->session) {
        snprintf(err, errlen, "创建 SSH 会话失败");
        goto fail;
    }
    libssh2_session_set_blocking(s->session, 1);
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
    }
    if (libssh2_session_handshake(s->session, fd) != 0) {
        set_err(s, err, errlen, "SSH 握手失败");
        goto fail;
    }
    hash = (unsigned char *)libssh2_hostkey_hash(s->session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash) {
        size_t pos = 0;
        for (int i = 0; i < 32 && pos + 3 < sizeof(s->fingerprint); i++)
            pos += (size_t)snprintf(s->fingerprint + pos, sizeof(s->fingerprint) - pos,
                                    "%02x", hash[i]);
    }

    if (key_path && key_path[0]) {
        if (libssh2_userauth_publickey_fromfile(s->session, user, NULL, key_path,
                                                s->password[0] ? s->password : NULL) != 0) {
            set_err(s, err, errlen, "公钥认证失败");
            goto fail;
        }
    } else {
        if (libssh2_userauth_password(s->session, user, s->password) != 0) {
            /* 尝试 keyboard-interactive */
            if (libssh2_userauth_keyboard_interactive(s->session, user,
                    kbd_callback) != 0) {
                set_err(s, err, errlen, "密码认证失败");
                goto fail;
            }
        }
    }

    *sock_out = fd;
    return s;

fail:
    if (s) {
        if (s->session) {
            libssh2_session_disconnect(s->session, "bye");
            libssh2_session_free(s->session);
            s->session = NULL;
        }
        free(s);
    }
    nd_net_close(fd);
    return NULL;
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
    if (!s || !s->channel)
        return -1;
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
    if (!s || !s->channel)
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
    free(s);
}

const char *nd_ssh_fingerprint(nd_ssh *s)
{
    return s ? s->fingerprint : "";
}

#else /* !ND_HAVE_SSH */

struct nd_ssh { int dummy; };

nd_ssh *nd_ssh_connect(const char *host, uint16_t port, nd_family family,
                       const char *user, const char *password,
                       const char *key_path, int compat,
                       int timeout_ms, int *sock_out,
                       char *err, size_t errlen)
{
    (void)host; (void)port; (void)family; (void)user; (void)password;
    (void)key_path; (void)compat; (void)timeout_ms;
    *sock_out = -1;
    snprintf(err, errlen, "当前构建未启用 SSH（需要 libssh2）");
    return NULL;
}
int nd_ssh_shell(nd_ssh *s, int cols, int rows, char *err, size_t errlen)
{ (void)s; (void)cols; (void)rows; snprintf(err, errlen, "SSH 未启用"); return -1; }
int nd_ssh_read(nd_ssh *s, void *buf, size_t cap) { (void)s; (void)buf; (void)cap; return -1; }
int nd_ssh_write(nd_ssh *s, const void *buf, size_t len) { (void)s; (void)buf; (void)len; return -1; }
void nd_ssh_close(nd_ssh *s) { (void)s; }
const char *nd_ssh_fingerprint(nd_ssh *s) { (void)s; return ""; }

#endif
