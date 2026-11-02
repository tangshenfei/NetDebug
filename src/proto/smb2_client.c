#include "proto/smb2_client.h"
#include "core/error.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ND_HAVE_SMB2
#include <fcntl.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

struct nd_smb2 {
    struct smb2_context *ctx;
    char                 host[256];
    char                 share[128];
    char                 last_error[512];
};

static void set_err(nd_smb2 *s, char *err, size_t errlen, const char *prefix)
{
    const char *e = s && s->ctx ? smb2_get_error(s->ctx) : NULL;
    snprintf(err, errlen, "%s: %s", prefix, e ? e : "SMB2 错误");
    if (s)
        nd_strlcpy(s->last_error, err, sizeof(s->last_error));
}

nd_smb2 *nd_smb2_connect(const char *host, const char *share, const char *user,
                         const char *password, const char *domain,
                         char *err, size_t errlen)
{
    nd_smb2 *s = (nd_smb2 *)calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "内存不足");
        return NULL;
    }
    nd_strlcpy(s->host, host, sizeof(s->host));
    nd_strlcpy(s->share, share, sizeof(s->share));

    s->ctx = smb2_init_context();
    if (!s->ctx) {
        snprintf(err, errlen, "初始化 SMB2 上下文失败");
        free(s);
        return NULL;
    }
    smb2_set_user(s->ctx, user);
    if (password)
        smb2_set_password(s->ctx, password);
    if (domain && domain[0])
        smb2_set_domain(s->ctx, domain);
    smb2_set_security_mode(s->ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);

    if (smb2_connect_share(s->ctx, host, share, user) < 0) {
        set_err(s, err, errlen, "连接共享失败");
        smb2_destroy_context(s->ctx);
        free(s);
        return NULL;
    }
    return s;
}

void nd_smb2_close(nd_smb2 *s)
{
    if (!s)
        return;
    if (s->ctx) {
        smb2_disconnect_share(s->ctx);
        smb2_destroy_context(s->ctx);
        s->ctx = NULL;
    }
    free(s);
}

const char *nd_smb2_error(nd_smb2 *s)
{
    return s ? s->last_error : "";
}

static const char *rel_path(const char *p)
{
    while (p && *p == '/')
        p++;
    return p ? p : "";
}

int nd_smb2_list(nd_smb2 *s, const char *path, nd_smb_entry **out, size_t *n,
                 char *err, size_t errlen)
{
    struct smb2dir *dir;
    struct smb2dirent *ent;
    nd_smb_entry *arr = NULL;
    size_t cnt = 0, cap = 0;
    const char *p = rel_path(path);

    *out = NULL;
    *n = 0;
    dir = smb2_opendir(s->ctx, p);
    if (!dir) {
        set_err(s, err, errlen, "打开目录失败");
        return ND_E_PROTO;
    }
    while ((ent = smb2_readdir(s->ctx, dir))) {
        if (!strcmp(ent->name, ".") || !strcmp(ent->name, ".."))
            continue;
        if (cnt == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            nd_smb_entry *na = (nd_smb_entry *)realloc(arr, ncap * sizeof(*na));
            if (!na) {
                free(arr);
                smb2_closedir(s->ctx, dir);
                snprintf(err, errlen, "内存不足");
                return ND_E_INTERNAL;
            }
            arr = na;
            cap = ncap;
        }
        memset(&arr[cnt], 0, sizeof(arr[cnt]));
        nd_strlcpy(arr[cnt].name, ent->name, sizeof(arr[cnt].name));
        arr[cnt].is_dir = ent->st.smb2_type == SMB2_TYPE_DIRECTORY;
        arr[cnt].size = ent->st.smb2_size;
        arr[cnt].mtime = ent->st.smb2_mtime;
        cnt++;
    }
    smb2_closedir(s->ctx, dir);
    *out = arr;
    *n = cnt;
    return ND_OK;
}

int nd_smb2_download(nd_smb2 *s, const char *remote, const char *local,
                     uint64_t *bytes, char *err, size_t errlen)
{
    struct smb2fh *fh;
    FILE *fp;
    uint64_t total = 0;
    int rc = ND_E_IO;

    *bytes = 0;
    fh = smb2_open(s->ctx, rel_path(remote), O_RDONLY);
    if (!fh) {
        set_err(s, err, errlen, "打开远端文件失败");
        return ND_E_PROTO;
    }
    fp = fopen(local, "wb");
    if (!fp) {
        smb2_close(s->ctx, fh);
        snprintf(err, errlen, "无法写入本地文件");
        return ND_E_IO;
    }
    for (;;) {
        char buf[65536];
        int n = smb2_read(s->ctx, fh, (uint8_t *)buf, sizeof(buf));
        if (n > 0) {
            if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
                snprintf(err, errlen, "写本地文件失败");
                goto done;
            }
            total += (uint64_t)n;
            continue;
        }
        if (n == 0)
            break;
        set_err(s, err, errlen, "读取远端文件失败");
        goto done;
    }
    rc = ND_OK;
done:
    fclose(fp);
    smb2_close(s->ctx, fh);
    *bytes = total;
    return rc;
}

int nd_smb2_upload(nd_smb2 *s, const char *local, const char *remote,
                   uint64_t *bytes, char *err, size_t errlen)
{
    struct smb2fh *fh;
    FILE *fp;
    uint64_t total = 0;
    int rc = ND_E_IO;

    *bytes = 0;
    fp = fopen(local, "rb");
    if (!fp) {
        snprintf(err, errlen, "无法读取本地文件");
        return ND_E_IO;
    }
    fh = smb2_open(s->ctx, rel_path(remote), O_WRONLY | O_CREAT | O_TRUNC);
    if (!fh) {
        fclose(fp);
        set_err(s, err, errlen, "创建远端文件失败");
        return ND_E_PROTO;
    }
    for (;;) {
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        int w;
        if (n == 0)
            break;
        w = smb2_write(s->ctx, fh, (const uint8_t *)buf, n);
        if (w < 0 || (size_t)w != n) {
            set_err(s, err, errlen, "写入远端文件失败");
            goto done;
        }
        total += n;
    }
    rc = ND_OK;
done:
    fclose(fp);
    smb2_close(s->ctx, fh);
    *bytes = total;
    return rc;
}

int nd_smb2_mkdir(nd_smb2 *s, const char *path, char *err, size_t errlen)
{
    if (smb2_mkdir(s->ctx, rel_path(path)) < 0) {
        set_err(s, err, errlen, "创建目录失败");
        return ND_E_PROTO;
    }
    return ND_OK;
}

int nd_smb2_unlink(nd_smb2 *s, const char *path, char *err, size_t errlen)
{
    if (smb2_unlink(s->ctx, rel_path(path)) < 0) {
        set_err(s, err, errlen, "删除文件失败");
        return ND_E_PROTO;
    }
    return ND_OK;
}

int nd_smb2_rmdir(nd_smb2 *s, const char *path, char *err, size_t errlen)
{
    if (smb2_rmdir(s->ctx, rel_path(path)) < 0) {
        set_err(s, err, errlen, "删除目录失败");
        return ND_E_PROTO;
    }
    return ND_OK;
}

#else /* !ND_HAVE_SMB2 */

struct nd_smb2 { int dummy; };

nd_smb2 *nd_smb2_connect(const char *host, const char *share, const char *user,
                         const char *password, const char *domain,
                         char *err, size_t errlen)
{
    (void)host; (void)share; (void)user; (void)password; (void)domain;
    snprintf(err, errlen, "当前构建未启用 SMB2");
    return NULL;
}
void nd_smb2_close(nd_smb2 *s) { (void)s; }
const char *nd_smb2_error(nd_smb2 *s) { (void)s; return ""; }
int nd_smb2_list(nd_smb2 *s, const char *p, nd_smb_entry **o, size_t *n, char *e, size_t l)
{ (void)s; (void)p; *o = NULL; *n = 0; snprintf(e, l, "SMB2 未启用"); return ND_E_PROTO; }
int nd_smb2_download(nd_smb2 *s, const char *r, const char *l, uint64_t *b, char *e, size_t le)
{ (void)s; (void)r; (void)l; *b = 0; snprintf(e, le, "SMB2 未启用"); return ND_E_PROTO; }
int nd_smb2_upload(nd_smb2 *s, const char *l, const char *r, uint64_t *b, char *e, size_t le)
{ (void)s; (void)r; (void)l; *b = 0; snprintf(e, le, "SMB2 未启用"); return ND_E_PROTO; }
int nd_smb2_mkdir(nd_smb2 *s, const char *p, char *e, size_t le)
{ (void)s; (void)p; snprintf(e, le, "SMB2 未启用"); return ND_E_PROTO; }
int nd_smb2_unlink(nd_smb2 *s, const char *p, char *e, size_t le)
{ (void)s; (void)p; snprintf(e, le, "SMB2 未启用"); return ND_E_PROTO; }
int nd_smb2_rmdir(nd_smb2 *s, const char *p, char *e, size_t le)
{ (void)s; (void)p; snprintf(e, le, "SMB2 未启用"); return ND_E_PROTO; }

#endif
