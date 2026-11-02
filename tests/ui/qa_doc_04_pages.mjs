/*
 * UI 文档截图：端口冲突、HTTP 目录列表、测速页、移动端布局
 */
import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const url = `http://127.0.0.1:${rt.port}/#t=${rt.token}`;
const OUT = 'docs/images/guide';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function openPage(target) {
  const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(target), { method: 'PUT' })).json();
  await sleep(1200);
  const ws = new WebSocket(t.webSocketDebuggerUrl);
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
    if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
    return r.result?.value;
  };
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(`${OUT}/${name}`, Buffer.from(r.data, 'base64'));
    console.log('  shot', name);
  };
  await send('Runtime.enable');
  await send('Page.enable');
  return { ws, send, ev, shot };
}
const setVal = (ev, id, v) => ev(`(() => { const e = document.getElementById('${id}'); e.value = ${JSON.stringify(v)};
  e.dispatchEvent(new Event('change', { bubbles: true })); })()`);

/* ---------- 桌面：端口冲突 + 目录列表 + 测速 ---------- */
const p = await openPage(url);
await p.send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(900);

console.log('1) 服务列表（含 HTTP 与测速）');
await p.ev("window.scrollTo(0,0)");
await sleep(800);
await p.shot('08-services-running.png');

/* HTTP 目录列表页 */
console.log('3) HTTP 目录列表');
const hp = await openPage('http://127.0.0.1:19102/');
await hp.send('Emulation.setDeviceMetricsOverride', { width: 900, height: 620, deviceScaleFactor: 1, mobile: false });
await sleep(1200);
console.log('   标题:', await hp.ev('document.title'));
await hp.shot('20-http-index.png');
await hp.ws.close();

const hd = await openPage('http://127.0.0.1:19102/');
await hd.send('Emulation.setDeviceMetricsOverride', { width: 900, height: 620, deviceScaleFactor: 1, mobile: false });
await sleep(1000);
await hd.ev("location.href = 'http://127.0.0.1:19102/sub/'");
await sleep(1200);
console.log('   目录页标题:', await hd.ev('document.title'));
await hd.shot('21-http-directory.png');
await hd.ws.close();

/* 测速页 */
console.log('4) LibreSpeed 页面');
const sp = await openPage('http://127.0.0.1:19103/');
await sp.send('Emulation.setDeviceMetricsOverride', { width: 1000, height: 760, deviceScaleFactor: 1, mobile: false });
await sleep(1600);
console.log('   标题:', await sp.ev('document.title'));
await sp.shot('22-speedtest.png');
await sp.ws.close();

/* ---------- 移动端 ---------- */
console.log('5) 移动端布局');
const m = await openPage(url);
await m.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 780, deviceScaleFactor: 2, mobile: true });
await sleep(1200);
await m.shot('23-mobile.png');
await m.ev("document.getElementById('btn-sidebar').click()");
await sleep(500);
await m.shot('24-mobile-drawer.png');
await m.ev("document.getElementById('sidebar-backdrop').click()");
await sleep(400);
await m.ev("document.getElementById('btn-new').click()");
await sleep(500);
await m.shot('25-mobile-dialog.png');
console.log('   对话框高度:', await m.ev("document.getElementById('dlg-service').getBoundingClientRect().height"));
await m.ev("document.getElementById('btn-cancel').click()");
await sleep(300);
await m.ws.close();

await p.ws.close();
