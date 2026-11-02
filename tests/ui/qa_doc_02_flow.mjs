/*
 * UI 文档截图：监听服务 + 客户端会话 + 终端
 */
import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const url = `http://127.0.0.1:${rt.port}/#t=${rt.token}`;
const OUT = 'docs/images/guide';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const target = await (await fetch(CDP + '/json/new?' + encodeURIComponent(url), { method: 'PUT' })).json();
await sleep(1200);
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
});
const send = (method, params = {}) =>
  new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const ev = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 400));
  return r.result?.value;
};
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`${OUT}/${name}`, Buffer.from(r.data, 'base64'));
  console.log('  shot', name);
};
const waitFor = async (expr, ms = 10000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await ev(expr)) return true; await sleep(120); }
  return false;
};
async function setVal(id, v) {
  await ev(`(() => { const e = document.getElementById('${id}'); e.value = ${JSON.stringify(v)};
    e.dispatchEvent(new Event('change', { bubbles: true })); })()`);
}

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(1200);
await waitFor("document.getElementById('status-line').textContent === '就绪'");

/* ---------- 1. 用界面创建 TCP 回声服务 ---------- */
console.log('1) 新建 TCP 回声服务');
await ev("document.getElementById('btn-new').click()");
await sleep(300);
await setVal('f-name', 'TCP 回声 19101');
await setVal('f-proto', 'raw-tcp');
await setVal('f-port', '19101');
await setVal('f-kind', 'any');
await setVal('f-family', 'v4');
await ev("document.getElementById('f-echo').checked = true");
await sleep(200);
await shot('06-service-filled.png');
await ev("document.getElementById('btn-test').click()");
await waitFor("!document.getElementById('bind-result').classList.contains('hidden') && document.getElementById('bind-result').innerText.length > 0");
await sleep(300);
console.log('   绑定测试:', (await ev("document.getElementById('bind-result').innerText")).replace(/\n/g, ' | '));
await shot('07-bind-test.png');
await ev("document.getElementById('btn-create').click()");
await waitFor("document.getElementById('service-list').innerText.includes('TCP 回声 19101')");
console.log('   服务卡片:', (await ev("document.getElementById('service-list').innerText")).replace(/\n/g, ' / '));

/* ---------- 2. 新建 HTTP 文件服务 + 测速服务 ---------- */
console.log('2) 新建 HTTP 文件服务');
const wwwPath = process.cwd().replace(/\\/g, '\\\\') + '\\\\.data\\\\qa-www';
await ev("document.getElementById('btn-new').click()");
await sleep(250);
await setVal('f-name', 'HTTP 文件服务');
await setVal('f-proto', 'http');
await setVal('f-port', '19102');
await setVal('f-kind', 'any');
await setVal('f-family', 'v4');
await setVal('f-ftp-root', wwwPath.replace(/\\\\/g, '\\'));
await sleep(200);
await ev("document.getElementById('btn-create').click()");
await waitFor("document.getElementById('service-list').innerText.includes('HTTP 文件服务')");

console.log('3) 新建 LibreSpeed 测速服务');
await ev("document.getElementById('btn-new').click()");
await sleep(250);
await setVal('f-name', 'LibreSpeed 测速');
await setVal('f-proto', 'speedtest');
await setVal('f-port', '19103');
await setVal('f-kind', 'any');
await setVal('f-family', 'v4');
await ev("document.getElementById('btn-create').click()");
await waitFor("document.getElementById('service-list').innerText.includes('LibreSpeed 测速')");
await sleep(600);
await shot('08-services-running.png');
console.log('   服务数:', await ev("document.querySelectorAll('#service-list .svc').length"));

/* ---------- 3. 编辑对话框 ---------- */
console.log('4) 编辑服务');
await ev(`(() => {
  const card = [...document.querySelectorAll('#service-list .svc')].find(c => c.innerText.includes('HTTP 文件服务'));
  card.querySelector('button[data-act=edit]').click();
})()`);
await sleep(400);
console.log('   标题:', await ev("document.querySelector('#dlg-service h2').textContent"));
console.log('   按钮:', await ev("document.getElementById('btn-create').textContent"));
console.log('   带出端口:', await ev("document.getElementById('f-port').value"));
await shot('09-service-edit.png');
await ev("document.getElementById('btn-cancel').click()");
await sleep(300);

/* ---------- 4. 新建 TCP 客户端会话 ---------- */
console.log('5) 新建 TCP 客户端会话');
await ev("document.getElementById('btn-new-sess').click()");
await sleep(300);
await setVal('s-name', 'TCP 客户端');
await setVal('s-transport', 'tcp');
await setVal('s-proto', 'raw');
await setVal('s-host', '127.0.0.1');
await setVal('s-port', '19101');
await setVal('s-family', 'v4');
await sleep(200);
await shot('10-session-dialog.png');
await ev("document.getElementById('s-create').click()");
await waitFor("document.getElementById('session-list').innerText.includes('TCP 客户端')");
await sleep(300);
console.log('   会话卡片:', (await ev("document.getElementById('session-list').innerText")).replace(/\n/g, ' / '));

/* ---------- 5. 打开终端并收发 ---------- */
console.log('6) 终端收发');
await ev("document.querySelector('#session-list .sess button[data-sact=open]').click()");
await waitFor("!document.getElementById('terminal').classList.contains('hidden') && document.getElementById('term-state').textContent === 'open'");
await sleep(400);
await setVal('send-input', 'hello from ui');
await ev("document.getElementById('send-btn').click()");
const echoed = await waitFor("document.getElementById('term-out').textContent.includes('RX hello from ui')");
console.log('   收到回显:', echoed);
await sleep(400);
await shot('11-terminal.png');
console.log('   终端文本:', (await ev("document.getElementById('term-out').innerText")).replace(/\n/g, ' | ').slice(0, 160));
console.log('   协议标签:', await ev("document.getElementById('term-proto').textContent"));

ws.close();
