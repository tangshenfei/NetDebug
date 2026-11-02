import fs from 'node:fs';

const CDP = 'http://127.0.0.1:9280';
const rt = JSON.parse(fs.readFileSync('.data/runtime.json', 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const t = await (await fetch(CDP + '/json/new?' + encodeURIComponent(`http://127.0.0.1:${rt.port}/#t=${rt.token}`), { method: 'PUT' })).json();
await sleep(1300);
const ws = new WebSocket(t.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener('open', r, { once: true }));
let id = 0;
const pending = new Map();
const errs = [];
ws.addEventListener('message', (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m.result); pending.delete(m.id); return; }
  if (m.method === 'Runtime.exceptionThrown')
    errs.push(String(m.params.exceptionDetails.exception?.description || '').slice(0, 200));
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
await sleep(1200);

const tabsText = () => ev("[...document.querySelectorAll('#tabs .tab')].map(t => t.innerText.replace(/\\s+/g,' ').trim())");

console.log('1) 打开 HTTP 工具');
await ev("document.getElementById('btn-http').click()");
await sleep(400);
console.log('   tabs:', JSON.stringify(await tabsText()));

console.log('2) 打开 FTP 工具（应新增标签，而不是替换）');
await ev("document.getElementById('btn-ftp').click()");
await sleep(400);
console.log('   tabs:', JSON.stringify(await tabsText()));
console.log('   可见视图:', await ev("[...document.querySelectorAll('.workspace > section, .workspace > div')].filter(e => e.id && !e.classList.contains('hidden')).map(e => e.id)"));

console.log('3) 切回 HTTP 标签');
await ev("document.querySelector('#tabs .tab[data-tab]:first-child').click()");
await sleep(300);
console.log('   可见视图:', await ev("[...document.querySelectorAll('.workspace > section, .workspace > div')].filter(e => e.id && !e.classList.contains('hidden')).map(e => e.id)"));
console.log('   active :', await ev("document.querySelector('#tabs .tab.active span').textContent"));
await shot('28-tabs.png');

console.log('4) 关闭 FTP 标签');
const n0 = await ev("document.querySelectorAll('#tabs .tab').length");
await ev("document.querySelectorAll('#tabs .tab')[1].querySelector('.tab-close').click()");
await sleep(300);
const n1 = await ev("document.querySelectorAll('#tabs .tab').length");
console.log(`   tabs ${n0} -> ${n1}`, JSON.stringify(await tabsText()));

console.log('5) 关闭最后一个标签');
await ev("document.querySelector('#tabs .tab .tab-close').click()");
await sleep(300);
console.log('   tabs:', JSON.stringify(await tabsText()), 'welcome:', await ev("!document.getElementById('welcome').classList.contains('hidden')"));

console.log('errors:', errs.length ? errs.join(' | ') : 'none');
process.exit(0);
