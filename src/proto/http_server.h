#ifndef ND_HTTP_SERVER_H
#define ND_HTTP_SERVER_H

#include <stdint.h>

struct nd_service;

#define ND_HTTP_ROOT_MAX 512

typedef struct {
    char root[ND_HTTP_ROOT_MAX];
    int  echo;
    int  speedtest;
} nd_http_srv_cfg;

/* 接管 fd，在独立线程里处理一个 HTTP 请求并关闭连接。 */
int nd_http_server_start(int fd, struct nd_service *svc, const nd_http_srv_cfg *cfg);

#endif
