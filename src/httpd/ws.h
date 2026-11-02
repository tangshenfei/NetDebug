#ifndef ND_WS_H
#define ND_WS_H

#include <stddef.h>
#include <stdint.h>

/* 接管 fd：完成握手后进入读写循环，返回时已关闭 fd */
int  nd_ws_serve(int fd, const char *sec_ws_key, uint64_t sub_conn);

/* 广播一条连接数据（dir: 0=rx 1=tx） */
void nd_ws_broadcast_data(uint64_t conn_id, int dir, const void *data, size_t len);

/* 连接状态变化（1=open，0=closed） */
void nd_ws_state_event(uint64_t conn_id, int state);

/* 全局事件（JSON 字符串） */
void nd_ws_event_json(const char *json);

int  nd_ws_client_count(void);

#endif /* ND_WS_H */
