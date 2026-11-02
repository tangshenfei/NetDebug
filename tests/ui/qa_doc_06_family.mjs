import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(`http://127.0.0.1:${rt.port}/#t=${rt.token}`), { method: 'PUT' })).json();
await sleep(1400);
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
const setVal = async (eid, v) => ev(`(() => { const e = document.getElementById('${eid}'); e.value = ${JSON.stringify(v)};
  e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true })); })()`);

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(800);

await ev("document.getElementById('btn-new-sess').click()");
await sleep(350);
await setVal('s-name', 'TCP 客户端');
await setVal('s-transport', 'tcp');
await setVal('s-proto', 'raw');
await setVal('s-host', '127.0.0.1');
await setVal('s-port', '19101');
await sleep(300);
console.log('family disabled:', await ev("document.getElementById('s-family').disabled"));
console.log('family value   :', await ev("document.getElementById('s-family').value"));
console.log('hint           :', await ev("document.getElementById('s-family-hint').textContent"));

const r = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync('docs/images/guide/10-session-dialog.png', Buffer.from(r.data, 'base64'));
console.log('shot 10-session-dialog.png');
process.exit(0);
