#ifndef ND_HTTP_CLIENT_H
#define ND_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define ND_HTTP_MAX_HEADERS 64
#define ND_HTTP_MAX_BODY    (8u * 1024u * 1024u)

typedef struct {
    char name[128];
    char value[1024];
} nd_http_req_hdr;

typedef struct {
    char name[128];
    char value[1024];
} nd_http_resp_hdr;

typedef struct {
    int    status;
    char   reason[128];
    char   version[16];
    nd_http_resp_hdr headers[ND_HTTP_MAX_HEADERS];
    int    n_headers;

    char  *body;
    size_t body_len;
    int    truncated;
    char   content_encoding[64];

    uint64_t connect_ms;
    uint64_t first_byte_ms;
    uint64_t total_ms;

    char  *req_head;
    size_t req_head_len;
    char  *resp_head;
    size_t resp_head_len;

    int    redirects;
    char   final_url[2048];
} nd_http_result;

/* 返回 ND_OK 或错误码；失败时 err 有可读原因 */
int  nd_http_do(const char *method, const char *url,
                const nd_http_req_hdr *hdrs, int n_hdrs,
                const void *body, size_t body_len,
                int timeout_ms, int follow_redirects,
                nd_http_result *out, char *err, size_t errlen);

void nd_http_result_free(nd_http_result *r);

#endif /* ND_HTTP_CLIENT_H */
