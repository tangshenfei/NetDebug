#ifndef ND_ERROR_H
#define ND_ERROR_H

/* 统一错误码：1xxx 参数/鉴权，2xxx 网络，3xxx 绑定，4xxx 协议，5xxx I/O */
enum {
    ND_OK = 0,

    ND_E_PARAM          = 1000,
    ND_E_PARAM_PORT     = 1001,
    ND_E_PARAM_FAMILY   = 1002,
    ND_E_PARAM_PATH     = 1003,
    ND_E_PARAM_IFACE    = 1004,

    ND_E_AUTH           = 1100,
    ND_E_AUTH_TOKEN     = 1101,
    ND_E_AUTH_ORIGIN    = 1102,
    ND_E_AUTH_HOST      = 1103,

    ND_E_NET            = 2000,
    ND_E_RESOLVE        = 2001,
    ND_E_CONNECT_TIMEOUT = 2002,
    ND_E_CONNECT_ALL    = 2003,

    ND_E_SRC            = 2100,
    ND_E_SRC_NOADDR     = 2101,
    ND_E_SRC_BIND       = 2102,

    ND_E_SERIAL         = 2200,
    ND_E_SERIAL_OPEN    = 2201,
    ND_E_SERIAL_IO      = 2202,

    ND_E_BIND           = 3000,
    ND_E_BIND_ALL       = 3001,
    ND_E_BIND_PERM      = 3002,
    ND_E_BIND_INUSE     = 3003,
    ND_E_BIND_FAMILY    = 3004,
    ND_E_BIND_NOADDR    = 3005,

    ND_E_PROTO          = 4000,
    ND_E_PROTO_PARSE    = 4001,
    ND_E_PROTO_AUTH     = 4002,

    ND_E_IO             = 5000,
    ND_E_IO_DISK        = 5001,
    ND_E_IO_THREAD      = 5002,
    ND_E_INTERNAL       = 5003
};

const char *nd_error_str(int code);

#endif /* ND_ERROR_H */
