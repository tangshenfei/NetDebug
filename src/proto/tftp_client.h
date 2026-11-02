#ifndef ND_TFTP_CLIENT_H
#define ND_TFTP_CLIENT_H

#include "net/planner.h"

#include <stddef.h>
#include <stdint.h>

#define ND_TFTP_DEFAULT_BLKSIZE 512
#define ND_TFTP_MAX_BLKSIZE     65464

int nd_tftp_download(const char *host, uint16_t port, nd_family family,
                     const char *remote, const char *local,
                     int blksize, int timeout_s, uint64_t *bytes,
                     char *err, size_t errlen);
int nd_tftp_upload(const char *host, uint16_t port, nd_family family,
                   const char *local, const char *remote,
                   int blksize, int timeout_s, uint64_t *bytes,
                   char *err, size_t errlen);

#endif /* ND_TFTP_CLIENT_H */
