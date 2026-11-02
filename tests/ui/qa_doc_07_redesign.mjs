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
const errors = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); return; }
  if (m.method === 'Runtime.exceptionThrown')
    errors.push(String(m.params.exceptionDetails.exception?.description || '').slice(0, 200));
});
const send = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const ev = async (x) => {
  const r = await send('Runtime.evaluate', { expression: x, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails).slice(0, 300));
  return r.result?.value;
};
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(`docs/images/guide/${name}`, Buffer.from(r.data, 'base64'));
  console.log('  shot', name);
};

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(1000);

console.log('1) 服务对话框（默认 TCP）');
await ev("document.getElementById('btn-new').click()");
await sleep(500);
console.log('   active tile:', await ev("document.querySelector('#f-proto-tiles .proto-tile.active')?.dataset.proto"));
await shot('02-service-dialog.png');

console.log('2) 点 HTTP 磁贴');
await ev("document.querySelector('#f-proto-tiles .proto-tile[data-proto=http]').click()");
await sleep(400);
console.log('   select:', await ev("document.getElementById('f-proto').value"));
console.log('   name  :', await ev("document.getElementById('f-name').value"));
console.log('   active :', await ev("document.querySelector('#f-proto-tiles .proto-tile.active')?.dataset.proto"));
console.log('   目录区 :', await ev("!document.getElementById('ftp-options').classList.contains('hidden')"));
await shot('03-service-dialog-http.png');

console.log('3) 点测速磁贴');
await ev("document.querySelector('#f-proto-tiles .proto-tile[data-proto=speedtest]').click()");
await sleep(350);
console.log('   name:', await ev("document.getElementById('f-name').value"));
await shot('04-service-dialog-speedtest.png');

console.log('4) 绑定范围切到指定 IP');
await ev("document.querySelector('#f-proto-tiles .proto-tile[data-proto=raw-tcp]').click()");
await sleep(200);
await ev(`(() => { const k=document.getElementById('f-kind'); k.value='addr'; k.dispatchEvent(new Event('change',{bubbles:true})); })()`);
await sleep(300);
await shot('05-service-dialog-bind.png');
await ev("document.getElementById('btn-cancel').click()");
await sleep(300);

console.log('5) 会话对话框');
await ev("document.getElementById('btn-new-sess').click()");
await sleep(400);
console.log('   active tile:', await ev("document.querySelector('#s-mode-tiles .proto-tile.active')?.dataset.mode"));
await shot('10-session-dialog.png');

console.log('6) 切到 SSH 磁贴');
await ev("document.querySelector('#s-mode-tiles .proto-tile[data-mode=ssh]').click()");
await sleep(350);
console.log('   transport:', await ev("document.getElementById('s-transport').value"));
console.log('   认证可见 :', await ev("!document.getElementById('ssh-config').classList.contains('hidden')"));
console.log('   端口     :', await ev("document.getElementById('s-port').value"));
await shot('17-ssh-dialog.png');

console.log('7) 切到串口磁贴');
await ev("document.querySelector('#s-mode-tiles .proto-tile[data-mode=serial]').click()");
await sleep(350);
console.log('   串口可见 :', await ev("!document.getElementById('serial-config').classList.contains('hidden')"));
await shot('18-serial-dialog.png');
await ev("document.getElementById('s-cancel').click()");
await sleep(200);

console.log('errors:', errors.length ? errors.join(' | ') : 'none');
process.exit(0);
