/*
 * NetDebug 管理页面 —— 界面专项测试
 *
 * 通过 CDP 驱动真实浏览器，覆盖：服务增删改启停、绑定校验、会话表单联动、
 * 终端收发、HTTP/FTP/TFTP 工具、移动端布局。
 *
 * 用法：node tools/ui_test.mjs [--md 输出路径]
 */
import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const APP = `http://127.0.0.1:${rt.port}/#t=${rt.token}`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---------------- 迷你测试框架 ---------------- */
const cases = [];
let cur = null;
let total = 0;
let failed = 0;

function beginCase(name) {
  cur = { name, checks: 0, failures: 0, notes: [] };
}
function check(cond, msg) {
  total++;
  cur.checks++;
  if (!cond) {
    failed++;
    cur.failures++;
    cur.notes.push(msg);
    console.log('       FAIL ' + msg);
  }
}
function eq(actual, expect, what) {
  check(actual === expect, `${what} 期望 ${JSON.stringify(expect)} 实际 ${JSON.stringify(actual)}`);
}
function endCase() {
  cases.push(cur);
  console.log(`  [${cur.failures ? 'FAIL' : ' ok '}] ${cur.name} (${cur.checks} 断言)`);
}

/* ---------------- API 辅助 ---------------- */
async function api(path, init = {}) {
  const r = await fetch(`http://127.0.0.1:${rt.port}${path}`, {
    ...init,
    headers: { 'X-Auth-Token': rt.token, 'Content-Type': 'application/json', ...(init.headers || {}) },
  });
  return r.json().catch(() => null);
}

/* 直接调 API 清理历史测试残留，避免用例受旧数据影响 */
async function cleanupTestServices() {
  const list = await api('/api/services');
  if (!Array.isArray(list)) return;
  for (const s of list) {
    if (/^(UI 测试|冲突测试)/.test(s.name)) await api(`/api/services/${s.id}`, { method: 'DELETE' });
  }
}

/* ---------------- CDP ---------------- */
async function openPage(target, opts = {}) {
  const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(target), { method: 'PUT' })).json();
  await sleep(1100);
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r, { once: true }));
  let id = 0;
  const pending = new Map();
  const pageErrors = [];
  const pageLogs = [];
  const dialogs = [];
  let sendRef = null;

  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); return; }
    if (m.method === 'Runtime.exceptionThrown')
      pageErrors.push(String(m.params.exceptionDetails.exception?.description || '').slice(0, 160));
    if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error')
      pageLogs.push(String(m.params.entry.text).slice(0, 160));
    /* confirm()/alert() 会阻塞页面，必须自动确认，否则 CDP 调用会一直挂住 */
    if (m.method === 'Page.javascriptDialogOpening') {
      dialogs.push(m.params.message);
      sendRef('Page.handleJavaScriptDialog', { accept: true });
    }
  });

  const send = (method, params = {}) =>
    new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  sendRef = send;

  const ev = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result?.value;
  };

  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', {
    width: opts.width || 1400,
    height: opts.height || 880,
    deviceScaleFactor: opts.dpr || 1,
    mobile: !!opts.mobile,
  });
  return { ws, send, ev, pageErrors, pageLogs, dialogs, close: () => ws.close() };
}

async function waitFor(ev, expr, ms = 10000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (await ev(expr)) return true;
    await sleep(120);
  }
  return false;
}

const setVal = (ev, id, v) =>
  ev(`(() => { const e = document.getElementById('${id}'); e.value = ${JSON.stringify(v)};
      e.dispatchEvent(new Event('input', { bubbles: true }));
      e.dispatchEvent(new Event('change', { bubbles: true })); })()`);

async function createService(p, { name, proto, port, kind = 'any', family = 'v4', root, echo = false }) {
  await p.ev("document.getElementById('btn-new').click()");
  await sleep(220);
  await setVal(p.ev, 'f-name', name);
  await setVal(p.ev, 'f-proto', proto);
  await setVal(p.ev, 'f-port', String(port));
  await setVal(p.ev, 'f-kind', kind);
  await setVal(p.ev, 'f-family', family);
  if (root) await setVal(p.ev, 'f-ftp-root', root);
  await p.ev(`document.getElementById('f-echo').checked = ${echo ? 'true' : 'false'}`);
  await p.ev("document.getElementById('btn-create').click()");
  const ok = await waitFor(p.ev, `document.getElementById('service-list').innerText.includes(${JSON.stringify(name)})`);
  await sleep(250);
  return ok;
}

async function findSvcCard(ev, name) {
  return ev(`(() => {
    const c = [...document.querySelectorAll('#service-list .svc')].find(x => x.innerText.includes(${JSON.stringify(name)}));
    return c ? c.innerText.replace(/\\n/g, ' / ') : null;
  })()`);
}

async function clickSvcBtn(p, name, act) {
  return p.ev(`(() => {
    const c = [...document.querySelectorAll('#service-list .svc')].find(x => x.innerText.includes(${JSON.stringify(name)}));
    if (!c) return false;
    const b = c.querySelector('button[data-act=${act}]');
    if (!b) return false;
    b.click();
    return true;
  })()`);
}

/* ---------------- 开始 ---------------- */
await cleanupTestServices();

const p = await openPage(APP);

beginCase('页面加载与初始渲染');
await waitFor(p.ev, "document.getElementById('status-line').textContent === '就绪'");
eq(await p.ev('document.title'), 'NetDebug · 网络调试工作台', '页面标题');
eq(await p.ev("document.getElementById('status-line').textContent"), '就绪', '连接状态');
check(await p.ev("!!document.getElementById('welcome') && !document.getElementById('welcome').classList.contains('hidden')"), '欢迎页可见');
check(await p.ev("getComputedStyle(document.querySelector('.sidebar')).display !== 'none'"), '桌面端侧边栏可见');
endCase();

beginCase('协议切换联动默认名称');
{
  const expect = {
    'raw-tcp': 'TCP 调试服务',
    'raw-udp': 'UDP 调试服务',
    telnet: 'Telnet 服务',
    ftp: 'FTP 文件服务',
    tftp: 'TFTP 传输服务',
    http: 'HTTP 文件服务',
    speedtest: 'LibreSpeed 测速',
  };
  await p.ev("document.getElementById('btn-new').click()");
  await sleep(250);
  for (const [proto, name] of Object.entries(expect)) {
    await setVal(p.ev, 'f-proto', proto);
    await sleep(120);
    eq(await p.ev("document.getElementById('f-name').value"), name, `协议 ${proto} 的默认名称`);
  }
  /* 用户改过名字后不应被覆盖 */
  await setVal(p.ev, 'f-name', '我的服务');
  await setVal(p.ev, 'f-proto', 'http');
  await sleep(150);
  eq(await p.ev("document.getElementById('f-name').value"), '我的服务', '手动改名后不被覆盖');
}
endCase();

beginCase('新建服务：默认端口按协议取值并随机偏移');
{
  const ranges = {
    'raw-tcp': [9000, 9099],
    http: [8080, 8179],
    ftp: [2121, 2220],
    tftp: [6969, 7068],
    telnet: [2323, 2422],
  };
  const seen = new Set();
  for (let i = 0; i < 3; i++) {
    await p.ev("document.getElementById('btn-new').click()");
    await sleep(260);
    const p0 = Number(await p.ev("document.getElementById('f-port').value"));
    check(p0 >= 9000 && p0 <= 9099, `TCP 默认端口落在 9000–9099（实际 ${p0}）`);
    seen.add(p0);
    for (const proto of ['http', 'ftp', 'tftp', 'telnet']) {
      await p.ev(`document.querySelector('#f-proto-tiles .proto-tile[data-proto=${proto}]').click()`);
      await sleep(160);
      const pv = Number(await p.ev("document.getElementById('f-port').value"));
      const [lo, hi] = ranges[proto];
      check(pv >= lo && pv <= hi, `${proto} 默认端口落在 ${lo}–${hi}（实际 ${pv}）`);
    }
    await p.ev("document.getElementById('btn-cancel').click()");
    await sleep(220);
  }
  check(seen.size > 1, '多次新建得到的端口不完全相同');

  await p.ev("document.getElementById('btn-new').click()");
  await sleep(260);
  await setVal(p.ev, 'f-port', '12345');
  await p.ev("document.querySelector('#f-proto-tiles .proto-tile[data-proto=http]').click()");
  await sleep(200);
  eq(await p.ev("document.getElementById('f-port').value"), '12345', '手动填的端口不被协议切换覆盖');
  await p.ev("document.getElementById('btn-cancel').click()");
  await sleep(200);
}
endCase();

beginCase('绑定方式切换字段显隐');
await setVal(p.ev, 'f-kind', 'any');
await sleep(120);
check(await p.ev("document.getElementById('wrap-iface').classList.contains('hidden')"), '全部地址隐藏网卡选择');
check(await p.ev("document.getElementById('wrap-addr').classList.contains('hidden')"), '全部地址隐藏 IP 输入');
await setVal(p.ev, 'f-kind', 'iface');
await sleep(120);
check(await p.ev("!document.getElementById('wrap-iface').classList.contains('hidden')"), '指定网卡显示网卡选择');
check((await p.ev("document.getElementById('f-iface').options.length")) > 0, '网卡列表非空');
await setVal(p.ev, 'f-kind', 'addr');
await sleep(120);
check(await p.ev("!document.getElementById('wrap-addr').classList.contains('hidden')"), '指定 IP 显示地址输入');
eq(await p.ev("document.getElementById('f-family').options.length"), 3, '地址族三个选项');
endCase();

beginCase('端口输入校验');
const portValid = async () => p.ev("document.getElementById('f-port').validity.valid");
await setVal(p.ev, 'f-port', '0');
check(await portValid(), '端口 0 合法（表示系统分配）');
await setVal(p.ev, 'f-port', '65536');
check(!(await portValid()), '端口 65536 视为非法');
await setVal(p.ev, 'f-port', '65535');
check(await portValid(), '端口 65535 合法');
await p.ev("document.getElementById('btn-cancel').click()");
await sleep(250);
check(await p.ev("!document.getElementById('dlg-service').open"), '取消后对话框关闭');
endCase();

beginCase('测试绑定与创建服务');
await p.ev("document.getElementById('btn-new').click()");
await sleep(220);
await setVal(p.ev, 'f-name', 'UI 测试 TCP');
await setVal(p.ev, 'f-proto', 'raw-tcp');
await setVal(p.ev, 'f-port', '19201');
await setVal(p.ev, 'f-kind', 'any');
await setVal(p.ev, 'f-family', 'v4');
await p.ev("document.getElementById('f-echo').checked = true");
await p.ev("document.getElementById('btn-test').click()");
await waitFor(p.ev, "document.getElementById('bind-result').innerText.includes('成功')");
let txt = await p.ev("document.getElementById('bind-result').innerText");
check(txt.includes('成功'), `绑定测试成功提示：${txt.replace(/\n/g, ' | ')}`);
check(txt.includes('0.0.0.0:19201'), '绑定成功地址正确');
await p.ev("document.getElementById('btn-create').click()");
await waitFor(p.ev, "document.getElementById('service-list').innerText.includes('UI 测试 TCP')");
await sleep(300);
const card = await findSvcCard(p.ev, 'UI 测试 TCP');
check(card && card.includes('运行中'), `服务卡片显示运行中：${card}`);
check(card && card.includes(':19201'), '服务卡片显示端口');
check(card && card.includes('0.0.0.0:19201'), '服务卡片显示监听地址');
endCase();

beginCase('端口冲突提示');
await p.ev("document.getElementById('btn-new').click()");
await sleep(220);
await setVal(p.ev, 'f-name', 'UI 测试冲突');
await setVal(p.ev, 'f-proto', 'raw-tcp');
await setVal(p.ev, 'f-port', '19201');
await setVal(p.ev, 'f-kind', 'any');
await setVal(p.ev, 'f-family', 'v4');
await p.ev("document.getElementById('btn-create').click()");
await waitFor(p.ev, "document.getElementById('bind-result').innerText.includes('占用')");
txt = await p.ev("document.getElementById('bind-result').innerText");
check(txt.includes('占用'), `冲突提示：${txt.replace(/\n/g, ' | ')}`);
check(/19201/.test(txt), '提示中包含冲突地址');
check(await p.ev("document.getElementById('dlg-service').open"), '失败后对话框保持打开');
await p.ev("document.getElementById('btn-cancel').click()");
await sleep(200);
endCase();

beginCase('停止与启动服务');
await clickSvcBtn(p, 'UI 测试 TCP', 'stop');
await sleep(900);
const stopped = await findSvcCard(p.ev, 'UI 测试 TCP');
check(stopped.includes('已停止'), `停止后状态：${stopped}`);
const list = await api('/api/services');
const svc = (list || []).find((s) => s.name === 'UI 测试 TCP');
check(svc && svc.running === false, 'API 标记为未运行');
await clickSvcBtn(p, 'UI 测试 TCP', 'start');
await sleep(900);
const started = await findSvcCard(p.ev, 'UI 测试 TCP');
check(started.includes('运行中'), `重新启动后状态：${started}`);
endCase();

beginCase('编辑服务');
await clickSvcBtn(p, 'UI 测试 TCP', 'edit');
await sleep(350);
eq(await p.ev("document.querySelector('#dlg-service h2').textContent"), '编辑监听服务', '编辑标题');
eq(await p.ev("document.getElementById('btn-create').textContent"), '保存修改', '编辑按钮文案');
eq(await p.ev("document.getElementById('f-port').value"), '19201', '端口回填');
eq(await p.ev("document.getElementById('f-name').value"), 'UI 测试 TCP', '名称回填');
eq(await p.ev("document.getElementById('f-proto').value"), 'raw-tcp', '协议回填');
await setVal(p.ev, 'f-name', 'UI 测试 TCP 改名');
await p.ev("document.getElementById('btn-create').click()");
check(await waitFor(p.ev, "document.getElementById('service-list').innerText.includes('UI 测试 TCP 改名')"), '改名后列表更新');
await sleep(400);
check((await findSvcCard(p.ev, 'UI 测试 TCP 改名')).includes('运行中'), '编辑后仍在运行');
endCase();

beginCase('HTTP 服务的活跃统计面板');
{
  const found = await p.ev(`(() => {
    const c = [...document.querySelectorAll('#service-list .svc')].find((x) => x.innerText.includes('请求'));
    if (!c) return false;
    c.click();
    return true;
  })()`);
  check(found, '存在 HTTP / 测速类服务卡片');
  await sleep(700);
  const txt = await p.ev("document.getElementById('conn-list').innerText");
  check(!txt.includes('暂无连接'), '点开 HTTP 服务不再显示「暂无连接」');
  check(txt.includes('进行中请求'), '显示进行中请求');
  check(txt.includes('累计请求'), '显示累计请求');
  check(txt.includes('不登记为长连接'), '说明统计口径');
  check(await p.ev("!!document.querySelector('#service-list button[data-act=conns]')"), '「活跃」是可点击按钮');
  check(/进行中/.test(await p.ev("document.getElementById('conn-count').textContent")), '计数标签显示进行中数量');
  /* 点卡片会打开服务详情标签，这里收掉，避免影响后续用例 */
  await p.ev(`(() => {
    const t = [...document.querySelectorAll('#tabs .tab')].find((x) => x.innerText.includes('服务 ·'));
    if (t) t.querySelector('.tab-close').click();
  })()`);
  await sleep(300);
}
endCase();

beginCase('服务实时日志：请求、关闭与清空');
{
  await p.ev(`(() => {
    const c = [...document.querySelectorAll('#service-list .svc')].find((x) => x.innerText.includes('请求'));
    c.querySelector('button[data-act=conns]').click();
  })()`);
  await sleep(700);
  check(await p.ev("!document.getElementById('log-view').classList.contains('hidden')"), '日志视图打开');
  check(
    (await p.ev("[...document.querySelectorAll('#tabs .tab')].map(t => t.innerText)")).some((t) => t.includes('日志')),
    '标签标题包含「日志」'
  );
  check(await p.ev("document.getElementById('log-list').innerText.includes('暂无记录')"), '初始为空态');
  const before = await p.ev("document.querySelectorAll('#log-list .log-row').length");
  await p.ev("fetch('http://127.0.0.1:19102/hello.txt', { mode: 'no-cors' }).catch(() => 0)");
  await sleep(900);
  const after = await p.ev("document.querySelectorAll('#log-list .log-row').length");
  check(after > before, '请求后新增日志行');
  const rowTxt = await p.ev("document.querySelector('#log-list .log-row').innerText.replace(/\\s+/g,' ').trim()");
  check(/已关闭|进行中/.test(rowTxt), '行内显示状态');
  check(rowTxt.includes('127.0.0.1'), '行内显示对端地址');
  check(/进行中/.test(await p.ev("document.getElementById('log-count').textContent")) || /条/.test(await p.ev("document.getElementById('log-count').textContent")), '计数显示条数');
  await p.ev("document.getElementById('log-clear').click()");
  await sleep(300);
  check(await p.ev("document.querySelectorAll('#log-list .log-row').length === 0"), '清空后没有行');
  check(await p.ev("document.getElementById('log-list').innerText.includes('暂无记录')"), '清空后回到空态');
  await p.ev(`(() => {
    const t = [...document.querySelectorAll('#tabs .tab')].find((x) => x.innerText.includes('日志'));
    if (t) t.querySelector('.tab-close').click();
  })()`);
  await sleep(350);
  check(await p.ev("document.querySelectorAll('#tabs .tab').length === 0"), '关闭日志标签后无残留');
}
endCase();

beginCase('服务详情页：指标、槽位与操作');
{
  await p.ev(`(() => {
    const c = [...document.querySelectorAll('#service-list .svc')].find((x) => x.innerText.includes('HTTP'));
    c.click();
  })()`);
  await sleep(800);
  check(await p.ev("!document.getElementById('svc-view').classList.contains('hidden')"), '详情视图打开');
  eq(await p.ev("document.getElementById('svc-title').textContent"), 'HTTP 文件服务', '标题是服务名');

  const metrics = await p.ev(
    "[...document.querySelectorAll('#svc-metrics .metric')].map(m => m.innerText.replace(/\\s+/g,' ').trim())"
  );
  check(metrics.some((m) => m.includes('峰值并发')), '有峰值并发指标');
  check(metrics.some((m) => m.includes('运行时长')), '有运行时长指标');
  check(metrics.some((m) => m.includes('监听槽位')), '有槽位数量指标');
  check(await p.ev("document.querySelectorAll('#svc-slots .slot-row').length > 0"), '槽位列表非空');
  check(await p.ev("document.querySelector('#svc-slots .slot-row.ok') !== null"), '槽位显示为正常状态');

  await p.ev("fetch('http://127.0.0.1:19102/hello.txt', { mode: 'no-cors' }).catch(() => 0)");
  await sleep(1000);
  const req = await p.ev(
    "[...document.querySelectorAll('#svc-metrics .metric')].find(m => m.innerText.includes('累计请求')).querySelector('b').textContent"
  );
  check(Number(req) > 0, `累计请求随流量增加（当前 ${req}）`);
  check((await p.ev("document.querySelectorAll('#svc-recent .recent-row').length")) > 0, '最近活动有记录');

  await p.ev("document.getElementById('svc-toggle').click()");
  await sleep(1300);
  eq(await p.ev("document.getElementById('svc-badge').textContent"), '已停止', '停止后徽章变为已停止');
  check((await p.ev("document.getElementById('svc-slots').innerText")).includes('未运行'), '停止后槽位提示未运行');

  await p.ev("document.getElementById('svc-toggle').click()");
  await sleep(1600);
  eq(await p.ev("document.getElementById('svc-badge').textContent"), '运行中', '启动后恢复运行中');
  check(await p.ev("document.querySelectorAll('#svc-slots .slot-row').length > 0"), '启动后槽位恢复');

  await p.ev("document.getElementById('svc-log-open').click()");
  await sleep(700);
  check(
    (await p.ev("[...document.querySelectorAll('#tabs .tab')].map(t => t.innerText)")).some((t) => t.includes('日志')),
    '详情页能打开该服务的日志标签'
  );
  for (const kw of ['日志', '服务 ·']) {
    await p.ev(`(() => {
      const t = [...document.querySelectorAll('#tabs .tab')].find((x) => x.innerText.includes(${JSON.stringify(kw)}));
      if (t) t.querySelector('.tab-close').click();
    })()`);
    await sleep(350);
  }
  check(await p.ev("document.querySelectorAll('#tabs .tab').length === 0"), '关闭后标签无残留');
}
endCase();

beginCase('新建会话：表单联动');
await p.ev("document.getElementById('btn-new-sess').click()");
await sleep(250);
await setVal(p.ev, 's-transport', 'tcp');
await sleep(150);
check(await p.ev("!document.getElementById('net-config').classList.contains('hidden')"), 'TCP 显示网络配置');
check(await p.ev("document.getElementById('ssh-config').classList.contains('hidden')"), 'TCP 隐藏 SSH 配置');
check(await p.ev("document.getElementById('serial-config').classList.contains('hidden')"), 'TCP 隐藏串口配置');
await setVal(p.ev, 's-transport', 'udp');
await sleep(150);
check(await p.ev("!document.querySelector('#s-transport option[value=udp]').disabled"), 'UDP 可选');
await setVal(p.ev, 's-proto', 'telnet');
await sleep(150);
check(await p.ev("document.querySelector('#s-transport option[value=udp]').disabled"), 'Telnet 下 UDP 被禁用');
await setVal(p.ev, 's-transport', 'ssh');
await sleep(200);
check(await p.ev("!document.getElementById('ssh-config').classList.contains('hidden')"), 'SSH 显示认证配置');
check(await p.ev("document.querySelector('#s-proto option[value=telnet]').disabled"), 'SSH 下 Telnet 被禁用');
check(await p.ev("!document.getElementById('net-config').classList.contains('hidden')"), 'SSH 仍需地址与端口');
await setVal(p.ev, 's-transport', 'serial');
await sleep(200);
check(await p.ev("!document.getElementById('serial-config').classList.contains('hidden')"), '串口显示串口参数');
check(await p.ev("document.getElementById('net-config').classList.contains('hidden')"), '串口隐藏网络配置');
endCase();

beginCase('目标为字面地址时自动确定地址族');
await p.ev("document.getElementById('btn-new-sess').click()");
await sleep(250);
await setVal(p.ev, 's-host', 'example.com');
await sleep(150);
check(!(await p.ev("document.getElementById('s-family').disabled")), '域名时地址族可选');
eq(await p.ev("document.getElementById('s-family').value"), 'dual', '域名时默认自动');
await setVal(p.ev, 's-host', '192.168.1.10');
await sleep(150);
check(await p.ev("document.getElementById('s-family').disabled"), 'IPv4 字面量时地址族锁定');
eq(await p.ev("document.getElementById('s-family').value"), 'v4', '自动选 IPv4');
check((await p.ev("document.getElementById('s-family-hint').textContent")).includes('IPv4'), '给出 IPv4 提示');
await setVal(p.ev, 's-host', 'fe80::1');
await sleep(150);
eq(await p.ev("document.getElementById('s-family').value"), 'v6', '自动选 IPv6');
await setVal(p.ev, 's-host', '');
await sleep(150);
check(!(await p.ev("document.getElementById('s-family').disabled")), '清空目标后恢复可选');
eq((await p.ev("document.getElementById('s-family-hint').textContent")).trim(), '', '清空后提示消失');
await p.ev("document.getElementById('s-cancel').click()");
await sleep(200);
endCase();

beginCase('TCP 会话连接与终端收发');
await p.ev("document.getElementById('s-cancel').click()");
await sleep(250);
await p.ev("document.getElementById('btn-new-sess').click()");
await sleep(250);
await setVal(p.ev, 's-name', 'UI 测试会话');
await setVal(p.ev, 's-transport', 'tcp');
await setVal(p.ev, 's-proto', 'raw');
await setVal(p.ev, 's-host', '127.0.0.1');
await setVal(p.ev, 's-port', '19201');
await setVal(p.ev, 's-family', 'v4');
await p.ev("document.getElementById('s-create').click()");
await waitFor(p.ev, "document.getElementById('session-list').innerText.includes('UI 测试会话')");
await sleep(300);
await p.ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
  c.querySelector('button[data-sact=open]').click();
})()`);
let opened = await waitFor(p.ev, "document.getElementById('term-state').textContent === 'open' && document.getElementById('term-peer').textContent.includes('19201')");
check(opened, '终端进入 open 状态');
eq(await p.ev("document.getElementById('term-proto').textContent"), 'TCP', '终端协议标签');
check((await p.ev("document.getElementById('term-peer').textContent")).includes('19201'), '终端显示对端地址');
await setVal(p.ev, 'send-input', 'ping-1');
await p.ev("document.getElementById('send-btn').click()");
/* 终端视图下日志 DOM 不逐条生成，切到日志视图再看收发记录 */
await p.ev("document.getElementById('term-mode-btn').click()");
await sleep(400);
check(await waitFor(p.ev, "document.getElementById('term-out').textContent.includes('RX ping-1')"), '收到服务端回显');
check((await p.ev("document.getElementById('term-out').textContent")).includes('TX ping-1'), '本地发送已记录');
await p.ev("document.getElementById('term-mode-btn').click()");
await sleep(300);
endCase();

beginCase('终端视图切换与清空');
await p.ev("document.getElementById('opt-hex').click()");
await sleep(300);
check(await p.ev("document.getElementById('opt-hex').checked"), 'HEX 开关已勾选');
await p.ev("document.getElementById('btn-clear').click()");
await sleep(300);
eq((await p.ev("document.getElementById('term-out').textContent")).trim(), '', '清空后终端为空');
await p.ev("document.getElementById('opt-hex').click()");
await sleep(200);
check(!(await p.ev("document.getElementById('opt-hex').checked")), 'HEX 开关可关闭');
endCase();

beginCase('快捷发送与全局预置');
{
  check(await p.ev("!document.getElementById('term-quick').classList.contains('hidden')"), '终端模式显示快捷发送栏');
  const cmd = `UI-TEST-${Date.now()}`;
  await p.ev(`(() => {
    const e = document.getElementById('quick-input');
    e.value = ${JSON.stringify(cmd)};
    e.dispatchEvent(new Event('input', { bubbles: true }));
  })()`);
  await p.ev("document.getElementById('quick-save').click()");
  await sleep(800);
  check(
    (await p.ev("[...document.querySelectorAll('.quick-preset')].map(b => b.textContent)")).includes(cmd),
    '存为预置后按钮出现在列表里'
  );
  check(((await api('/api/presets')) || []).includes(cmd), '预置已写入后端（重启后仍在）');

  const txBefore = await p.ev("document.getElementById('term-tx').textContent");
  await p.ev(`(() => {
    const b = [...document.querySelectorAll('.quick-preset')].find((x) => x.textContent === ${JSON.stringify(cmd)});
    if (b) b.click();
  })()`);
  await sleep(600);
  check((await p.ev("document.getElementById('term-tx').textContent")) !== txBefore, '点预置按钮后 TX 字节增加');

  /* 清理：把测试命令从后端删掉，避免污染真实配置 */
  const list = (await api('/api/presets')) || [];
  await api('/api/presets', { method: 'PUT', body: JSON.stringify({ presets: list.filter((x) => !x.startsWith('UI-TEST-')) }) });
}
endCase();

beginCase('多标签：工具与终端并存、可切换可关闭');

beginCase('多标签：工具与终端并存、可切换可关闭');
{
  const base = await p.ev("document.querySelectorAll('#tabs .tab').length");
  check(base === 1, `当前应有 1 个终端标签，实际 ${base}`);
  await p.ev("document.getElementById('btn-http').click()");
  await sleep(500);
  check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === base + 1, '打开工具后新增一个标签');
  check(await p.ev("[...document.querySelectorAll('#tabs .tab')].some(t => t.innerText.includes('HTTP 调试'))"), 'HTTP 标签存在');
  check(await p.ev("!document.getElementById('http-view').classList.contains('hidden')"), 'HTTP 视图可见');
  await p.ev("document.getElementById('btn-ftp').click()");
  await sleep(500);
  check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === base + 2, '再打开一个工具，标签继续累加');
  await p.ev("document.querySelector('#tabs .tab').click()");
  await sleep(500);
  check(await p.ev("!document.getElementById('terminal').classList.contains('hidden')"), '点第一个标签切回终端');
  check((await p.ev("document.getElementById('term-peer').textContent")).length > 3, '终端头部信息恢复');
  for (const name of ['HTTP 调试', 'FTP 文件']) {
    await p.ev(`(() => {
      const t = [...document.querySelectorAll('#tabs .tab')].find((x) => x.innerText.includes(${JSON.stringify(name)}));
      if (t) t.querySelector('.tab-close').click();
    })()`);
    await sleep(350);
    check(
      !(await p.ev(`[...document.querySelectorAll('#tabs .tab')].some(t => t.innerText.includes(${JSON.stringify(name)}))`)),
      `${name} 标签已关闭`
    );
  }
  check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === 1, '关掉工具后只剩终端标签');
  check(await p.ev("!document.getElementById('terminal').classList.contains('hidden')"), '终端仍然可用');
}
endCase();

beginCase('会话断开：保留终端与历史输出');
{
  const txt = await p.ev(`(() => {
    const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
    return c ? c.innerText.replace(/\\n/g, ' / ') : '';
  })()`);
  check(txt.includes('断开'), `已连接会话显示断开按钮：${txt}`);
  check(txt.includes('切到终端'), '终端已打开时按钮显示「切到终端」');
}
const linesBefore = await p.ev("document.querySelectorAll('#term-out .line').length");
await p.ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
  c.querySelector('button[data-sact=disconnect]').click();
})()`);
await sleep(1300);
check(await p.ev("!document.getElementById('terminal').classList.contains('hidden')"), '断开后终端不关闭');
check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === 1, '断开后标签页保留');
check(
  (await p.ev("document.querySelectorAll('#term-out .line').length")) >= linesBefore,
  `历史输出保留（断开前 ${linesBefore} 行）`
);
check(await p.ev("document.getElementById('term-state').textContent === 'closed'"), '终端状态变为 closed');
check(
  await p.ev(`(() => {
    const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
    return c ? c.innerText.includes('未连接') : false;
  })()`),
  '会话状态回到未连接'
);
check(
  await p.ev(`!([...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话')).innerText.includes('断开'))`),
  '未连接时不再显示断开按钮'
);
endCase();

beginCase('删除会话：终端一并关闭');
await p.ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
  c.querySelector('button[data-sact=open]').click();
})()`);
check(await waitFor(p.ev, "document.getElementById('term-state').textContent === 'open'"), '重新连接成功');
await p.ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试会话'));
  c.querySelector('button[data-sact=del]').click();
})()`);
await sleep(1400);
check(await p.ev("document.getElementById('terminal').classList.contains('hidden')"), '删除后终端关闭');
check(await p.ev("!document.getElementById('welcome').classList.contains('hidden')"), '回到欢迎页');
eq((await p.ev("document.getElementById('tabs').innerHTML")).trim(), '', '标签页已清空');
check(!(await p.ev("document.getElementById('session-list').innerText.includes('UI 测试会话')")), '会话已从列表移除');
endCase();

beginCase('关闭最后一个标签回到欢迎页');
{
  await p.ev("document.getElementById('btn-http').click()");
  await sleep(450);
  check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === 1, '打开工具后有 1 个标签');
  await p.ev("document.querySelector('#tabs .tab .tab-close').click()");
  await sleep(400);
  check((await p.ev("document.querySelectorAll('#tabs .tab').length")) === 0, '标签全部关闭');
  check(await p.ev("!document.getElementById('welcome').classList.contains('hidden')"), '回到欢迎页');
  check(await p.ev("document.getElementById('tabs').innerHTML.trim() === ''"), '标签条已清空');
}
endCase();

beginCase('UDP 回声服务与会话');
check(await createService(p, { name: 'UI 测试 UDP', proto: 'raw-udp', port: 19202, echo: true }), 'UDP 服务创建');
await p.ev("document.getElementById('btn-new-sess').click()");
await sleep(250);
await setVal(p.ev, 's-name', 'UI 测试 UDP 会话');
await setVal(p.ev, 's-transport', 'udp');
await setVal(p.ev, 's-proto', 'raw');
await setVal(p.ev, 's-host', '127.0.0.1');
await setVal(p.ev, 's-port', '19202');
await setVal(p.ev, 's-family', 'v4');
await p.ev("document.getElementById('s-create').click()");
await waitFor(p.ev, "document.getElementById('session-list').innerText.includes('UI 测试 UDP 会话')");
await sleep(300);
await p.ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('UI 测试 UDP 会话'));
  c.querySelector('button[data-sact=open]').click();
})()`);
opened = await waitFor(p.ev, "document.getElementById('term-state').textContent === 'open' && document.getElementById('term-peer').textContent.includes('19202')");
check(opened, 'UDP 会话已连接');
await setVal(p.ev, 'send-input', 'udp-ping');
await p.ev("document.getElementById('send-btn').click()");
await p.ev("document.getElementById('term-mode-btn').click()");
await sleep(400);
check(await waitFor(p.ev, "document.getElementById('term-out').textContent.includes('RX udp-ping')"), 'UDP 数据回显');
await p.ev("document.getElementById('term-mode-btn').click()");
await sleep(300);
endCase();

beginCase('HTTP 调试工具');
await p.ev("document.getElementById('btn-http').click()");
await sleep(300);
await setVal(p.ev, 'http-method', 'GET');
await setVal(p.ev, 'http-url', 'http://127.0.0.1:19102/hello.txt');
await p.ev("document.getElementById('http-send').click()");
await waitFor(p.ev, "document.getElementById('http-status').textContent.startsWith('200')");
check((await p.ev("document.getElementById('http-status').textContent")).startsWith('200'), '200 状态');
check((await p.ev("document.getElementById('http-response').textContent")).includes('hello from NetDebug'), '响应正文正确');
check((await p.ev("document.getElementById('http-resp-headers').innerText")).includes('Content-Type'), '响应头已渲染');
await setVal(p.ev, 'http-url', 'http://127.0.0.1:19102/nope.txt');
await p.ev("document.getElementById('http-send').click()");
await waitFor(p.ev, "document.getElementById('http-status').textContent.startsWith('404')");
check((await p.ev("document.getElementById('http-status').textContent")).startsWith('404'), '404 状态');
endCase();

beginCase('FTP 工具连接与列目录');
await p.ev("document.getElementById('btn-ftp').click()");
await sleep(300);
await setVal(p.ev, 'ftp-host', '127.0.0.1');
await setVal(p.ev, 'ftp-port', '19104');
await setVal(p.ev, 'ftp-user', 'anonymous');
await setVal(p.ev, 'ftp-pass', '');
await p.ev("document.getElementById('ftp-connect').click()");
check(await waitFor(p.ev, "document.getElementById('ftp-state').textContent.includes('已连接')"), 'FTP 已连接');
check(await waitFor(p.ev, "document.getElementById('ftp-list').innerText.includes('readme.txt')"), 'FTP 目录列出文件');
endCase();

beginCase('TFTP 下载');
{
  const svcs = await api('/api/services');
  const tftp = (svcs || []).find((s) => s.proto === 'tftp');
  check(!!tftp, '存在 TFTP 服务');
  if (tftp && !tftp.running) await api(`/api/services/${tftp.id}/start`, { method: 'POST' });
  await sleep(500);
}
await p.ev("document.getElementById('btn-tftp').click()");
await sleep(300);
await setVal(p.ev, 'tftp-host', '127.0.0.1');
await setVal(p.ev, 'tftp-port', '19105');
await setVal(p.ev, 'tftp-remote', 'firmware.bin');
await p.ev("document.getElementById('tftp-get').click()");
check(await waitFor(p.ev, "document.getElementById('tftp-state').textContent.includes('已下载')"), 'TFTP 下载完成');
check((await p.ev("document.getElementById('tftp-state').textContent")).includes('18'), '下载字节数正确');
endCase();

beginCase('删除服务');
const delOk = await clickSvcBtn(p, 'UI 测试 UDP', 'del');
check(delOk, '删除按钮可点击');
check(await waitFor(p.ev, "!document.getElementById('service-list').innerText.includes('UI 测试 UDP')", 6000), 'UDP 服务已从列表移除');
check(p.dialogs.some((d) => d.includes('删除')), '删除前有确认提示');
endCase();

beginCase('浏览器直连提示页');
const np = await openPage(`http://127.0.0.1:${rt.port}/`, { width: 1000, height: 760 });
await sleep(900);
eq(await np.ev('document.title'), 'NetDebug · 请使用程序窗口', '提示页标题');
eq(await np.ev("document.querySelector('h1').textContent"), '请使用 NetDebug 主窗口', '提示页首行');
{
  const body = await np.ev('document.body.innerText');
  check(body.includes('--console'), '提示页说明 --console 的调试地址');
  check(!body.includes('粘贴到浏览器'), '提示页不再引导用浏览器');
}
check(np.pageErrors.length === 0, `提示页 JS 异常：${np.pageErrors.join(' | ')}`);
await np.close();
endCase();

beginCase('页面无 JS 异常');
check(p.pageErrors.length === 0, `运行期 JS 异常：${p.pageErrors.join(' | ')}`);
endCase();

await cleanupTestServices();
await p.close();

/* ---------------- 移动端 ---------------- */
beginCase('移动端布局与抽屉');
const m = await openPage(APP, { width: 390, height: 780, dpr: 2, mobile: true });
await waitFor(m.ev, "document.getElementById('status-line').textContent === '就绪'");
check(await m.ev("getComputedStyle(document.querySelector('.mobile-bar')).display !== 'none'"), '移动端顶栏可见');
check(await m.ev("getComputedStyle(document.querySelector('.sidebar')).transform !== 'none'"), '移动端侧边栏收起');
await m.ev("document.getElementById('btn-sidebar').click()");
await sleep(450);
check(await m.ev("document.querySelector('.sidebar').classList.contains('open')"), '点击菜单打开抽屉');
await m.ev("document.getElementById('sidebar-backdrop').click()");
await sleep(400);
check(!(await m.ev("document.querySelector('.sidebar').classList.contains('open')")), '点击遮罩关闭抽屉');
await m.ev("document.getElementById('btn-sidebar').click()");
await sleep(400);
await m.ev("document.getElementById('btn-new').click()");
await sleep(400);
check(await m.ev("document.getElementById('dlg-service').open"), '移动端可打开新建服务对话框');
const dlgW = await m.ev("document.getElementById('dlg-service').getBoundingClientRect().width");
check(dlgW <= 390, `对话框宽度不超出屏幕（${dlgW}）`);
check(await m.ev('document.documentElement.scrollWidth <= window.innerWidth + 1'), '页面无横向溢出');
check(m.pageErrors.length === 0, `移动端 JS 异常：${m.pageErrors.join(' | ')}`);
await m.close();
endCase();

/* ---------------- 汇总 ---------------- */
const failedCases = cases.filter((c) => c.failures).length;
console.log('\n------------------------------------------------');
console.log(`用例 ${cases.length}  断言 ${total}  失败 ${failed}  失败用例 ${failedCases}`);

const mdArg = process.argv.indexOf('--md');
if (mdArg > 0 && process.argv[mdArg + 1]) {
  const d = new Date();
  const p2 = (x) => String(x).padStart(2, '0');
  const tzMin = -d.getTimezoneOffset();
  const tz = `UTC${tzMin >= 0 ? '+' : '-'}${p2(Math.floor(Math.abs(tzMin) / 60))}:${p2(Math.abs(tzMin) % 60)}`;
  const stamp = `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} `
    + `${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())} (${tz})`;
  const lines = [
    '# NetDebug 界面专项测试报告',
    '',
    '| 项目 | 值 |',
    '|---|---|',
    `| 生成时间 | ${stamp} |`,
    '| 驱动方式 | Chrome DevTools Protocol（Edge headless，真实渲染与真实接口） |',
    '| 被测页面 | 内嵌管理页（与 WebView2 同一套资源） |',
    `| 用例 | ${cases.length} |`,
    `| 断言 | ${total} |`,
    `| 失败 | ${failed} |`,
    `| 结论 | **${failed ? '失败' : '通过'}** |`,
    '',
    '## 用例明细',
    '',
    '| 用例 | 断言 | 结果 |',
    '|---|---|---|',
    ...cases.map((c) => `| ${c.name} | ${c.checks} | ${c.failures ? '❌ 失败' : '✅ 通过'} |`),
  ];
  if (failed) {
    lines.push('', '## 失败详情', '');
    for (const c of cases) {
      if (!c.failures) continue;
      for (const n of c.notes) lines.push(`- **${c.name}**：${n}`);
    }
  }
  fs.writeFileSync(process.argv[mdArg + 1], lines.join('\n') + '\n');
  console.log('报告：' + process.argv[mdArg + 1]);
}
process.exit(failed ? 1 : 0);
