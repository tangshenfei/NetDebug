#ifndef ND_BASE64_H
#define ND_BASE64_H

#include <stddef.h>

int nd_base64_encode(const void *data, size_t len, char *out, size_t cap);
int nd_base64_decode(const char *in, void *out, size_t cap); /* 字节数，-1 非法 */

#endif /* ND_BASE64_H */
