#ifndef ND_FQUEUE_H
#define ND_FQUEUE_H

#include "core/thread.h"

#include <stddef.h>

/* 带长度上限的帧队列：每条消息保持完整边界（WS 帧、待发送数据块） */
typedef struct nd_frame {
    struct nd_frame *next;
    size_t len;
    unsigned char data[];
} nd_frame;

typedef struct {
    nd_mutex lock;
    nd_cond cond;
    nd_frame *head;
    nd_frame *tail;
    size_t bytes;
    size_t limit;
    int closed;
    unsigned long dropped;
} nd_fqueue;

int  nd_fqueue_init(nd_fqueue *q, size_t limit);
void nd_fqueue_destroy(nd_fqueue *q);

/* 0 = 成功，-1 = 超时/队列满，-2 = 已关闭 */
int  nd_fqueue_push(nd_fqueue *q, const void *data, size_t len, int timeout_ms);

/* 返回帧（调用方 free），NULL = 超时或已关闭 */
nd_frame *nd_fqueue_pop(nd_fqueue *q, int timeout_ms);

void nd_fqueue_close(nd_fqueue *q);
size_t nd_fqueue_bytes(nd_fqueue *q);

#endif /* ND_FQUEUE_H */
