/*
 * UI 文档截图：基础页面 + 新建服务对话框
 */
import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const url = `http://127.0.0.1:${rt.port}/#t=${rt.token}`;
const OUT = 'docs/images/guide';
fs.mkdirSync(OUT, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const target = await (await fetch(CDP + '/json/new?' + encodeURIComponent(url), { method: 'PUT' })).json();
await sleep(1200);
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0;
const pending = new Map();
const errors = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); return; }
  if (m.method === 'Runtime.exceptionThrown')
    errors.push('EX ' + (m.params.exceptionDetails.exception?.description || '').slice(0, 200));
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error')
    errors.push('LOG ' + m.params.entry.text.slice(0, 200));
});
const send = (method, params = {}) =>
  new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const ev = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
  return r.result?.value;
};
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`${OUT}/${name}`, Buffer.from(r.data, 'base64'));
  console.log('  shot', name);
};
const waitFor = async (expr, ms = 8000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await ev(expr)) return true; await sleep(120); }
  return false;
};

await send('Runtime.enable');
await send('Log.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(1500);

console.log('1) 仪表盘');
await waitFor("document.getElementById('status-line').textContent === '就绪'");
await shot('01-dashboard.png');

console.log('2) 新建服务对话框 - TCP');
await ev("document.getElementById('btn-new').click()");
await sleep(400);
await shot('02-service-dialog.png');
console.log('   默认名称:', await ev("document.getElementById('f-name').value"));

console.log('3) 协议切换 → HTTP');
await ev(`(() => { const p = document.getElementById('f-proto'); p.value = 'http'; p.dispatchEvent(new Event('change', {bubbles:true})); })()`);
await sleep(300);
console.log('   HTTP 默认名称:', await ev("document.getElementById('f-name').value"));
console.log('   根目录区可见:', await ev("!document.getElementById('ftp-options').classList.contains('hidden')"));
await shot('03-service-dialog-http.png');

console.log('4) 协议切换 → LibreSpeed');
await ev(`(() => { const p = document.getElementById('f-proto'); p.value = 'speedtest'; p.dispatchEvent(new Event('change', {bubbles:true})); })()`);
await sleep(300);
console.log('   测速默认名称:', await ev("document.getElementById('f-name').value"));
await shot('04-service-dialog-speedtest.png');

console.log('5) 绑定方式切换');
await ev(`(() => { const k = document.getElementById('f-kind'); k.value='addr'; k.dispatchEvent(new Event('change',{bubbles:true})); })()`);
await sleep(200);
console.log('   指定 IP 时地址框可见:', await ev("!document.getElementById('wrap-addr').classList.contains('hidden')"));
console.log('   网卡选择框隐藏:', await ev("document.getElementById('wrap-iface').classList.contains('hidden')"));
await ev(`(() => { const k = document.getElementById('f-kind'); k.value='iface'; k.dispatchEvent(new Event('change',{bubbles:true})); })()`);
await sleep(200);
console.log('   网卡选项数:', await ev("document.getElementById('f-iface').options.length"));
await shot('05-service-dialog-bind.png');

await ev("document.getElementById('btn-cancel').click()");
await sleep(300);
console.log('对话框已关闭:', await ev("!document.getElementById('dlg-service').open"));

console.log('errors:', errors.length ? errors.join('\n') : 'none');
ws.close();
