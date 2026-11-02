# NetDebug 单元测试报告

| 项目 | 值 |
|---|---|
| 生成时间 | 2026-11-02 14:48:13 |
| 平台 | windows |
| 测试用例 | 44 |
| 断言总数 | 449 |
| 失败断言 | 0 |
| 失败用例 | 0 |
| 结论 | **通过** |

## 按套件汇总

| 套件 | 用例数 | 断言数 | 失败 | 结果 |
|---|---|---|---|---|
| core.util | 8 | 61 | 0 | ✅ |
| core.container | 5 | 59 | 0 | ✅ |
| net.addr | 8 | 105 | 0 | ✅ |
| net.url | 4 | 56 | 0 | ✅ |
| net.planner | 7 | 83 | 0 | ✅ |
| httpd | 6 | 56 | 0 | ✅ |
| proto.telnet | 6 | 29 | 0 | ✅ |

## 用例明细

| 套件 | 用例 | 断言 | 结果 | 耗时 |
|---|---|---|---|---|
| core.util | strlcpy 复制与截断 | 11 | ✅ 通过 | 0 ms |
| core.util | strdup 独立副本 | 4 | ✅ 通过 | 0 ms |
| core.util | trim 去除首尾空白 | 4 | ✅ 通过 | 0 ms |
| core.util | starts_with / ends_with | 9 | ✅ 通过 | 0 ms |
| core.util | hex 编解码 | 14 | ✅ 通过 | 0 ms |
| core.util | UTF-8 校验 | 9 | ✅ 通过 | 0 ms |
| core.util | 错误码文本 | 7 | ✅ 通过 | 0 ms |
| core.util | 随机数 | 3 | ✅ 通过 | 1 ms |
| core.container | 环形缓冲 基本读写 | 14 | ✅ 通过 | 0 ms |
| core.container | 环形缓冲 环绕与满/空边界 | 19 | ✅ 通过 | 0 ms |
| core.container | 帧队列 入队/出队顺序 | 14 | ✅ 通过 | 14 ms |
| core.container | 帧队列 容量上限与关闭语义 | 7 | ✅ 通过 | 61 ms |
| core.container | 帧队列 多线程生产者/消费者 | 5 | ✅ 通过 | 3 ms |
| net.addr | IPv4 解析/格式化 | 16 | ✅ 通过 | 0 ms |
| net.addr | IPv6 解析/格式化 | 16 | ✅ 通过 | 0 ms |
| net.addr | 非法地址拒绝 | 8 | ✅ 通过 | 0 ms |
| net.addr | 地址比较 | 4 | ✅ 通过 | 0 ms |
| net.addr | 地址分类 any/loopback/linklocal | 21 | ✅ 通过 | 0 ms |
| net.addr | sockaddr 双向转换 | 16 | ✅ 通过 | 0 ms |
| net.addr | IPv4 映射地址归一化 | 10 | ✅ 通过 | 0 ms |
| net.addr | 字面地址解析忽略地址族 | 14 | ✅ 通过 | 0 ms |
| net.url | URL 基本解析 | 15 | ✅ 通过 | 0 ms |
| net.url | URL 路径/查询/片段处理 | 11 | ✅ 通过 | 0 ms |
| net.url | URL 非法输入 | 8 | ✅ 通过 | 0 ms |
| net.url | 重定向 URL 解析 | 22 | ✅ 通过 | 0 ms |
| net.planner | 地址族名称 | 4 | ✅ 通过 | 0 ms |
| net.planner | 绑定全部地址 | 13 | ✅ 通过 | 0 ms |
| net.planner | 绑定指定网卡 | 13 | ✅ 通过 | 0 ms |
| net.planner | 网卡缺少地址族时的降级/失败 | 6 | ✅ 通过 | 0 ms |
| net.planner | 绑定指定 IP 的校验 | 9 | ✅ 通过 | 0 ms |
| net.planner | 真实绑定 socket | 12 | ✅ 通过 | 4 ms |
| net.planner | 绑定 JSON 解析/序列化 | 26 | ✅ 通过 | 0 ms |
| httpd | SHA1 标准测试向量 | 4 | ✅ 通过 | 0 ms |
| httpd | SHA1 分块更新与百万字节 | 2 | ✅ 通过 | 6 ms |
| httpd | Base64 编码 | 15 | ✅ 通过 | 0 ms |
| httpd | Base64 解码 | 13 | ✅ 通过 | 0 ms |
| httpd | URL 解码 | 5 | ✅ 通过 | 0 ms |
| httpd | 查询串取值 | 17 | ✅ 通过 | 0 ms |
| proto.telnet | 普通数据透传 | 4 | ✅ 通过 | 0 ms |
| proto.telnet | IAC IAC 转义为单个 0xFF | 4 | ✅ 通过 | 0 ms |
| proto.telnet | 协商命令(3 字节)被剥离 | 8 | ✅ 通过 | 0 ms |
| proto.telnet | 子协商(NAWS)整段剥离 | 2 | ✅ 通过 | 0 ms |
| proto.telnet | 跨数据包的 IAC 状态保持 | 9 | ✅ 通过 | 0 ms |
| proto.telnet | 服务端接收 NAWS/TTYPE 协商不产生输出 | 2 | ✅ 通过 | 0 ms |
