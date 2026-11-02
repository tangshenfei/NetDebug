import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(`http://127.0.0.1:${rt.port}/#t=${rt.token}`), { method: 'PUT' })).json();
await sleep(1200);
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
await send('Network.enable');
await send('Network.setCacheDisabled', { cacheDisabled: true });
await send('Page.reload', { ignoreCache: true });
await sleep(1500);
await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 780, deviceScaleFactor: 2, mobile: true });
await sleep(400);

await ev("document.getElementById('btn-sidebar').click()");
await sleep(300);
await ev("document.getElementById('btn-new').click()");
await sleep(500);

console.log('tile width:', await ev("getComputedStyle(document.querySelector('.proto-tile')).width"));
console.log('has media:', await ev("[...document.styleSheets].some(s => { try { return [...s.cssRules].some(r => r.conditionText && r.conditionText.includes('520')) } catch(e) { return false } })"));
console.log('dialog h  :', await ev("document.getElementById('dlg-service').getBoundingClientRect().height"));
const r = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync('docs/images/guide/25-mobile-dialog.png', Buffer.from(r.data, 'base64'));
console.log('shot 25-mobile-dialog.png');
process.exit(0);
