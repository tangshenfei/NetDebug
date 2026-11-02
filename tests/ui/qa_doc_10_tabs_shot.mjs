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
const ev = async (x) => {
  const r = await send('Runtime.evaluate', { expression: x, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
  return r.result?.value;
};
const setVal = (eid, v) =>
  ev(`(() => { const e = document.getElementById('${eid}'); e.value = ${JSON.stringify(v)};
    e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true })); })()`);

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(1200);

console.log('1) 建一个 TCP 会话并连接');
await ev("document.getElementById('btn-new-sess').click()");
await sleep(350);
await setVal('s-name', 'TCP 客户端');
await setVal('s-host', '127.0.0.1');
await setVal('s-port', '19101');
await sleep(200);
await ev("document.getElementById('s-create').click()");
await sleep(900);
await ev(`(() => {
  const c = [...document.querySelectorAll('#session-list .sess')].find(x => x.innerText.includes('TCP 客户端'));
  c.querySelector('button[data-sact=open]').click();
})()`);
await sleep(1500);
await setVal('send-input', 'GET / HTTP/1.0');
await ev("document.getElementById('send-btn').click()");
await sleep(600);

console.log('2) 再开 HTTP 和 FTP 工具');
await ev("document.getElementById('btn-http').click()");
await sleep(400);
await setVal('http-url', 'http://127.0.0.1:19102/hello.txt');
await ev("document.getElementById('http-send').click()");
await sleep(900);
await ev("document.getElementById('btn-ftp').click()");
await sleep(400);
await setVal('ftp-host', '127.0.0.1');
await setVal('ftp-port', '19104');
await setVal('ftp-user', 'anonymous');
await ev("document.getElementById('ftp-connect').click()");
await sleep(1200);

console.log('3) 回到终端标签截图');
await ev("document.querySelector('#tabs .tab').click()");
await sleep(600);
console.log('   tabs:', JSON.stringify(await ev("[...document.querySelectorAll('#tabs .tab')].map(t => t.innerText.replace(/\\s+/g,' ').trim())")));
const r = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync('docs/images/guide/28-tabs.png', Buffer.from(r.data, 'base64'));
console.log('shot 28-tabs.png');
process.exit(0);
