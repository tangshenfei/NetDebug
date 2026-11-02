#ifndef ND_SERVER_H
#define ND_SERVER_H

#include <stddef.h>

int  nd_http_start(int port, const char *token, int *actual_port, char *err, size_t errlen);
void nd_http_stop(void);
int  nd_http_port(void);

/* api.c */
int  nd_api_handle(void *req); /* nd_http_req*，返回 1=已处理 */

#endif /* ND_SERVER_H */
