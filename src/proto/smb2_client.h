#ifndef ND_SMB2_CLIENT_H
#define ND_SMB2_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     name[512];
    int      is_dir;
    uint64_t size;
    int64_t  mtime;
} nd_smb_entry;

typedef struct nd_smb2 nd_smb2;

nd_smb2 *nd_smb2_connect(const char *host, const char *share, const char *user,
                         const char *password, const char *domain,
                         char *err, size_t errlen);
void nd_smb2_close(nd_smb2 *s);

int nd_smb2_list(nd_smb2 *s, const char *path, nd_smb_entry **out, size_t *n,
                 char *err, size_t errlen);
int nd_smb2_download(nd_smb2 *s, const char *remote, const char *local,
                     uint64_t *bytes, char *err, size_t errlen);
int nd_smb2_upload(nd_smb2 *s, const char *local, const char *remote,
                   uint64_t *bytes, char *err, size_t errlen);
int nd_smb2_mkdir(nd_smb2 *s, const char *path, char *err, size_t errlen);
int nd_smb2_unlink(nd_smb2 *s, const char *path, char *err, size_t errlen);
int nd_smb2_rmdir(nd_smb2 *s, const char *path, char *err, size_t errlen);
const char *nd_smb2_error(nd_smb2 *s);

#endif
