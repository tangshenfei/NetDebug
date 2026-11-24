#ifndef ND_STORE_H
#define ND_STORE_H

#include <stddef.h>

/* 服务定义持久化：服务列表存于配置目录 services.json，启动时恢复（不自动启动） */
void nd_store_load_services(void);
void nd_store_save_services(void);

/* 快捷发送的预置命令：全局一份，存于配置目录 presets.json */
#define ND_MAX_PRESETS 24
#define ND_PRESET_LEN  64

size_t nd_presets_list(char out[][ND_PRESET_LEN], size_t cap);
int    nd_presets_save(const char *const *items, size_t n);

#endif /* ND_STORE_H */
