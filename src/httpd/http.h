#ifndef ND_HTTP_H
#define ND_HTTP_H

#include <stddef.h>

typedef struct nd_http_hdr {
    char name[64];
    char value[768];
} nd_http_hdr;

typedef struct nd_http_req {
    int    fd;
    char   method[8];
    char   path[512];
    char   query[512];
    char   version[16];
    nd_http_hdr headers[48];
    int    n_headers;
    char  *body;
    size_t body_len;
} nd_http_req;

int  nd_http_read_request(int fd, nd_http_req *req, char *err, size_t errlen);
void nd_http_free_request(nd_http_req *req);
const char *nd_http_header(const nd_http_req *req, const char *name);

void nd_http_respond(int fd, int status, const char *ctype, const void *body, size_t len);
void nd_http_json(int fd, int status, void *cjson_obj);
void nd_http_error_json(int fd, int status, int code, const char *detail);

int  nd_http_query_get(const char *query, const char *name, char *out, size_t cap);
void nd_http_url_decode(char *s);

#endif /* ND_HTTP_H */
