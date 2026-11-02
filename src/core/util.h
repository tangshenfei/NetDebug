#ifndef ND_UTIL_H
#define ND_UTIL_H

#include <stddef.h>
#include <stdint.h>

size_t nd_strlcpy(char *dst, const char *src, size_t cap);
char  *nd_strdup(const char *s);
void   nd_trim(char *s);
int    nd_starts_with(const char *s, const char *prefix);
int    nd_ends_with(const char *s, const char *suffix);

size_t nd_hex_encode(const void *data, size_t len, char *out, size_t cap, int upper);
int    nd_hex_decode(const char *hex, void *out, size_t cap); /* 返回字节数，-1 = 非法 */

int    nd_random_bytes(void *buf, size_t n);
int    nd_utf8_valid(const void *data, size_t len);

#endif /* ND_UTIL_H */
