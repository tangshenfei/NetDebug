#ifndef ND_FTP_CLIENT_H
#define ND_FTP_CLIENT_H

#include "net/addr.h"
#include "net/planner.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char      name[512];
    int       is_dir;
    long long size;
    char      perms[16];
    char      modify[32];
} nd_ftp_entry;

typedef struct nd_ftp nd_ftp;

nd_ftp *nd_ftp_connect(const char *host, uint16_t port, nd_family family,
                       int timeout_ms, char *err, size_t errlen);
int  nd_ftp_login(nd_ftp *f, const char *user, const char *pass,
                  char *err, size_t errlen);
void nd_ftp_close(nd_ftp *f);

int  nd_ftp_pwd(nd_ftp *f, char *out, size_t cap, char *err, size_t errlen);
int  nd_ftp_cwd(nd_ftp *f, const char *path, char *err, size_t errlen);
int  nd_ftp_list(nd_ftp *f, const char *path, nd_ftp_entry **out, size_t *n,
                 char *err, size_t errlen);
int  nd_ftp_mkdir(nd_ftp *f, const char *path, char *err, size_t errlen);
int  nd_ftp_rmdir(nd_ftp *f, const char *path, char *err, size_t errlen);
int  nd_ftp_delete(nd_ftp *f, const char *path, char *err, size_t errlen);
int  nd_ftp_rename(nd_ftp *f, const char *from, const char *to,
                   char *err, size_t errlen);
int  nd_ftp_download(nd_ftp *f, const char *remote, const char *local,
                     uint64_t *bytes, char *err, size_t errlen);
int  nd_ftp_upload(nd_ftp *f, const char *local, const char *remote,
                   uint64_t *bytes, char *err, size_t errlen);

#endif /* ND_FTP_CLIENT_H */
