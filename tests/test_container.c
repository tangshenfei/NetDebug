#include "test_framework.h"

#include "core/fqueue.h"
#include "core/ring.h"
#include "core/thread.h"

#include <stdio.h>
#include <stdlib.h>

static void case_ring_basic(void)
{
    nd_ring r;
    char buf[16];

    BEGIN_CASE("环形缓冲 基本读写");

    CHECK_INT(nd_ring_init(&r, 8), 0);
    CHECK_INT(nd_ring_space(&r), 8);
    CHECK_INT(nd_ring_used(&r), 0);

    CHECK_INT(nd_ring_write(&r, "abcdef", 6), 6);
    CHECK_INT(nd_ring_used(&r), 6);
    CHECK_INT(nd_ring_space(&r), 2);

    /* peek 不消费数据 */
    memset(buf, 0, sizeof(buf));
    CHECK_INT(nd_ring_peek(&r, buf, sizeof(buf)), 6);
    CHECK(memcmp(buf, "abcdef", 6) == 0);
    CHECK_INT(nd_ring_used(&r), 6);

    CHECK_INT(nd_ring_read(&r, buf, 2), 2);
    CHECK(memcmp(buf, "ab", 2) == 0);
    CHECK_INT(nd_ring_used(&r), 4);

    nd_ring_free(&r);
    CHECK(r.buf == NULL);
    CHECK_INT(nd_ring_used(&r), 0);

    END_CASE();
}

static void case_ring_wrap(void)
{
    nd_ring r;
    char buf[16];

    BEGIN_CASE("环形缓冲 环绕与满/空边界");

    CHECK_INT(nd_ring_init(&r, 8), 0);

    CHECK_INT(nd_ring_write(&r, "abcdef", 6), 6);
    CHECK_INT(nd_ring_write(&r, "ghij", 4), 2); /* 只能写入 2 字节 */
    CHECK_INT(nd_ring_used(&r), 8);
    CHECK_INT(nd_ring_space(&r), 0);
    CHECK_INT(nd_ring_write(&r, "x", 1), 0);    /* 已满 */

    CHECK_INT(nd_ring_read(&r, buf, 5), 5);
    CHECK(memcmp(buf, "abcde", 5) == 0);

    /* 此时还剩 "fgh"（3 字节），再写 3 字节会跨过环形尾部 */
    CHECK_INT(nd_ring_write(&r, "klm", 3), 3);
    CHECK_INT(nd_ring_used(&r), 6);

    CHECK_INT(nd_ring_read(&r, buf, sizeof(buf)), 6);
    CHECK(memcmp(buf, "fghklm", 6) == 0);
    CHECK_INT(nd_ring_used(&r), 0);

    /* 空缓冲读取返回 0 */
    CHECK_INT(nd_ring_read(&r, buf, 4), 0);

    CHECK_INT(nd_ring_write(&r, "1234", 4), 4);
    nd_ring_clear(&r);
    CHECK_INT(nd_ring_used(&r), 0);

    nd_ring_free(&r);

    /* 容量下限：cap < 2 会被提升到 2 */
    CHECK_INT(nd_ring_init(&r, 0), 0);
    CHECK_INT(nd_ring_space(&r), 2);
    CHECK_INT(nd_ring_write(&r, "abc", 3), 2);
    nd_ring_free(&r);

    END_CASE();
}

static void case_fqueue_basic(void)
{
    nd_fqueue q;
    nd_frame *f;

    BEGIN_CASE("帧队列 入队/出队顺序");

    CHECK_INT(nd_fqueue_init(&q, 1024), 0);
    CHECK_INT(nd_fqueue_bytes(&q), 0);

    CHECK_INT(nd_fqueue_push(&q, "aa", 2, 0), 0);
    CHECK_INT(nd_fqueue_push(&q, "", 0, 0), 0); /* 空帧直接成功，不入队 */
    CHECK_INT(nd_fqueue_bytes(&q), 2);

    f = nd_fqueue_pop(&q, 0);
    CHECK(f != NULL);
    if (f) {
        CHECK_INT(f->len, 2);
        CHECK(memcmp(f->data, "aa", 2) == 0);
        free(f);
    }
    CHECK_INT(nd_fqueue_bytes(&q), 0);

    /* FIFO 顺序 + 帧边界完整 */
    CHECK_INT(nd_fqueue_push(&q, "one", 3, 0), 0);
    CHECK_INT(nd_fqueue_push(&q, "two", 3, 0), 0);
    f = nd_fqueue_pop(&q, 0);
    CHECK(f && f->len == 3 && memcmp(f->data, "one", 3) == 0);
    free(f);
    f = nd_fqueue_pop(&q, 0);
    CHECK(f && f->len == 3 && memcmp(f->data, "two", 3) == 0);
    free(f);

    /* 空队列超时返回 NULL */
    CHECK(nd_fqueue_pop(&q, 10) == NULL);

    nd_fqueue_destroy(&q);

    END_CASE();
}

static void case_fqueue_limit(void)
{
    nd_fqueue q;
    nd_frame *f;

    BEGIN_CASE("帧队列 容量上限与关闭语义");

    CHECK_INT(nd_fqueue_init(&q, 4), 0);

    CHECK_INT(nd_fqueue_push(&q, "abc", 3, 0), 0);
    /* 3 + 2 > 4，等待超时后拒绝 */
    CHECK_INT(nd_fqueue_push(&q, "de", 2, 50), -1);
    CHECK_INT(nd_fqueue_bytes(&q), 3);

    nd_fqueue_close(&q);

    /* 关闭后拒绝新帧，但已入队的仍可取走 */
    CHECK_INT(nd_fqueue_push(&q, "x", 1, 0), -2);

    f = nd_fqueue_pop(&q, 0);
    CHECK(f && f->len == 3 && memcmp(f->data, "abc", 3) == 0);
    free(f);

    /* 关闭且为空：立即返回 NULL，不阻塞 */
    CHECK(nd_fqueue_pop(&q, 0) == NULL);

    nd_fqueue_destroy(&q);

    END_CASE();
}

static void *fqueue_producer(void *arg)
{
    nd_fqueue *q = (nd_fqueue *)arg;
    char buf[16];
    int i;

    for (i = 0; i < 200; i++) {
        snprintf(buf, sizeof(buf), "frame-%03d", i);
        if (nd_fqueue_push(q, buf, 9, -1) != 0)
            break;
    }
    nd_fqueue_close(q);
    return NULL;
}

static void case_fqueue_threaded(void)
{
    nd_fqueue q;
    nd_thread th;
    int got = 0;
    int ordered = 1;

    BEGIN_CASE("帧队列 多线程生产者/消费者");

    CHECK_INT(nd_fqueue_init(&q, 1 << 20), 0);
    CHECK_INT(nd_thread_start(&th, fqueue_producer, &q, 128 * 1024), 0);

    for (;;) {
        nd_frame *f = nd_fqueue_pop(&q, -1);
        char expect[16];
        if (!f)
            break;
        snprintf(expect, sizeof(expect), "frame-%03d", got);
        if (f->len != 9 || memcmp(f->data, expect, 9) != 0)
            ordered = 0;
        free(f);
        got++;
        if (got > 500)
            break;
    }

    CHECK_INT(nd_thread_join(&th), 0);
    CHECK_INT(got, 200);
    CHECK(ordered);

    nd_fqueue_destroy(&q);

    END_CASE();
}

void suite_core_container(void)
{
    SUITE("core.container");
    case_ring_basic();
    case_ring_wrap();
    case_fqueue_basic();
    case_fqueue_limit();
    case_fqueue_threaded();
}
