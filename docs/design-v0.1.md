# 详细设计 v0.1 —— M0 基线

> 输入：`requirements-v0.3.md`（D1–D11）
> 范围：**只设计 M0**（骨架 + 绑定层 + 原始 TCP/UDP + WS + 最小前端）。其余协议本轮只定接口，开工前各自补设计。
> 状态：待评审，未写代码。

---

## 0. M0 要打通的一条链路

```
选网卡 → 选地址族(v4/v6/双栈) → 测试绑定 → 启动服务 → 另一台机器连上 → 网页实时收发 → 停止并释放端口
```

这条链路打通，后面 HTTP/FTP/TFTP/Telnet 都只是"在 `nd_conn` 上换一个协议解析器"。所以 M0 的重点不是协议数量，而是**绑定层、连接抽象、数据通路**三件事。

---

## 1. 设计原则

1. **平台差异只允许出现在 `platform/`**。上层代码不出现 `#ifdef _WIN32`（编译期常量除外）。
2. **每个失败都能回答四个问题**：哪个地址、哪个端口、系统错误是什么、下一步怎么办。
3. **协议只依赖 `nd_conn` API**，不依赖 HTTP 层、不依赖全局状态。
4. **绑定规划是纯函数**：输入网卡快照 + 绑定描述，输出套接字计划。可以脱离真实网卡做单测。
5. **不做隐式回退**。双栈退化成两个套接字、部分地址绑定失败，都必须**明确标记并上报 UI**，不能悄悄成功。

---

## 2. 决策补记（本轮）

| 编号 | 结论 |
|---|---|
| D8 | Telnet 服务端要做，排在 M3 之后 |
| D9 | SMB 仅客户端、仅 SMB2/2.1/3.x |
| D10 | HTTP 服务端 Mock 后置，不进首版 |
| D11 | 客户端出站支持绑源网卡/源 IP，纳入 M0 绑定层 |

按默认值推进的 P1：新服务默认绑回环（带"允许局域网"开关）；Linux 默认高位端口；macOS 不在首版；先出绿色版。

---

## 3. 目录结构

```
netdebug/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  cmake/
    EmbedWebAssets.cmake        # web/dist → C 数组
  src/
    main.c                      # 启动、单实例、托盘、开浏览器
    core/
      log.c/h                   # 分级日志、按连接落盘
      error.c/h                 # 错误码 + 文案
      ring.c/h                  # SPSC 环形缓冲
      thread.c/h                # 线程/互斥/条件变量/原子封装
      queue.c/h                 # 通用队列（tx、WS 队列）
      json.c/h                  # cJSON 薄封装
      config.c/h                # 配置读写、路径
      event.c/h                 # 全局事件（推 WS）
    platform/
      net_win.c / net_posix.c   # socket 封装、超时连接、错误转换
      iface_win.c / iface_posix.c
      serial_win.c / serial_posix.c   # M1
      fs.c/h                    # 路径、大文件、UTF-8
      time.c/h                  # 单调时钟
      proc.c/h                  # 单实例锁、打开浏览器
    net/
      addr.c/h                  # 解析/格式化/归一化(::ffff:)/scope
      bind_spec.c/h             # 绑定描述校验与序列化
      planner.c/h               # 监听规划器（纯函数）
      listener.c/h              # 多套接字 accept、降级、停止
      resolver.c/h              # getaddrinfo 封装
      connect.c/h               # 带源绑定与超时的连接
    session/
      service.c/h               # 服务生命周期
      conn.c/h                  # 连接对象、rx/tx 线程
      session.c/h               # 客户端会话
      registry.c/h              # id 分配、全局注册表
    proto/
      proto.h                   # nd_proto_ops
      raw_tcp.c / raw_udp.c     # M0 唯一实现
      ftp/ tftp/ http/ ssh/ smb/ telnet/ serial/   # 后续
    httpd/
      server.c                  # civetweb 启动、静态资源
      api_net.c                 # interfaces / test-bind
      api_service.c             # 服务 CRUD 与启停
      api_session.c             # 会话
      auth.c                    # token、Host/Origin 校验
      ws.c                      # 连接流 + 全局事件 + pusher
  web/
    index.html  app.js  style.css  vendor/xterm.*
    dist/                       # 构建产物（可无构建）
  tests/
    test_planner.c  test_addr.c  test_ring.c  test_ftp_parse.c ...
  docs/
```

---

## 4. 模块依赖

```
main ──> httpd ──> api_* ──> session ──> proto ──> net ──> platform
                             │                     │
                             └──> core <───────────┘
web/ 只通过 HTTP/WS 契约交互，不参与 C 依赖
```

规则：`platform` 不反向依赖任何上层；`proto` 不直接调用 `httpd`；`core` 不依赖 `net`。

---

## 5. 核心数据结构

### 5.1 地址与网卡

```c
typedef struct {
    int      family;        /* AF_INET / AF_INET6 */
    int      is_mapped;     /* ::ffff:a.b.c.d */
    uint16_t port;          /* 主机序 */
    uint32_t scope_id;      /* IPv6 链路本地 */
    uint8_t  raw[16];       /* 网络序 */
} nd_addr;

typedef struct {
    char     id[80];        /* "win:{GUID}" 或 "linux:eth0" */
    char     name[128];     /* 展示名（Windows 上可能本地化） */
    uint32_t index;
    int      up;
    uint32_t mtu;
    size_t   n_addr;
    nd_addr  addrs[ND_MAX_IF_ADDRS];   /* 32 */
} nd_iface;

int nd_iface_list(nd_iface *out, size_t cap, size_t *n);   /* 快照 */
const nd_iface *nd_iface_find(const nd_iface *v, size_t n, const char *id);
```

要点：

- `id` 带平台前缀，避免 Windows 与 Linux 的 id 混用。
- 快照式接口：规划器只吃快照，测试时喂假数据。
- 归一化：`::ffff:192.168.1.5` 在 **API 输出时**还原成 v4 展示，内部保留原样以判断真实家族。

### 5.2 绑定描述与规划结果

```c
typedef enum { ND_FAM_V4 = 1, ND_FAM_V6 = 2, ND_FAM_DUAL = 3 } nd_family;
typedef enum { ND_BIND_ANY = 0, ND_BIND_IFACE = 1, ND_BIND_ADDR = 2 } nd_bind_kind;

typedef struct {
    nd_bind_kind kind;
    nd_family    family;
    uint16_t     port;          /* 0 = 系统分配 */
    char         iface_id[80];  /* kind == IFACE */
    nd_addr      addr;          /* kind == ADDR */
} nd_bind_spec;

typedef struct {
    int      fd;                /* 成功后为已 bind+listen 的套接字 */
    nd_addr  local;             /* bind 后回填，port 可能已变 */
    int      err;               /* 0 或 ND_E_* */
    int      sys_errno;
} nd_plan_slot;

typedef struct {
    nd_bind_spec spec;
    size_t       n;
    nd_plan_slot slot[ND_MAX_LISTEN_SOCKS];  /* 64 */
    int          degraded;      /* 部分地址失败，服务仍可用 */
    int          fatal;
    int          err;
} nd_plan;

int nd_plan_build(const nd_bind_spec *spec, const nd_iface *ifs, size_t n_ifs,
                  int do_bind, nd_plan *out);
void nd_plan_close(nd_plan *p);
```

### 5.3 连接

```c
typedef enum {
    ND_CONN_NEW, ND_CONN_OPEN, ND_CONN_CLOSING, ND_CONN_CLOSED
} nd_conn_state;

struct nd_conn {
    uint64_t            id;          /* 单调递增，不复用 */
    nd_proto_kind       kind;
    const nd_proto_ops *ops;

    int                 fd;          /* socket；串口实现里为 -1 */
    void               *link;        /* 串口句柄等 */

    nd_thread           th_rx, th_tx;
    nd_mutex            tx_lock;
    nd_cond             tx_cond;
    nd_queue            tx_q;        /* 待发送块 */

    nd_ring             rx;          /* 默认 256KB */
    atomic_bool         closing;

    nd_ws_sink          ws;          /* 浏览器推送队列 */
    FILE               *log;
    nd_stats            stats;
    void               *ctx;         /* 协议私有 */
    char                err[256];
};
```

### 5.4 服务

```c
struct nd_service {
    uint64_t       id;
    char           name[64];
    nd_proto_kind  kind;         /* FTP/TFTP/TELNET/RAW_TCP... */
    nd_bind_spec   bind;
    cJSON         *options;      /* root/passive_ports/allow/... */

    nd_plan        plan;
    nd_thread      th_accept;
    atomic_bool    stopping;

    nd_mutex       lock;
    nd_conn      **conns;
    size_t         n_conns, cap_conns;
    nd_stats       stats;
    char           last_error[256];
};
```

### 5.5 协议驱动

```c
typedef struct nd_proto_ops {
    const char *name;
    /* 连接建立后初始化协议状态；失败返回 ND_E_* */
    int  (*start)(nd_conn *c, const cJSON *cfg, char *err, size_t errlen);
    /* rx 线程调用；必须短小，不得阻塞等待对端应答 */
    int  (*on_rx)(nd_conn *c, const uint8_t *p, size_t n);
    /* 把一帧数据放进 tx_q 并唤醒 tx 线程 */
    int  (*send)(nd_conn *c, const uint8_t *p, size_t n, char *err, size_t errlen);
    void (*stop)(nd_conn *c);
    /* 协议私有控制：SFTP 列目录、FTP 发原始命令、TFTP 重传等 */
    int  (*ctl)(nd_conn *c, const char *op, const cJSON *in, cJSON **out);
} nd_proto_ops;
```

**约定**：需要等应答的协议（FTP/HTTP）不能在 `on_rx` 里等；`on_rx` 只解析并投递到 `reply_q`，API 线程在条件变量上等。这样 rx 线程永远不会被业务逻辑卡死。

### 5.6 缓冲与 WS 队列

```c
typedef struct { uint8_t *p; size_t n; } nd_chunk;

typedef struct {
    uint64_t    conn_id;
    nd_queue    q;          /* 已编码的 WS 帧 */
    size_t      bytes;      /* 当前排队字节 */
    size_t      limit;      /* 默认 8MB */
    uint32_t    dropped;    /* 丢弃计数 */
    uint64_t    last_warn_ms;
} nd_ws_sink;
```

---

## 6. 线程模型

| 线程 | 数量 | 职责 | 栈 |
|---|---|---|---|
| main | 1 | 初始化、托盘、等待退出 | 默认 |
| http | 2 | civetweb 事件循环（HTTP + WS 回调） | 默认 |
| ws_push | 1 | 消费各连接 WS 队列，批量写帧 | 256KB |
| accept | 每服务 1 | poll 多个 listen fd，accept → 建连接 | 256KB |
| rx | 每连接 1 | recv → `ops->on_rx` | 256KB |
| tx | 每连接 1 | 从 tx_q 取块 → send | 256KB |
| transfer | 每次文件传输 1 | 分块读写 + 进度（避免占住 HTTP 线程） | 256KB |

**为什么不用事件循环**：串口在 Windows 上不进入 `select/WSAPoll`，硬套事件循环要写两套 wait；而调试工具连接数是几十，2 线程/连接完全可接受。代价是线程多，收益是每个协议都能用最直白的阻塞写法。

**栈大小**：Windows 默认 1MB/线程，50 连接就是 50MB 虚拟地址（不是物理内存），但仍显式设 256KB；Linux 用 `pthread_attr_setstacksize`。

### 6.1 生命周期

```
Service:  STOPPED → STARTING → RUNNING ⇄ DEGRADED → STOPPING → STOPPED
Conn:     NEW → OPEN → CLOSING → CLOSED
Session:  IDLE → RESOLVING → CONNECTING → OPEN → CLOSING → CLOSED
```

`DEGRADED` 表示部分地址绑定失败（例如网卡只剩 v6 地址），服务继续跑，UI 必须显示哪条地址失败了。

### 6.2 关闭顺序（防死锁）

1. 置 `stopping`，关闭所有 listen fd → accept 线程的 `poll` 返回错误 → 退出。
2. join accept 线程。
3. 对每个连接：置 `closing` → `shutdown(fd, SHUT_RDWR)`（**必须**，否则阻塞的 `recv` 醒不过来）→ `pthread_cond_signal(tx_cond)`。
4. join rx / tx 线程（**不能 join 自己**；若在连接线程内触发关闭，只置标志，由外部收尸）。
5. `ops->stop` → 关 fd / 串口 → 刷日志 → 从 registry 摘除 → 释放。

全局退出顺序：停所有服务 → 停所有会话 → 停 WS pusher → 停 civetweb → 关日志。

---

## 7. 监听规划器

### 7.1 展开规则

| bind | family | 产物 |
|---|---|---|
| ANY | V4 | `0.0.0.0:port` |
| ANY | V6 | `[::]:port` + `IPV6_V6ONLY=1` |
| ANY | DUAL | `[::]:port` + `IPV6_V6ONLY=0`（单套接字吃两族） |
| IFACE | V4 | 该网卡**每个** v4 地址一个 |
| IFACE | V6 | 该网卡**每个** v6 地址一个（带 scope_id） |
| IFACE | DUAL | 上述两组全部 |
| ADDR | 由地址决定 | 一个；DUAL 组合直接判非法 |

**失败语义**：

- 显式指定 V4/V6 但该网卡一个此类地址都没有 → **fatal**（用户明确要求的东西不存在）。
- DUAL 只有一族有地址 → **degraded**，告警但不失败。
- 个别地址 bind 失败（`EADDRNOTAVAIL`，地址刚消失）→ 记入 slot，其余继续；全失败才 fatal。

### 7.2 伪代码

```c
int nd_plan_build(spec, ifs, n, do_bind, out) {
    if (spec->family == DUAL && spec->kind == ADDR) return ND_E_BIND_FAMILY;
    n = expand(spec, ifs, n, cand, ND_MAX);          /* 见 7.1 */
    if (n == 0) return ND_E_BIND_NO_ADDR;

    for (i = 0; i < n; i++) {
        fd = nd_socket(cand[i].family, SOCK_STREAM);
        nd_sockopt_listen(fd, cand[i].family, spec->family == DUAL);  /* 见 7.3 */
        if (do_bind) {
            if (nd_bind(fd, &cand[i]) < 0 || nd_listen(fd, 128) < 0) {
                slot[i].err = ND_E_BIND; slot[i].sys_errno = last_error();
                close(fd); continue;
            }
            nd_getsockname(fd, &slot[i].local);      /* port 0 → 实际端口 */
        }
        slot[i].fd = fd; slot[i].err = 0;
    }
    return tally(out);                                /* ok / degraded / fatal */
}
```

### 7.3 套接字选项（跨平台差异集中在这里）

```c
void nd_sockopt_listen(int fd, int addr_family, int dual) {
#ifdef _WIN32
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, sizeof one); /* 防端口劫持 */
#else
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);       /* 快速重启 */
#endif
    if (addr_family == AF_INET6) {
        int v6only = dual ? 0 : 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
    }
}
```

注意：

- `IPV6_V6ONLY` **必须显式设**。Linux 默认值受 `net.ipv6.bindv6only` 影响，BSD 系默认 1。
- Windows 上 `SO_REUSEADDR` 允许别的进程抢端口，服务端一律用 `SO_EXCLUSIVEADDRUSE`。
- 双栈套接字仅在 `ANY + DUAL` 时出现；绑定具体地址时不可能双栈。

### 7.4 冲突检测

两层：

1. **进程内预检**：遍历运行中的服务，判断 `(family, addr, port)` 是否重叠（含通配覆盖具体地址、双栈通配同时覆盖 v4/v6）。命中则直接返回"已被服务 X 占用"，比 `EADDRINUSE` 可读。
2. **OS 兜底**：真正 `bind` 的结果为准，把 `sys_errno` 一起带回。

### 7.5 test-bind

`POST /api/net/test-bind` 走同一条 `nd_plan_build(..., do_bind=1)`，拿到结果后**立即全关**，返回：

```json
{ "ok": false,
  "slots": [
    {"local": "0.0.0.0:9000", "ok": false, "code": 3100, "sys_errno": 10048, "hint": "端口已被占用"},
    {"local": "[::]:9000",    "ok": true}
  ] }
```

端口填 0 时，返回系统分配的端口（必须在 close 前 `getsockname`）。

### 7.6 地址漂移

- 每 10s 重采一次网卡快照（后续可换 `NotifyIpInterfaceChange` / netlink 事件）。
- 只对 `IFACE` 绑定做 diff：地址集合变了 → 关掉消失地址的 listen fd，给新地址补 bind。
- 已建立的连接**不迁移**（TCP 本来也迁不了），只在 UI 提示。
- 服务状态从 `RUNNING` 变 `DEGRADED` 或反向，推一条 WS 事件。

---

## 8. 客户端连接流程（D11）

```
1. 校验：源绑定地址家族 vs 目标家族（DUAL 允许任一）
2. 解析：getaddrinfo(host, port, family 过滤)；结果存会话，供 UI 展示
3. 逐个尝试（默认 1.5s 超时/地址）：
   fd = socket(addr.family)
   若指定源：bind(fd, 源地址)          ← 失败直接报 E_SRC_BIND，不换地址重试
   非阻塞 connect + poll 超时
   getsockopt(SO_ERROR) 判定
   成功：设回阻塞 + SO_RCVTIMEO/SO_SNDTIMEO + TCP_NODELAY
4. 全部失败：汇总成一条错误，列出每个地址的失败原因和耗时
```

细节：

- 源绑定选网卡时，取该网卡**与目标同家族**的第一个地址；没有则报"该网卡没有 IPv4 地址"，而不是偷偷换网卡。
- 连接日志必须记录**实际使用的地址**（不是域名），这是排障关键。
- `自动` 模式按 `getaddrinfo` 顺序，不做 Happy Eyeballs 竞速；顺序和结果都展示出来。

---

## 9. 数据通路与背压

```
浏览器 ──WS binary──> ws 回调 ──> conn->tx_q ──> tx 线程 ──> socket
socket ──> rx 线程 ──> ops->on_rx ──┬──> ws 队列 ──> pusher ──> 浏览器
                                     └──> 日志文件（带缓冲）
```

- 一次 `recv` 最多 64KB；`on_rx` 内只做解析 + 投递，不做磁盘同步写（日志走 64KB 缓冲，1s 或关闭时 flush）。
- WS 队列上限 8MB/连接。超限：丢最旧的块、`dropped++`，每 5s 最多发一条 `{"t":"warn","dropped":N}`。
- 文件传输进度事件限流 4Hz，避免把 WS 打满。
- 大文件全链路流式：64KB 块，不整文件读入内存。

---

## 10. WS 协议

一个连接一个端点，同时承载双向数据与状态：

```
/ws/conn/{id}      某条连接的数据与状态
/ws/events         全局：服务启停、地址漂移、错误
```

**二进制帧**（数据）：8 字节小端 `u64` 毫秒时间戳 + 原始字节。

- 服务端 → 浏览器：收到的数据（rx）
- 浏览器 → 服务端：要发送的数据（tx，前端已完成文本→UTF-8 或 HEX→字节的转换）

用二进制而不是 base64：串口/文件场景体积差 33%，且前端 `ArrayBuffer` 处理更直接。

**文本帧**（JSON）：

```json
{"t":"state","state":"open","local":"192.168.1.5:9000","peer":"192.168.1.50:51234","family":"v4"}
{"t":"log","level":"warn","msg":"..."}
{"t":"progress","dir":"up","done":1048576,"total":10485760}
{"t":"error","code":3000,"message":"bind failed","addr":"[::]:9000","sys_errno":98}
{"t":"warn","dropped":120}
```

---

## 11. REST API（M0 子集）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/meta` | 版本、平台、能力位 |
| GET | `/api/net/interfaces` | 网卡 + 地址（family/prefix/scope/up/mtu） |
| POST | `/api/net/test-bind` | 试绑定，返回逐地址结果 |
| GET | `/api/services` | 服务列表与状态 |
| POST | `/api/services` | 新建（不自动启动） |
| POST | `/api/services/{id}/start` | 启动，返回 plan slots |
| POST | `/api/services/{id}/stop` | 停止 |
| DELETE | `/api/services/{id}` | 删除（先停） |
| GET | `/api/services/{id}/connections` | 当前连接 |
| GET | `/api/sessions` | 客户端会话列表 |
| POST | `/api/sessions` | 新建（raw tcp/udp） |
| POST | `/api/sessions/{id}/connect` | 连接（含源绑定、地址族） |
| POST | `/api/sessions/{id}/send` | `{data, enc:"utf8"|"hex"}` |
| DELETE | `/api/sessions/{id}` | 断开并删除 |
| GET | `/api/sessions/{id}/resolve` | 解析结果（A/AAAA、耗时） |

统一错误体：

```json
{"code":3000,"message":"bind failed","detail":"[::]:9000 已被占用","sys_errno":98,"addr":"[::]:9000"}
```

---

## 12. 错误码

| 段 | 含义 | 例子 |
|---|---|---|
| 1000 | 参数/配置 | 1001 非法端口，1002 地址族与绑定方式冲突 |
| 1100 | 鉴权 | 1101 token 缺失/错误，1102 Origin 非法 |
| 2000 | 解析/连接 | 2001 域名无记录，2002 连接超时，2003 全部地址失败 |
| 2100 | 源绑定 | 2101 网卡无对应家族地址，2102 源地址 bind 失败 |
| 3000 | 绑定/监听 | 3001 全部地址失败，3002 权限不足，3003 端口占用 |
| 4000 | 协议 | 4001 报文解析失败，4002 认证失败 |
| 5000 | I/O/内部 | 5001 磁盘、5002 线程、5003 未知 |

每条错误必须带 `sys_errno` 与地址文本；UI 把 `sys_errno` 映射成人话（`EADDRINUSE` → "端口已被占用"）。

---

## 13. 配置与路径

```
Windows: %APPDATA%\NetDebug\        Linux: ~/.config/netdebug/
  config.json    services.json    sessions.json
  runtime.json   ← 管理端口 + token（0600）
  logs/  downloads/  keys/  known_hosts
```

服务定义示例：

```json
{
  "id": "svc_1",
  "name": "设备固件 FTP",
  "protocol": "ftp",
  "bind": {"kind": "iface", "iface_id": "linux:eth0", "family": "dual", "port": 2121},
  "options": {
    "root": "/srv/firmware",
    "readonly": false,
    "passive_ports": [50000, 50100],
    "advertise_addr": null,
    "allow": ["192.168.1.0/24", "fd00::/64"],
    "max_conns": 16
  }
}
```

`advertise_addr` 是 NAT/端口映射下 PASV 回复用的地址，与绑定地址分开。

---

## 14. 安全设计

**管理面（只回环）**

- 只听 `127.0.0.1` + `::1` 两个套接字，**绝不**用 `::` 通配。
- 启动生成 32 字节随机 token，写 `runtime.json`（0600）。
- 浏览器打开 `http://127.0.0.1:7801/#t=<token>`；token 放在 **URL fragment**，不会进 Referer/日志；前端读 `location.hash` 后存内存，所有请求带 `X-Auth-Token`。
- 校验 `Host` ∈ {`127.0.0.1:port`, `[::1]:port`, `localhost:port`} —— 防 DNS rebinding。
- 有 `Origin` 时必须匹配上述来源，否则 403；**不返回任何 CORS 头**。
- `/api/fs` 只能访问配置的根目录，`realpath` 前缀校验 + 拒绝 `..`。

**服务面（可能对局域网）**

- 每个服务可选 CIDR 允许名单（v4/v6 都要支持，映射地址先归一化）。
- 最大连接数、空闲超时、单 IP 连接数上限。
- TFTP 默认只读、限制文件大小；FTP 默认关闭匿名、根目录 jail、主动模式只回连对端 IP。
- UI 顶部常驻"有 N 个服务对局域网开放"提示 + 一键全停。

---

## 15. 平台层接口清单（M0 必须有的）

```c
/* net */
int  nd_socket(int family, int type);
void nd_sockopt_listen(int fd, int addr_family, int dual);
int  nd_bind(int fd, const nd_addr *a);
int  nd_listen(int fd, int backlog);
int  nd_accept(int fd, nd_addr *peer, nd_addr *local);
int  nd_connect_timeout(int fd, const nd_addr *a, int ms, int *sys_err);
int  nd_set_blocking(int fd, int on);
int  nd_set_timeouts(int fd, int rx_ms, int tx_ms);
void nd_shutdown(int fd);
void nd_close(int fd);
int  nd_last_error(void);                     /* 统一 errno / WSAGetLastError */

/* iface */
int  nd_iface_list(nd_iface *out, size_t cap, size_t *n);

/* addr */
int  nd_addr_parse(const char *text, int def_port, nd_addr *out);
int  nd_addr_format(const nd_addr *a, char *buf, size_t n);
int  nd_addr_normalize(nd_addr *a);           /* 映射地址 → v4 */

/* time / proc / fs */
uint64_t nd_monotonic_ms(void);
int  nd_open_browser(const char *url);
int  nd_single_instance(const char *name);
```

`nd_addr` 与 `nd_last_error` 这两个抽象做对，上层就基本没有 `#ifdef` 了。

---

## 16. 测试策略

| 层 | 方式 |
|---|---|
| 规划器 | 纯函数 + 假网卡表：v4-only、v6-only、双栈、无地址、双栈只有一族、地址数超上限 |
| 地址 | 解析/格式化往返：`[::1]:80`、`fe80::1%eth0`、映射地址归一化、非法输入 |
| 缓冲 | 环形缓冲边界：满、空、跨尾、并发读写（TSan） |
| 协议解析 | 静态报文 fixture（后续 FTP 多行应答、TFTP 选项、HTTP chunked） |
| 集成 | 回环双族：v4 连、v6 连、双栈同端口；端口冲突；服务重启后端口可复用 |
| 端到端 | 两台机器/两个网络命名空间，验真实网卡绑定 |
| 模糊 | Linux CI 上对协议解析器跑 libFuzzer（M2 起） |

CI 矩阵：Windows Server 2022 + MSVC；Ubuntu 22.04 + GCC 12；Linux 额外开 ASan/UBSan。

---

## 17. M0 任务拆解

| # | 任务 | 产出 | 工时 |
|---|---|---|---|
| 1 | 仓库、CMake presets、vcpkg manifest、CI 骨架 | 两平台能编译空 main | 8h |
| 2 | core：log / error / ring / thread / queue / json | 单测通过 | 16h |
| 3 | platform：网卡枚举（Win/Linux）、addr 解析格式化归一化、单调时钟、错误映射 | `nd_iface_list` 与 `ipconfig`/`ip addr` 一致 | 24h |
| 4 | planner + 冲突检测 + 单测 | 7.1 表全覆盖 | 20h |
| 5 | listener：多 fd poll、accept、DEGRADED、停止 | 起停无残留 fd | 12h |
| 6 | raw TCP/UDP 服务 + 客户端会话 + 收发 | nc 互通，网页可见 | 16h |
| 7 | httpd：civetweb、token、Host/Origin、静态资源内嵌 | 单文件能出页面 | 12h |
| 8 | REST：interfaces / test-bind / services / sessions | curl 全通 | 16h |
| 9 | WS：conn 流、events、pusher、背压 | 双向二进制收发 | 16h |
| 10 | 前端最小页：网卡表、绑定表单、服务卡、终端 | 手工可完成全链路 | 24h |
| 11 | 打包：单文件、自动开浏览器、路径与日志 | 绿色版可拷走运行 | 8h |
| 12 | 双平台验收与缺陷修复 | 验收清单全绿 | 16h |

合计约 **188h ≈ 23 人日 ≈ 4.5 周**（含调试与返工缓冲）。这与需求文档修订后的 M0 4–5 周一致。

---

## 18. M0 验收清单

1. Windows 10/11 与 Ubuntu 22.04 均能一条命令构建，CI 绿。
2. 网卡列表的地址、前缀、scope、up/down 与系统工具一致。
3. `test-bind`：占用端口 → `3003` + 具体地址；`ADDR + DUAL` → `1002`；网卡无 v4 地址且选 v4 → 明确报错。
4. 绑 `ethX + dual + 9000`：`ss -lntp` / `netstat -ano` 中能看到预期套接字（1 个或 2 个，与规划一致）。
5. 另一台机器分别用 v4、v6 连上，网页终端双向收发不乱序、不丢字节。
6. 双栈通配时，v4 对端在 UI 显示为 `192.168.x.x`（已归一化），日志保留原始映射形式。
7. 拔网线/改 IP 后 ≤10s，监听表与 UI 状态更新，不崩溃。
8. 停服务后端口立即释放，可立刻重新绑定（Linux 无 TIME_WAIT 阻塞）。
9. 无 token / 错 token → 401；伪造 Origin → 403；`Host` 非回环 → 403。
10. 退出后无残留线程、无残留监听端口、日志完整落盘。

---

## 19. Spike 与未决

| 编号 | 内容 | 时点 | 失败预案 |
|---|---|---|---|
| S1 | civetweb：跨线程写 WS 是否安全、二进制帧、静态资源内嵌 | M0 第 1 周 | 改 pusher 单线程 + `mg_lock_connection`；再不行换 libwebsockets |
| S2 | libsmb2：Windows 构建、IPv6 字面量、源绑定、共享枚举接口 | M6 前 | 明确降级"仅 IPv4"，UI 标注 |
| S3 | 串口：Windows OVERLAPPED vs 读线程；Linux 高波特率 `termios2` | M1 | Windows 用读线程 + `COMMTIMEOUTS` |
| S4 | TLS：OpenSSL 静态链接、自签证书校验开关 | M2 | 改用 mbedTLS |

未决（不阻塞 M0）：SSH 老算法清单要在真机上验证；FTP 服务端被动端口段与 NAT 通告地址的组合需要真机回归。

---

## 20. 后续里程碑的设计边界

- **M1**：串口与 Telnet 复用同一个 `nd_conn`；Telnet 服务端只是多一个 accept 后的协议实现，不加新框架。
- **M2/M3**：HTTP、FTP 客户端需要"请求-应答"等待器（`reply_q` + condvar），在 M0 的 `nd_conn` 上补，不改线程模型。
- **M3/M4**：FTP/TFTP 服务端需要"每个控制连接再开数据套接字"，复用 `nd_plan_build(spec=ADDR, port=0)`。
- **M5/M6**：SSH/SMB 用库，协议层只做"库回调 ↔ nd_conn 队列"的适配，不碰网络层。

每个协议开工前单独出一页设计（报文状态机 + 异常路径 + 真机验证清单），不一次性写全集。
