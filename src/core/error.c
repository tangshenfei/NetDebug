#include "core/error.h"

const char *nd_error_str(int code)
{
    switch (code) {
    case ND_OK:                return "ok";
    case ND_E_PARAM:           return "参数错误";
    case ND_E_PARAM_PORT:      return "端口必须是 0-65535";
    case ND_E_PARAM_FAMILY:    return "地址族与绑定方式不匹配";
    case ND_E_PARAM_PATH:      return "路径非法";
    case ND_E_PARAM_IFACE:     return "网卡不存在或已失效";
    case ND_E_AUTH:            return "未授权";
    case ND_E_AUTH_TOKEN:      return "token 缺失或错误";
    case ND_E_AUTH_ORIGIN:     return "Origin 不被允许";
    case ND_E_AUTH_HOST:       return "Host 不被允许";
    case ND_E_NET:             return "网络错误";
    case ND_E_RESOLVE:         return "域名解析失败";
    case ND_E_CONNECT_TIMEOUT: return "连接超时";
    case ND_E_CONNECT_ALL:     return "所有地址均连接失败";
    case ND_E_SRC:             return "源地址绑定失败";
    case ND_E_SRC_NOADDR:      return "所选网卡没有该地址族的地址";
    case ND_E_SRC_BIND:        return "源地址 bind 失败";
    case ND_E_SERIAL:          return "串口错误";
    case ND_E_SERIAL_OPEN:     return "串口打开失败";
    case ND_E_SERIAL_IO:       return "串口读写失败";
    case ND_E_BIND:            return "绑定失败";
    case ND_E_BIND_ALL:        return "所有地址均绑定失败";
    case ND_E_BIND_PERM:       return "权限不足（Linux 上低于 1024 的端口需要 root 或 CAP_NET_BIND_SERVICE）";
    case ND_E_BIND_INUSE:      return "端口已被占用";
    case ND_E_BIND_FAMILY:     return "绑定具体地址时不能选择双栈";
    case ND_E_BIND_NOADDR:     return "没有可用地址";
    case ND_E_PROTO:           return "协议错误";
    case ND_E_PROTO_PARSE:     return "报文解析失败";
    case ND_E_PROTO_AUTH:      return "协议认证失败";
    case ND_E_IO:              return "I/O 错误";
    case ND_E_IO_DISK:         return "磁盘错误";
    case ND_E_IO_THREAD:       return "线程创建失败";
    case ND_E_INTERNAL:        return "内部错误";
    default:                   return "未知错误";
    }
}
