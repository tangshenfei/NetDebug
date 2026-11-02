/*
 * glibc >= 2.38 C23 兼容垫片
 *
 * 新版 glibc 在 _GNU_SOURCE 下会把 strtol/strtoul/strtoull 重定向到
 * __isoc23_strtol 等符号，这些符号要求运行时 glibc >= 2.38。
 * 我们并不使用 C23 的 0b 前缀或 ' 分隔符语义，因此关掉重定向，
 * 让产出的二进制继续能在 glibc 2.34+（Ubuntu 22.04 / Debian 12）上运行。
 *
 * 该文件通过 CMake 的 -include 强制包含在每个 TU 的最前面。
 */
#ifndef ND_GLIBC_C23_COMPAT_H
#define ND_GLIBC_C23_COMPAT_H

#include <features.h>

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38))
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#endif

#endif
