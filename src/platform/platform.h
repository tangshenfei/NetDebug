#ifndef ND_PLATFORM_H
#define ND_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  nd_platform_init(void);
void nd_platform_cleanup(void);

/* Unix 纪元毫秒（用于前端时间戳） */
uint64_t nd_wall_ms(void);

int  nd_platform_random(void *buf, size_t n);

/* 0 = 本进程持有实例锁；1 = 已有实例在运行；-1 = 错误 */
int  nd_single_instance(const char *name);

const char *nd_config_dir(void);           /* 已创建，带结尾分隔符 */
int  nd_mkdir_p(const char *path);
int  nd_path_join(char *out, size_t cap, const char *a, const char *b);

int  nd_write_file(const char *path, const void *data, size_t len);
int  nd_read_file(const char *path, void **out, size_t *len); /* 调用方 free */

/* 弹出系统文件夹选择器。1=已选择并写入 out，0=取消，-1=不可用。 */
int  nd_pick_folder(char *out, size_t cap);

/* 弹出系统文件选择器。返回值同上。 */
int  nd_pick_file(char *out, size_t cap);

/* 系统错误码 → 可读文本（Win: FormatMessage，POSIX: strerror） */
char *nd_sys_error(int err, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ND_PLATFORM_H */
