import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(`http://127.0.0.1:${rt.port}/`), { method: 'PUT' })).json();
await sleep(1300);
const ws = new WebSocket(t.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0;
const pending = new Map();
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); }
});
const send = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const ev = async (x) => (await send('Runtime.evaluate', { expression: x, returnByValue: true, awaitPromise: true })).result?.value;

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1000, height: 620, deviceScaleFactor: 1, mobile: false });
await sleep(600);

console.log('标题:', await ev('document.title'));
console.log('首行:', await ev("document.querySelector('h1').textContent"));

const r = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync('docs/images/guide/26-window-only.png', Buffer.from(r.data, 'base64'));
console.log('shot 26-window-only.png');
process.exit(0);
