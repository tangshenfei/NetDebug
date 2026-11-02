#ifndef ND_STORE_H
#define ND_STORE_H

/* 服务定义持久化：服务列表存于配置目录 services.json，启动时恢复（不自动启动） */
void nd_store_load_services(void);
void nd_store_save_services(void);

#endif /* ND_STORE_H */
