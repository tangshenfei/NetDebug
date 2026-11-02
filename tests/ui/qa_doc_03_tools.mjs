/*
 * UI 文档截图：工具面板（HTTP / FTP / TFTP / SMB）、SSH 与串口会话、错误状态
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
const waitFor = async (expr, ms = 12000) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await ev(expr)) return true; await sleep(150); }
  return false;
};
async function setVal(id, v) {
  await ev(`(() => { const e = document.getElementById('${id}'); e.value = ${JSON.stringify(v)};
    e.dispatchEvent(new Event('change', { bubbles: true })); })()`);
}
async function newService(name, proto, port, root) {
  await ev("document.getElementById('btn-new').click()");
  await sleep(250);
  await setVal('f-name', name);
  await setVal('f-proto', proto);
  await setVal('f-port', String(port));
  await setVal('f-kind', 'any');
  await setVal('f-family', 'v4');
  if (root) await setVal('f-ftp-root', root);
  await sleep(150);
  await ev("document.getElementById('btn-create').click()");
  await waitFor(`document.getElementById('service-list').innerText.includes(${JSON.stringify(name)})`);
  await sleep(250);
}

await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 880, deviceScaleFactor: 1, mobile: false });
await sleep(1200);
await waitFor("document.getElementById('status-line').textContent === '就绪'");

const cwd = process.cwd();
console.log('1) 准备 FTP / TFTP 服务');
await newService('FTP 文件服务', 'ftp', 19104, cwd + '\\.data\\qa-ftp');
await newService('TFTP 传输服务', 'tftp', 19105, cwd + '\\.data\\qa-tftp');

/* ---------- HTTP 调试工具 ---------- */
console.log('2) HTTP 调试工具');
await ev("document.getElementById('btn-http').click()");
await sleep(400);
await setVal('http-method', 'GET');
await setVal('http-url', 'http://127.0.0.1:19102/hello.txt');
await ev("document.getElementById('http-headers').value = 'Accept: text/plain'");
await ev("document.getElementById('http-send').click()");
await waitFor("document.getElementById('http-status').textContent.startsWith('200')");
await sleep(300);
console.log('   状态:', await ev("document.getElementById('http-status').textContent"));
console.log('   响应:', (await ev("document.getElementById('http-response').textContent")).slice(0, 60));
await shot('12-http-tool.png');
await ev("document.querySelector('[data-hview=raw]').click()");
await sleep(250);
await ev("document.querySelector('.http-raw').open = true");
await sleep(300);
await shot('13-http-raw.png');

/* ---------- FTP 工具 ---------- */
console.log('3) FTP 文件工具');
await ev("document.getElementById('btn-ftp').click()");
await sleep(300);
await setVal('ftp-host', '127.0.0.1');
await setVal('ftp-port', '19104');
await setVal('ftp-user', 'anonymous');
await setVal('ftp-pass', '');
await ev("document.getElementById('ftp-connect').click()");
await waitFor("document.getElementById('ftp-list').innerText.includes('readme.txt')");
await sleep(400);
console.log('   状态:', await ev("document.getElementById('ftp-state').textContent"));
console.log('   列表:', (await ev("document.getElementById('ftp-list').innerText")).replace(/\n/g, ' | ').slice(0, 120));
await shot('14-ftp.png');

/* ---------- TFTP 工具 ---------- */
console.log('4) TFTP 传输');
await ev("document.getElementById('btn-tftp').click()");
await sleep(300);
await setVal('tftp-host', '127.0.0.1');
await setVal('tftp-port', '19105');
await setVal('tftp-remote', 'firmware.bin');
await ev("document.getElementById('tftp-get').click()");
await waitFor("document.getElementById('tftp-state').textContent.includes('下载')");
await sleep(500);
console.log('   状态:', await ev("document.getElementById('tftp-state').textContent"));
await shot('15-tftp.png');

/* ---------- SMB 工具（无服务端，展示表单） ---------- */
console.log('5) SMB 工具表单');
await ev("document.getElementById('btn-smb').click()");
await sleep(300);
await setVal('smb-host', '127.0.0.1');
await setVal('smb-share', 'share');
await setVal('smb-user', 'user');
await setVal('smb-pass', 'password');
await sleep(200);
await shot('16-smb.png');

/* ---------- SSH 会话对话框 ---------- */
console.log('6) SSH 会话对话框');
await ev("document.getElementById('btn-new-sess').click()");
await sleep(300);
await setVal('s-transport', 'ssh');
await sleep(300);
console.log('   SSH 配置可见:', await ev("!document.getElementById('ssh-config').classList.contains('hidden')"));
console.log('   网络配置隐藏:', await ev("document.getElementById('net-config').classList.contains('hidden')"));
await setVal('s-name', 'SSH 会话');
await setVal('s-host', '127.0.0.1');
await setVal('s-port', '2222');
await setVal('s-ssh-user', 'dev');
await setVal('s-ssh-pass', 'dev');
await sleep(200);
await shot('17-ssh-dialog.png');

/* ---------- 串口会话对话框 ---------- */
console.log('7) 串口会话对话框');
await setVal('s-transport', 'serial');
await sleep(300);
console.log('   串口配置可见:', await ev("!document.getElementById('serial-config').classList.contains('hidden')"));
await shot('18-serial-dialog.png');
await ev("document.getElementById('s-cancel').click()");
await sleep(300);

/* ---------- 端口冲突错误 ---------- */
console.log('8) 端口冲突提示');
await ev("document.getElementById('btn-new').click()");
await sleep(250);
await setVal('f-name', '冲突测试');
await setVal('f-proto', 'raw-tcp');
await setVal('f-port', '19101');
await setVal('f-kind', 'any');
await setVal('f-family', 'v4');
await ev("document.getElementById('btn-create').click()");
await waitFor("!document.getElementById('bind-result').classList.contains('hidden') && document.getElementById('bind-result').innerText.length > 20");
await sleep(400);
console.log('   错误:', (await ev("document.getElementById('bind-result').innerText")).replace(/\n/g, ' | ').slice(0, 140));
await shot('19-port-conflict.png');
await ev("document.getElementById('btn-cancel').click()");
await sleep(200);

ws.close();
