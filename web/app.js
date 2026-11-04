(() => {
  'use strict';

  const $ = (s) => document.querySelector(s);
  const token = (() => {
    const p = new URLSearchParams(location.hash.replace(/^#/, ''));
    return p.get('t') || '';
  })();

  if (!token) {
    const host = location.host || '127.0.0.1:7801';
    document.title = 'NetDebug · 请使用程序窗口';
    document.body.innerHTML = `
      <div style="max-width:560px;margin:12vh auto;padding:0 24px;font:14px/1.8 system-ui,'Segoe UI',sans-serif;color:#c9d1d9">
        <h1 style="font-size:20px;margin:0 0 10px">请使用 NetDebug 主窗口</h1>
        <p style="color:#8b949e;margin:0 0 16px">管理界面只在程序窗口里提供，这里是 <code>${host}</code> 的本地接口，不接受浏览器直接访问。</p>
        <p style="color:#8b949e;margin:0">需要用命令行跑（<code>--console</code>）时，启动后终端会打印带 token 的调试接口地址。</p>
      </div>`;
    return;
  }

  const state = {
    meta: null,
    ifaces: [],
    services: [],
    sessions: [],
    currentService: null,
    currentConn: null,
    editService: null,
    ws: null,
    events: null,
    eventsTimer: null,
    term: { hex: false, ts: true, rx: 0, tx: 0 },
    http: { data: null, bytes: null, view: 'text' },
    ftp: { id: null, pwd: '/', entries: [] },
    smb: { id: null, pwd: '/', entries: [] },
    autoScroll: true,
    tabs: [],
    activeTab: null,
    tabSeq: 1,
    recent: {},
    presets: [],
  };

  const paintTimers = new Map();

  const SERVICE_DEFAULTS = {
    'raw-tcp': 'TCP 调试服务',
    'raw-udp': 'UDP 调试服务',
    telnet: 'Telnet 服务',
    ftp: 'FTP 文件服务',
    tftp: 'TFTP 传输服务',
    http: 'HTTP 文件服务',
    speedtest: 'LibreSpeed 测速',
  };

  /* 各协议的默认端口：取「常用端口 +1000」这类非特权端口，
     避免 Linux 上绑定 <1024 需要 root，同时保留辨识度。
     新建时在基准上随机偏移，连续新建多个不会都撞在同一个端口。 */
  const SERVICE_PORTS = {
    'raw-tcp': 9000,
    'raw-udp': 9100,
    telnet: 2323,
    ftp: 2121,
    tftp: 6969,
    http: 8080,
    speedtest: 8081,
  };
  const PORT_SPREAD = 100;

  /* 协议在界面上显示的名字 */
  const PROTO_NAMES = {
    'raw-tcp': 'TCP',
    'raw-udp': 'UDP',
    telnet: 'Telnet',
    ftp: 'FTP',
    tftp: 'TFTP',
    http: 'HTTP',
    speedtest: '测速',
  };

  /* 终端顶部展示用的协议名（后端返回的是内部标识） */
  const protoLabels = {
    'raw-tcp': 'TCP',
    'tcp-client': 'TCP',
    'raw-udp': 'UDP',
    'udp-client': 'UDP',
    telnet: 'Telnet',
    ftp: 'FTP',
    tftp: 'TFTP',
    ssh: 'SSH',
    serial: '串口',
    http: 'HTTP',
  };

  const h = (s) =>
    String(s === undefined || s === null ? '' : s).replace(/[&<>"']/g, (c) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    }[c]));

  function fmtBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(2) + ' MB';
    return (n / 1073741824).toFixed(2) + ' GB';
  }

  function fmtTime(ms) {
    const d = new Date(ms);
    const p = (x, n = 2) => String(x).padStart(n, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
  }

  async function api(path, opts = {}) {
    const headers = Object.assign({}, opts.headers || {});
    headers['X-Auth-Token'] = token;
    if (opts.body !== undefined && !headers['Content-Type'])
      headers['Content-Type'] = 'application/json';
    const r = await fetch(path, Object.assign({}, opts, { headers }));
    const ct = r.headers.get('content-type') || '';
    const data = ct.includes('json') ? await r.json() : await r.text();
    if (!r.ok) {
      const err = new Error((data && data.message) || r.statusText);
      err.data = data;
      err.status = r.status;
      throw err;
    }
    return data;
  }

  let toastTimer = null;
  function toast(msg) {
    const el = $('#toast');
    el.textContent = msg;
    el.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.classList.add('hidden'), 4500);
  }

  function errText(e) {
    if (!e) return '未知错误';
    let s = e.message || String(e);
    if (e.data && e.data.detail) s += '：' + e.data.detail;
    if (e.data && Array.isArray(e.data.slots)) {
      const bad = e.data.slots.filter((x) => !x.ok);
      if (bad.length) s += '（' + bad.map((x) => x.addr + ' ' + (x.hint || '')).join('；') + '）';
    }
    return s;
  }

  // ---------- 服务列表 ----------

  function bindLabel(b) {
    const fam = { v4: 'IPv4', v6: 'IPv6', dual: '双栈' }[b.family] || b.family;
    const kind = { any: '全部地址', iface: '网卡', addr: '指定 IP' }[b.kind] || b.kind;
    return `${kind} · ${fam}`;
  }

  function renderServices() {
    const protoNames = PROTO_NAMES;
    const root = $('#service-list');
    if (!state.services.length) {
      root.innerHTML = '<div class="muted" style="padding:6px 10px;font-size:12px">还没有监听服务</div>';
      return;
    }
    root.innerHTML = state.services
      .map((s) => {
        const addrs = (s.slots || [])
          .filter((x) => x.ok)
          .map((x) => '<div>' + h(x.addr) + '</div>')
          .join('');
        return `<div class="svc" data-id="${s.id}">
          <div class="svc-head">
            <span class="svc-name">${h(s.name)}</span>
            <span class="svc-badge ${s.running ? 'on' : ''}">${s.running ? '运行中' : '已停止'}</span>
          </div>
          <div class="svc-meta">${h(protoNames[s.proto] || s.proto)} · ${h(bindLabel(s.bind))} · :${s.bind.port}${(s.proto === 'http' || s.proto === 'speedtest') ? ` · 请求 ${s.requests || 0}` : ''}</div>
          ${addrs ? '<div class="svc-addrs">' + addrs + '</div>' : ''}
          <div class="svc-actions">
            <button class="mini-btn" data-act="${s.running ? 'stop' : 'start'}" data-id="${s.id}">${s.running ? '停止' : '启动'}</button>
            ${(s.proto === 'http' || s.proto === 'speedtest')
              ? `<button class="mini-btn" data-act="conns" data-id="${s.id}" title="查看进行中请求">活跃 ${s.conns || 0}</button>`
              : `<button class="mini-btn" data-act="conns" data-id="${s.id}">连接 ${s.conns || 0}</button>`}
            <button class="mini-btn" data-act="edit" data-id="${s.id}">编辑</button>
            <button class="mini-btn danger" data-act="del" data-id="${s.id}">删除</button>
          </div>
        </div>`;
      })
      .join('');
  }

  function updateLanWarning() {
    const exposed = state.services.some((s) => {
      if (!s.running) return false;
      if (s.bind.kind === 'any') return true;
      if (s.bind.kind === 'iface') return true;
      return false;
    });
    $('#lan-warning').classList.toggle('hidden', !exposed);
  }

  async function refreshServices() {
    try {
      state.services = await api('/api/services');
      renderServices();
      updateLanWarning();
      if (state.currentService) {
        const s = state.services.find((x) => x.id === state.currentService);
        if (s) renderConns(s.id);
      }
      const active = tabById(state.activeTab);
      if (active && (active.kind === 'svc' || active.kind === 'log')) schedulePaint(active);
    } catch (e) {
      $('#status-line').textContent = '后端连接失败';
    }
  }

  // ---------- 客户端会话 ----------

  function sessStateLabel(st) {
    return { idle: '未连接', connecting: '连接中', open: '已连接', error: '失败' }[st] || st;
  }

  /* 某个连接对应的终端标签（没有则返回 undefined） */
  function termTabForConn(connId) {
    if (!connId) return undefined;
    return state.tabs.find((t) => t.kind === 'term' && t.connId === connId);
  }

  function renderSessions() {
    const root = $('#session-list');
    if (!state.sessions.length) {
      root.innerHTML = '<div class="muted" style="padding:6px 10px;font-size:12px">还没有客户端会话</div>';
      return;
    }
    root.innerHTML = state.sessions
      .map(
        (s) => `<div class="sess" data-sess="${s.id}">
          <div class="svc-head">
            <span class="svc-name">${h(s.name)}</span>
            <span class="sess-badge ${h(s.state)}">${h(sessStateLabel(s.state))}</span>
          </div>
          <div class="svc-meta">${h(
            s.transport === 'serial'
              ? `serial · ${s.serial_port} @${s.baud}`
              : s.transport === 'ssh'
                ? `ssh · ${s.user || ''}@${s.host}:${s.port}`
                : `${s.protocol === 'telnet' ? 'telnet' : (s.transport || 'tcp')} · ${s.host}:${s.port} · ${s.family}`
          )}</div>
          <div class="svc-actions">
            <button class="mini-btn" data-sact="open" data-id="${s.id}">${h(
              termTabForConn(s.conn_id) ? '切到终端' : s.state === 'open' ? '打开终端' : '连接'
            )}</button>
            ${s.state === 'open' ? `<button class="mini-btn" data-sact="disconnect" data-id="${s.id}">断开</button>` : ''}
            <button class="mini-btn danger" data-sact="del" data-id="${s.id}">删除</button>
          </div>
        </div>`
      )
      .join('');
  }

  async function refreshSessions() {
    try {
      state.sessions = await api('/api/sessions');
      renderSessions();
    } catch (e) {}
  }

  async function openSession(id) {
    const s = state.sessions.find((x) => x.id === id);
    if (!s) return;
    if (s.state === 'open' && s.conn_id) {
      connectTerminal(s.conn_id, id);
      return;
    }
    try {
      const r = await api(`/api/sessions/${id}/connect`, { method: 'POST' });
      connectTerminal(r.conn_id, id);
      await refreshSessions();
    } catch (e) {
      toast(errText(e));
      await refreshSessions();
    }
  }

  function fillSessionIfaces() {
    const sel = $('#s-iface');
    sel.innerHTML = state.ifaces
      .map((i) => {
        const addrs = i.addrs.map((a) => a.addr).join(', ');
        return `<option value="${h(i.id)}">${h(i.name)} — ${h(addrs || '无地址')}</option>`;
      })
      .join('');
  }

  function updateSessionSource() {
    const v = $('#s-source').value;
    $('#s-wrap-iface').classList.toggle('hidden', v !== 'iface');
    $('#s-wrap-addr').classList.toggle('hidden', v !== 'addr');
  }

  async function fillSerialPorts() {
    const sel = $('#s-serial-port');
    const hint = $('#s-serial-hint');
    try {
      const ports = await api('/api/serial/ports');
      if (!ports.length) {
        sel.innerHTML = '<option value="">未检测到串口</option>';
        sel.disabled = true;
        hint.textContent = '本机没有已注册的串口设备。USB 转串口需要先装驱动（设备管理器里出现「端口 (COM 和 LPT)」后点「刷新」）。';
        return;
      }
      sel.disabled = false;
      hint.textContent = '';
      sel.innerHTML = ports
        .map((p) => `<option value="${h(p.port)}">${h(p.port)} — ${h(p.desc)}</option>`)
        .join('');
    } catch (e) {
      sel.innerHTML = '<option value="">读取串口列表失败</option>';
      sel.disabled = true;
      hint.textContent = `读取串口列表失败：${errText(e)}`;
    }
  }

  /* 目标如果是字面 IP，地址族由目标决定，不需要用户再选一次 */
  function literalFamily(host) {
    const s = String(host || '').trim();
    if (!s) return null;
    if (/^\d{1,3}(\.\d{1,3}){3}$/.test(s)) {
      const ok = s.split('.').every((p) => Number(p) <= 255);
      return ok ? 'v4' : null;
    }
    if (s.includes(':')) return 'v6'; /* ::1、fe80::1%3、[::1] */
    return null;
  }

  function syncSessionFamily() {
    const sel = $('#s-family');
    const hint = $('#s-family-hint');
    const fam = literalFamily($('#s-host').value);
    if (fam) {
      sel.value = fam;
      sel.disabled = true;
      hint.textContent = fam === 'v4' ? '目标已是 IPv4 地址，自动按 IPv4 连接' : '目标已是 IPv6 地址，自动按 IPv6 连接';
    } else {
      sel.disabled = false;
      hint.textContent = '';
    }
  }

  /* 协议磁贴与隐藏 select 双向同步 */
  function syncServiceTiles() {
    const v = $('#f-proto').value;
    document.querySelectorAll('#f-proto-tiles .proto-tile').forEach((b) =>
      b.classList.toggle('active', b.dataset.proto === v));
  }

  function sessionMode() {
    const t = $('#s-transport').value;
    if (t === 'serial' || t === 'ssh') return t;
    if (t === 'udp') return 'udp';
    return $('#s-proto').value === 'telnet' ? 'telnet' : 'tcp-raw';
  }

  function syncModeTiles() {
    const m = sessionMode();
    document.querySelectorAll('#s-mode-tiles .proto-tile').forEach((b) =>
      b.classList.toggle('active', b.dataset.mode === m));
  }

  function syncProtoTransport() {    const isTelnet = $('#s-proto').value === 'telnet';
    const isSerial = $('#s-transport').value === 'serial';
    const isSsh = $('#s-transport').value === 'ssh';
    const udp = $('#s-transport').querySelector('option[value="udp"]');
    const telnetOpt = $('#s-proto').querySelector('option[value="telnet"]');
    telnetOpt.disabled = isSerial || isSsh;
    if (isSerial || isSsh) $('#s-proto').value = 'raw';
    udp.disabled = isTelnet || isSerial || isSsh;
    if ((isTelnet || isSerial || isSsh) && $('#s-transport').value === 'udp')
      $('#s-transport').value = 'tcp';
    $('#net-config').classList.toggle('hidden', isSerial);
    $('#serial-config').classList.toggle('hidden', !isSerial);
    $('#ssh-config').classList.toggle('hidden', !isSsh);
    syncModeTiles();
  }

  async function createSession() {
    const transport = $('#s-transport').value;
    let body;

    if (transport === 'ssh') {
      const host = $('#s-host').value.trim();
      if (!host) {
        toast('主机不能为空');
        return;
      }
      body = {
        name: $('#s-name').value || host,
        host,
        port: Number($('#s-port').value || 22),
        family: $('#s-family').value,
        transport: 'ssh',
        protocol: 'ssh',
        user: $('#s-ssh-user').value.trim() || 'root',
        password: $('#s-ssh-pass').value,
        key_path: $('#s-ssh-key').value.trim(),
        compat: $('#s-ssh-compat').checked ? 1 : 0,
      };
    } else if (transport === 'serial') {
      const port = $('#s-serial-port').value;
      if (!port) {
        toast('没有可用串口：先插上设备并装好驱动，再点「刷新」');
        $('#s-serial-port').scrollIntoView({ block: 'center' });
        return;
      }
      body = {
        name: $('#s-name').value || port,
        transport: 'serial',
        protocol: 'serial',
        serial_port: port,
        baud: Number($('#s-baud').value || 115200),
        data_bits: Number($('#s-data-bits').value || 8),
        parity: Number($('#s-parity').value || 0),
        stop_bits: Number($('#s-stop-bits').value || 1),
        flow: Number($('#s-flow').value || 0),
        dtr: $('#s-dtr').checked ? 1 : 0,
        rts: $('#s-rts').checked ? 1 : 0,
      };
    } else {
      const host = $('#s-host').value.trim();
      if (!host) {
        toast('目标地址不能为空');
        return;
      }
      body = {
        name: $('#s-name').value || host,
        host,
        port: Number($('#s-port').value || 0),
        family: $('#s-family').value,
        transport,
        protocol: $('#s-proto').value,
        iface_id: '',
        src_addr: '',
      };
      const src = $('#s-source').value;
      if (src === 'iface') body.iface_id = $('#s-iface').value;
      if (src === 'addr') body.src_addr = $('#s-addr').value.trim();
    }

    try {
      await api('/api/sessions', { method: 'POST', body: JSON.stringify(body) });
      $('#dlg-session').close();
      await refreshSessions();
    } catch (e) {
      toast(errText(e));
    }
  }

  async function showConns(id) {
    state.currentService = id;
    renderConns(id);
  }

  function connRows(list) {
    return list
      .map(
        (c) =>
          `<div class="conn ${state.currentConn === c.id ? 'active' : ''}" data-conn="${c.id}">
            <span class="dir">${h(c.proto)}</span>
            <span class="peer">${h(c.peer)}</span>
            <span class="dir">${fmtBytes(c.rx)}</span>
          </div>`
      )
      .join('');
  }

  async function renderConns(id) {
    const root = $('#conn-list');
    const svc = state.services.find((x) => x.id === id);
    const httpLike = !!svc && (svc.proto === 'http' || svc.proto === 'speedtest');
    try {
      const list = await api(`/api/services/${id}/connections`);

      if (svc && !svc.running) {
        $('#conn-count').textContent = '';
        root.innerHTML = '<div class="conn-note">服务已停止</div>';
        return;
      }

      /* HTTP / LibreSpeed 的请求毫秒级完成，不登记在长连接表里，
         卡片上的「活跃 N」统计的是在途请求数，这里要按同一口径展示。 */
      if (httpLike) {
        const active = (svc && svc.conns) || 0;
        const total = (svc && svc.requests) || 0;
        $('#conn-count').textContent = `${active} 进行中`;
        root.innerHTML =
          `<div class="conn-stat">
             <div class="conn-stat-row"><span>进行中请求</span><b>${active}</b></div>
             <div class="conn-stat-row"><span>累计请求</span><b>${total}</b></div>
             <div class="conn-note">HTTP / LibreSpeed 的请求处理极快，不登记为长连接，因此这里按请求数统计。</div>
           </div>` + connRows(list);
        return;
      }

      $('#conn-count').textContent = list.length ? `${list.length} 条` : '';
      if (!list.length) {
        root.innerHTML = '<div class="conn-note">暂无连接</div>';
        return;
      }
      root.innerHTML = connRows(list);
    } catch (e) {
      root.innerHTML = '<div class="conn-note">读取失败</div>';
    }
  }

  // ---------- 标签页与视图 ----------

  const TAB_VIEWS = {
    term: '#terminal',
    http: '#http-view',
    ftp: '#ftp-view',
    smb: '#smb-view',
    tftp: '#tftp-view',
    log: '#log-view',
    svc: '#svc-view',
  };
  const TOOL_TITLES = { http: 'HTTP 调试', ftp: 'FTP 文件', smb: 'SMB 文件', tftp: 'TFTP 传输' };

  function tabById(id) {
    return state.tabs.find((t) => t.id === id) || null;
  }

  function activeTab() {
    return tabById(state.activeTab);
  }

  function activeTermTab() {
    const t = activeTab();
    return t && t.kind === 'term' ? t : null;
  }

  function renderTabs() {
    const root = $('#tabs');
    /* 会话卡片的按钮文字取决于「终端是否已打开」，标签变化时一并刷新 */
    if (typeof renderSessions === 'function' && state.sessions.length) renderSessions();
    if (!state.tabs.length) {
      root.innerHTML = '';
      return;
    }
    root.innerHTML = state.tabs
      .map(
        (t) => `<div class="tab${t.id === state.activeTab ? ' active' : ''}" data-tab="${t.id}" title="${h(
          t.title
        )}"><span>${h(t.title)}</span><button class="tab-close" data-close="${t.id}" title="关闭">×</button></div>`
      )
      .join('');
  }

  function hideAllViews() {
    $('#welcome').classList.add('hidden');
    Object.values(TAB_VIEWS).forEach((sel) => $(sel).classList.add('hidden'));
  }

  function showWelcome() {
    state.activeTab = null;
    state.currentConn = null;
    hideAllViews();
    $('#welcome').classList.remove('hidden');
    renderTabs();
  }

  function activateTab(id) {
    const tab = tabById(id);
    if (!tab) return;
    state.activeTab = tab.id;
    state.currentConn = tab.kind === 'term' ? tab.connId : null;
    hideAllViews();
    const view = TAB_VIEWS[tab.kind];
    if (view) $(view).classList.remove('hidden');
    if (tab.kind === 'term') {
      paintTerminal(tab);
      ensureTermSocket(tab);
      applyTermView(tab);
      const out = $('#term-out');
      out.scrollTop = out.scrollHeight;
    } else if (tab.kind === 'ftp') {
      renderFtp();
    } else if (tab.kind === 'smb') {
      renderSmb();
    } else if (tab.kind === 'http') {
      renderHttpBody();
    } else if (tab.kind === 'log') {
      paintLog(tab);
    } else if (tab.kind === 'svc') {
      paintServiceDetail(tab);
    }
    renderTabs();
  }

  /* 打开一个视图：同类已存在就直接切过去（终端按连接、日志按服务区分） */
  function openTab(spec) {
    const exist = state.tabs.find((t) => {
      if (spec.kind === 'term') return t.kind === 'term' && t.connId === spec.connId;
      if (spec.kind === 'log' || spec.kind === 'svc') return t.kind === spec.kind && t.svcId === spec.svcId;
      return t.kind === spec.kind;
    });
    if (exist) {
      activateTab(exist.id);
      return exist;
    }
    const tab = Object.assign({ id: state.tabSeq++ }, spec);
    if (tab.kind === 'term') {
      tab.lines = [];
      tab.rx = 0;
      tab.tx = 0;
      tab.state = 'connecting';
      tab.proto = '';
      tab.peer = '';
      tab.local = '';
      tab.ws = null;
      tab.view = 'term'; /* term = xterm 终端仿真，log = 日志视图 */
    }
    if (tab.kind === 'log') tab.rows = [];
    state.tabs.push(tab);
    activateTab(tab.id);
    return tab;
  }

  function closeTab(id) {
    const idx = state.tabs.findIndex((t) => t.id === id);
    if (idx < 0) return;
    const tab = state.tabs[idx];
    if (tab.kind === 'term' && tab.ws) {
      try { tab.ws.close(); } catch (e) {}
      tab.ws = null;
    }
    if (tab.kind === 'term') disposeXterm(tab);
    state.tabs.splice(idx, 1);
    if (state.activeTab === id) {
      const next = state.tabs[idx] || state.tabs[idx - 1];
      if (next) activateTab(next.id);
      else showWelcome();
    } else {
      renderTabs();
    }
  }

  function closeTabsForConn(connId) {
    if (!connId) return false;
    const hits = state.tabs.filter((t) => t.kind === 'term' && t.connId === connId);
    hits.forEach((t) => closeTab(t.id));
    return hits.length > 0;
  }

  /* 打开某个服务的实时日志（对端 / 请求 / 关闭） */
  function openServiceLog(svcId) {
    const svc = state.services.find((s) => s.id === svcId);
    const tab = openTab({
      kind: 'log',
      svcId,
      title: `${svc ? svc.name : '服务'} · 日志`,
    });
    if (!tab.rows) tab.rows = [];
    if (state.activeTab === tab.id) paintLog(tab);
    return tab;
  }

  function fmtClock(ms) {
    const d = new Date(ms || Date.now());
    const p = (x) => String(x).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  function fmtDur(ms) {
    if (ms == null) return '';
    if (ms < 1000) return `${Math.round(ms)}ms`;
    return `${(ms / 1000).toFixed(2)}s`;
  }

  function logRowHtml(r) {
    const open = r.state === 'open';
    return `<div class="log-row ${open ? 'open' : 'closed'}">
      <span class="log-time">${fmtClock(r.ts)}</span>
      <span class="log-peer">${h(r.peer || '-')}</span>
      <span class="log-detail">${h(r.detail || '')}</span>
      <span class="log-state">${open ? '进行中' : '已关闭'}</span>
      <span class="log-dur">${fmtDur(r.dur)}</span>
    </div>`;
  }

  function paintLog(tab) {
    const svc = state.services.find((s) => s.id === tab.svcId);
    $('#log-title').textContent = `${svc ? svc.name : '服务'} · 日志`;
    const open = tab.rows.filter((r) => r.state === 'open').length;
    $('#log-count').textContent = tab.rows.length
      ? `${tab.rows.length} 条${open ? ` · ${open} 进行中` : ''}`
      : '';
    const list = $('#log-list');
    list.innerHTML = tab.rows.length
      ? tab.rows.map(logRowHtml).join('')
      : '<div class="log-empty">暂无记录。该服务收到请求或建立连接后会实时出现在这里。</div>';
    if ($('#log-follow').checked) list.scrollTop = list.scrollHeight;
  }

  /* 把后端事件应用到所有匹配的日志标签页 */
  function applyServiceEvent(m) {
    if (!m || !m.svc) return;
    recordRecent(m);
    const tabs = state.tabs.filter((t) => (t.kind === 'log' || t.kind === 'svc') && t.svcId === m.svc);
    if (!tabs.length) return;
    for (const tab of tabs) {
      if (tab.kind === 'svc') {
        schedulePaint(tab);
        continue;
      }
      if (!tab.rows) tab.rows = [];
      const isStart = m.t === 'req_start' || m.t === 'conn_open';
      const key = m.t === 'req_start' || m.t === 'req_end' ? `r${m.seq}` : `c${m.conn}`;
      if (isStart) {
        tab.rows.push({
          key,
          ts: m.ts,
          peer: m.peer,
          detail: m.t === 'conn_open' ? m.proto : '',
          state: 'open',
          dur: null,
        });
        if (tab.rows.length > 500) tab.rows.shift();
      } else if (m.t === 'req_end' || m.t === 'conn_close') {
        const row = tab.rows.find((r) => r.key === key);
        if (row) {
          row.state = 'closed';
          if (m.dur_ms != null) row.dur = m.dur_ms;
          if (m.method) row.detail = `${m.method} ${m.path || ''}`.trim();
          if (m.t === 'conn_close') row.dur = m.ts && row.ts ? m.ts - row.ts : null;
        }
      } else {
        continue;
      }
      schedulePaint(tab);
    }
  }

  /* 服务详情页的「最近活动」：与是否打开日志标签无关，始终记录最近 20 条 */
  function recordRecent(m) {
    if (!state.recent[m.svc]) state.recent[m.svc] = [];
    const list = state.recent[m.svc];
    const key = m.t === 'req_start' || m.t === 'req_end' ? `r${m.seq}` : `c${m.conn}`;
    if (m.t === 'req_start' || m.t === 'conn_open') {
      list.push({ key, ts: m.ts, peer: m.peer, detail: m.t === 'conn_open' ? m.proto : '', state: 'open' });
      if (list.length > 20) list.shift();
    } else {
      const row = list.find((r) => r.key === key);
      if (row) {
        row.state = 'closed';
        if (m.method) row.detail = `${m.method} ${m.path || ''}`.trim();
      }
    }
  }

  /* 高频事件下合并重绘，避免速度测试把页面刷爆 */
  function schedulePaint(tab) {
    if (paintTimers.has(tab.id)) return;
    paintTimers.set(
      tab.id,
      setTimeout(() => {
        paintTimers.delete(tab.id);
        if (state.activeTab !== tab.id) return;
        if (tab.kind === 'log') paintLog(tab);
        else if (tab.kind === 'svc') paintServiceDetail(tab);
      }, 200)
    );
  }

  /* ---------- 服务详情 ---------- */

  function fmtUptime(ms) {
    if (!ms) return '—';
    const total = Math.floor(ms / 1000);
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    const s = total % 60;
    if (h) return `${h}h ${m}m`;
    if (m) return `${m}m ${s}s`;
    return `${s}s`;
  }

  function openServiceDetail(svcId) {
    const svc = state.services.find((s) => s.id === svcId);
    const tab = openTab({ kind: 'svc', svcId, title: `服务 · ${svc ? svc.name : svcId}` });
    if (state.activeTab === tab.id) paintServiceDetail(tab);
    return tab;
  }

  function recentRowsHtml(svcId) {
    const rows = (state.recent[svcId] || []).slice(-8).reverse();
    if (!rows.length) return '<div class="conn-note">暂无活动</div>';
    return rows
      .map(
        (r) => `<div class="recent-row ${r.state === 'open' ? 'open' : 'closed'}">
          <span class="r-time">${fmtClock(r.ts)}</span>
          <span class="r-peer">${h(r.peer || '-')}</span>
          <span class="r-detail">${h(r.detail || '')}</span>
          <span class="r-state">${r.state === 'open' ? '进行中' : '已结束'}</span>
        </div>`
      )
      .join('');
  }

  function paintServiceDetail(tab) {
    const svc = state.services.find((s) => s.id === tab.svcId);
    if (!svc) {
      $('#svc-title').textContent = '服务已删除';
      $('#svc-badge').textContent = '—';
      $('#svc-badge').className = 'svc-badge';
      $('#svc-meta').textContent = '';
      $('#svc-metrics').innerHTML = '';
      $('#svc-slots').innerHTML = '<div class="conn-note">服务已不存在，可以关掉这个标签</div>';
      $('#svc-recent').innerHTML = '';
      return;
    }
    const title = `服务 · ${svc.name}`;
    if (tab.title !== title) {
      tab.title = title;
      renderTabs();
    }
    $('#svc-title').textContent = svc.name;
    $('#svc-badge').textContent = svc.running ? '运行中' : '已停止';
    $('#svc-badge').className = 'svc-badge' + (svc.running ? ' on' : '');
    $('#svc-meta').textContent = `${PROTO_NAMES[svc.proto] || svc.proto} · ${bindLabel(svc.bind)} · :${svc.bind.port}`;
    $('#svc-toggle').textContent = svc.running ? '停止' : '启动';

    const slots = svc.slots || [];
    const okSlots = slots.filter((x) => x.ok).length;
    const httpLike = svc.proto === 'http' || svc.proto === 'speedtest';
    const metrics = [
      ['进行中', String(svc.conns || 0)],
      ['峰值并发', String(svc.peak || 0)],
      ['累计请求', httpLike || svc.requests ? String(svc.requests || 0) : '—'],
      ['运行时长', svc.running ? fmtUptime(svc.uptime_ms) : '—'],
      ['监听槽位', slots.length ? `${okSlots} / ${slots.length}` : '—'],
    ];
    $('#svc-metrics').innerHTML = metrics
      .map(([k, v]) => `<div class="metric"><span>${h(k)}</span><b>${h(v)}</b></div>`)
      .join('');

    $('#svc-slots').innerHTML = !svc.running
      ? '<div class="conn-note">未运行，没有监听槽位</div>'
      : slots.length
        ? slots
            .map(
              (x) => `<div class="slot-row ${x.ok ? 'ok' : 'bad'}">
                <span class="dot"></span>
                <span class="addr">${h(x.addr || '')}</span>
                <span class="why">${x.ok ? '监听中' : h(x.hint || '绑定失败')}</span>
              </div>`
            )
            .join('')
        : '<div class="conn-note">这个服务没有监听槽位</div>';

    $('#svc-recent').innerHTML = recentRowsHtml(svc.id);
  }

  // ---------- 终端 ----------

  function termText(bytes) {
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []);
    if (state.term.hex)
      return Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join(' ');
    return new TextDecoder('utf-8', { fatal: false }).decode(u8);
  }

  function termLineNode(line) {
    const el = document.createElement('div');
    if (line.sys) {
      el.className = 'line sys';
      const body = document.createElement('span');
      body.className = 'body';
      body.textContent = '· ' + line.sys;
      el.appendChild(body);
      return el;
    }
    el.className = 'line ' + (line.d ? 'tx' : 'rx');
    if (state.term.ts) {
      const stamp = document.createElement('span');
      stamp.className = 'ts';
      stamp.textContent = fmtTime(line.ts || Date.now());
      el.appendChild(stamp);
    }
    const body = document.createElement('span');
    body.className = 'body';
    body.textContent = (line.d ? 'TX ' : 'RX ') + line.text;
    el.appendChild(body);
    return el;
  }

  /* 串口会话没有 IP 地址，标题栏改为显示「端口 · 波特率 帧格式」 */
  function termEndpoints(tab) {
    const sess = tab.sessionId ? state.sessions.find((s) => s.id === tab.sessionId) : null;
    if (!sess || sess.transport !== 'serial') return null;
    const parity = { 0: 'N', 1: 'O', 2: 'E' }[sess.parity] || 'N';
    const bits = sess.data_bits || 8;
    const stop = sess.stop_bits || 1;
    const port = sess.serial_port || '串口';
    const baud = sess.baud ? ` · ${sess.baud} ${bits}${parity}${stop}` : '';
    return { peer: port + baud, local: '本地串口' };
  }

  function paintTerminal(tab) {
    const ep = termEndpoints(tab);
    $('#term-proto').textContent = tab.proto ? (protoLabels[tab.proto] || tab.proto) : '-';
    $('#term-peer').textContent = ep ? ep.peer : tab.peer || '—';
    $('#term-local').textContent = ep ? ep.local : tab.local || '—';
    $('#term-state').textContent = tab.state || 'closed';
    $('#term-state').className = 'state ' + (tab.state === 'open' ? 'open' : 'closed');
    $('#term-rx').textContent = 'RX ' + fmtBytes(tab.rx);
    $('#term-tx').textContent = 'TX ' + fmtBytes(tab.tx);
    $('#term-mode-btn').textContent = tab.view === 'term' ? '日志视图' : '终端视图';
    const out = $('#term-out');
    out.innerHTML = '';
    tab.lines.forEach((l) => out.appendChild(termLineNode(l)));
    out.scrollTop = out.scrollHeight;
  }

  /* ---------- 快捷发送与全局预置 ---------- */

  const QUICK_EOL = { crlf: '\r\n', lf: '\n', cr: '\r', none: '' };

  function sendBytes(bytes) {
    const tab = activeTermTab();
    if (!tab || !tab.ws || tab.ws.readyState !== WebSocket.OPEN) {
      toast('尚未连接');
      return false;
    }
    tab.ws.send(bytes);
    tab.tx += bytes.length;
    if (state.activeTab === tab.id) $('#term-tx').textContent = 'TX ' + fmtBytes(tab.tx);
    return true;
  }

  function quickSend(text) {
    const value = text != null ? text : $('#quick-input').value;
    if (!value) return;
    const eol = QUICK_EOL[$('#quick-eol').value] || '';
    if (sendBytes(new TextEncoder().encode(value + eol)) && text == null) $('#quick-input').value = '';
  }

  function renderQuickPresets() {
    const box = $('#quick-presets');
    if (!state.presets.length) {
      box.innerHTML = '<span class="quick-empty">还没有预置命令：输入框里写一条，点「存为预置」</span>';
      return;
    }
    box.innerHTML = state.presets
      .map((c, i) => `<button class="quick-preset" data-idx="${i}" title="左键发送，右键删除">${h(c)}</button>`)
      .join('');
  }

  async function refreshPresets() {
    try {
      state.presets = await api('/api/presets');
    } catch (e) {
      state.presets = [];
    }
    renderQuickPresets();
  }

  async function savePresets(list) {
    await api('/api/presets', { method: 'PUT', body: JSON.stringify({ presets: list }) });
    state.presets = list;
    renderQuickPresets();
  }

  /* ---------- xterm 终端仿真 ---------- */

  const termInst = new Map(); /* tab.id -> { term, fit, slot } */

  function xtermAvailable() {
    return typeof Terminal !== 'undefined' && typeof FitAddon !== 'undefined';
  }

  function ensureXterm(tab) {
    let inst = termInst.get(tab.id);
    if (inst || !xtermAvailable()) return inst || null;
    const host = $('#term-xterm');
    const slot = document.createElement('div');
    slot.className = 'xterm-slot';
    slot.dataset.tab = String(tab.id);
    host.appendChild(slot);

    const term = new Terminal({
      convertEol: true,
      cursorBlink: true,
      fontSize: 13,
      fontFamily: 'Consolas, "Cascadia Mono", "Courier New", monospace',
      scrollback: 5000,
      theme: {
        background: '#0d1117',
        foreground: '#c9d1d9',
        cursor: '#58a6ff',
        selectionBackground: '#264f78',
      },
    });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open(slot);
    inst = { term, fit, slot };
    termInst.set(tab.id, inst);
    fit.fit();

    /* 键盘输入直接发到连接 */
    term.onData((data) => {
      const t = tabById(tab.id);
      if (!t || !t.ws || t.ws.readyState !== WebSocket.OPEN) return;
      const bytes = new TextEncoder().encode(data);
      t.ws.send(bytes);
      t.tx += bytes.length;
      if (state.activeTab === t.id) $('#term-tx').textContent = 'TX ' + fmtBytes(t.tx);
    });

    /* 回放已有输出 */
    for (const l of tab.lines) {
      if (l.sys) term.write(`\x1b[90m· ${l.sys}\x1b[0m\r\n`);
      else if (l.b) term.write(l.b);
    }
    return inst;
  }

  function disposeXterm(tab) {
    const inst = termInst.get(tab.id);
    if (!inst) return;
    try { inst.term.dispose(); } catch (e) {}
    if (inst.slot && inst.slot.parentNode) inst.slot.parentNode.removeChild(inst.slot);
    termInst.delete(tab.id);
  }

  /* 切换/应用终端视图：term = xterm，log = 逐行日志 */
  function applyTermView(tab) {
    const termMode = tab.view === 'term' && xtermAvailable();
    $('#term-xterm').classList.toggle('hidden', !termMode);
    $('#term-out').classList.toggle('hidden', termMode);
    $('#term-send').classList.toggle('hidden', termMode);
    $('#term-quick').classList.toggle('hidden', !termMode);
    $('#quick-presets').classList.toggle('hidden', !termMode);
    $('#term-mode-btn').textContent = tab.view === 'term' ? '日志视图' : '终端视图';

    termInst.forEach((inst, id) => {
      inst.slot.classList.toggle('hidden', !(termMode && id === tab.id));
    });

    if (termMode) {
      const inst = ensureXterm(tab);
      if (inst) {
        requestAnimationFrame(() => {
          try {
            inst.fit.fit();
            inst.term.focus();
          } catch (e) {}
        });
      }
    }
  }

  function termPush(tab, line) {
    tab.lines.push(line);
    while (tab.lines.length > 3000) tab.lines.shift();
    if (state.activeTab !== tab.id) return;
    const out = $('#term-out');
    const nearBottom = out.scrollTop + out.clientHeight >= out.scrollHeight - 40;
    out.appendChild(termLineNode(line));
    while (out.childElementCount > 3000) out.removeChild(out.firstChild);
    if (nearBottom) out.scrollTop = out.scrollHeight;
  }

  function termSys(tab, text) {
    termPush(tab, { sys: text });
    if (tab.view === 'term') {
      const inst = termInst.get(tab.id);
      if (inst) inst.term.write(`\x1b[90m· ${text}\x1b[0m\r\n`);
    }
  }

  function repaintTerminal() {
    const tab = activeTermTab();
    if (tab) paintTerminal(tab);
  }

  function ensureTermSocket(tab) {
    if (!tab || !tab.connId) return;
    if (tab.ws && (tab.ws.readyState === WebSocket.OPEN || tab.ws.readyState === WebSocket.CONNECTING))
      return;
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${location.host}/ws/conn/${tab.connId}?t=${encodeURIComponent(token)}`);
    ws.binaryType = 'arraybuffer';
    tab.ws = ws;

    ws.onopen = () => {
      if (tab.ws !== ws) return;
      tab.state = 'open';
      if (state.activeTab === tab.id) {
        $('#term-state').textContent = 'open';
        $('#term-state').className = 'state open';
      }
      termSys(tab, `已连接到 #${tab.connId}`);
    };
    ws.onclose = () => {
      if (tab.ws !== ws) return;
      tab.ws = null;
      tab.state = 'closed';
      if (state.activeTab === tab.id) {
        $('#term-state').textContent = 'closed';
        $('#term-state').className = 'state closed';
      }
      termSys(tab, '连接已关闭');
    };
    ws.onerror = () => {
      if (tab.ws === ws) termSys(tab, 'WebSocket 错误');
    };
    ws.onmessage = (ev) => {
      if (tab.ws !== ws) return;
      if (typeof ev.data === 'string') {
        try {
          const m = JSON.parse(ev.data);
          if (m.t === 'state') {
            tab.state = m.state;
            if (state.activeTab === tab.id) {
              $('#term-state').textContent = m.state;
              $('#term-state').className = 'state ' + (m.state === 'open' ? 'open' : 'closed');
            }
          }
        } catch (e) {}
        return;
      }
      const u8 = new Uint8Array(ev.data);
      if (u8.length < 9) return;
      const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
      const ts = Number(dv.getBigUint64(0, true));
      const dir = dv.getUint8(8);
      const payload = u8.slice(9);
      if (dir) tab.tx += payload.length;
      else tab.rx += payload.length;
      if (state.activeTab === tab.id) {
        $('#term-rx').textContent = 'RX ' + fmtBytes(tab.rx);
        $('#term-tx').textContent = 'TX ' + fmtBytes(tab.tx);
      }
      termPush(tab, { d: dir, text: termText(payload), ts, b: payload });
      /* 终端只渲染收到的数据；自己发出去的内容由对端回显，避免重复显示 */
      if (tab.view === 'term' && !dir) {
        const inst = termInst.get(tab.id);
        if (inst) inst.term.write(payload);
      }
    };
  }

  function connectTerminal(connId, sessionId) {
    const tab = openTab({ kind: 'term', connId, title: `终端 #${connId}` });
    if (sessionId) tab.sessionId = sessionId;
    if (!tab.proto) {
      api(`/api/conn/${connId}`)
        .then((c) => {
          tab.proto = c.proto;
          tab.peer = c.peer;
          tab.local = c.local;
          tab.rx = c.rx;
          tab.tx = c.tx;
          if (state.activeTab === tab.id) paintTerminal(tab);
        })
        .catch(() => {});
    }
    return tab;
  }

  /* 删除会话时把它的终端标签一起关掉（不论连接是否还活着） */
  function closeTabsForSession(sessionId) {
    if (!sessionId) return;
    state.tabs.filter((t) => t.kind === 'term' && t.sessionId === sessionId).forEach((t) => closeTab(t.id));
  }

  /* 当前终端对应的连接已经不存在（例如所属会话/服务被删）时收掉标签 */
  async function closeTerminalIfGone() {
    const tab = activeTermTab();
    if (!tab) return;
    try {
      await api(`/api/conn/${tab.connId}`);
    } catch (e) {
      closeTab(tab.id);
    }
  }

  function clearActiveTerminal() {
    const tab = activeTermTab();
    if (!tab) return;
    tab.lines = [];
    tab.rx = 0;
    tab.tx = 0;
    const inst = termInst.get(tab.id);
    if (inst) {
      try { inst.term.clear(); } catch (e) {}
    }
    paintTerminal(tab);
  }

  function sendInput() {
    const tab = activeTermTab();
    const ws = tab && tab.ws;
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      toast('尚未连接');
      return;
    }
    const input = $('#send-input');
    const raw = input.value;
    if (!raw) return;
    let bytes;
    if ($('#send-enc').value === 'hex') {
      const clean = raw.replace(/[^0-9a-fA-F]/g, '');
      if (clean.length % 2) { toast('HEX 长度必须是偶数'); return; }
      bytes = new Uint8Array(clean.length / 2);
      for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(clean.substr(i * 2, 2), 16);
    } else {
      const eol = { none: '', crlf: '\r\n', lf: '\n', cr: '\r' }[$('#send-eol').value] || '';
      bytes = new TextEncoder().encode(raw + eol);
    }
    ws.send(bytes);
    input.value = '';
  }

  // ---------- HTTP 调试 ----------

  function parseHeaderLines(text) {
    const out = [];
    for (const raw of text.split(/\r?\n/)) {
      const line = raw.trim();
      if (!line) continue;
      const i = line.indexOf(':');
      if (i <= 0) continue;
      out.push({ name: line.slice(0, i).trim(), value: line.slice(i + 1).trim() });
    }
    return out;
  }

  function b64ToBytes(b64) {
    const bin = atob(b64);
    const u = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i);
    return u;
  }

  function hexdump(bytes) {
    const max = 65536;
    const n = Math.min(bytes.length, max);
    const lines = [];
    for (let i = 0; i < n; i += 16) {
      const slice = bytes.subarray(i, i + 16);
      let hex = '';
      let ascii = '';
      for (let j = 0; j < 16; j++) {
        if (j < slice.length) {
          hex += slice[j].toString(16).padStart(2, '0') + ' ';
          ascii += slice[j] >= 32 && slice[j] < 127 ? String.fromCharCode(slice[j]) : '.';
        } else {
          hex += '   ';
          ascii += ' ';
        }
      }
      lines.push(i.toString(16).padStart(6, '0') + '  ' + hex + ' ' + ascii);
    }
    if (bytes.length > n) lines.push('...（共 ' + bytes.length + ' 字节，仅显示前 ' + n + '）');
    return lines.join('\n');
  }

  function openHttpPanel() {
    openTab({ kind: 'http', title: TOOL_TITLES.http });
    $('#http-url').focus();
  }

  function renderHttpBody() {
    const d = state.http.data;
    if (!d) return;
    const bytes = state.http.bytes || new Uint8Array();
    let out = '';
    if (state.http.view === 'hex') {
      out = bytes.length ? hexdump(bytes) : '（空响应体）';
    } else if (state.http.view === 'raw') {
      out =
        (d.response_head || '') +
        '\n' +
        (d.body_text !== undefined ? d.body_text : `[二进制 ${bytes.length} 字节]`);
    } else {
      const ct = (d.headers.find((x) => x.name.toLowerCase() === 'content-type') || {}).value || '';
      if (d.body_text !== undefined) {
        out = d.body_text;
      } else if (d.body_base64) {
        out = `[二进制响应 ${bytes.length} 字节]\nContent-Type: ${ct || '未知'}\n切换到 HEX 查看原始字节`;
      } else {
        out = new TextDecoder('utf-8', { fatal: false }).decode(bytes);
      }
      if (ct.includes('json') && out) {
        try {
          out = JSON.stringify(JSON.parse(out), null, 2);
        } catch (e) {}
      }
      if (!out) out = '（空响应体）';
    }
    if (out.length > 200000) out = out.slice(0, 200000) + '\n...（已截断显示）';
    $('#http-response').textContent = out;
  }

  function renderHttp(d) {
    state.http.data = d;
    state.http.view = 'text';
    const cls = d.status >= 200 && d.status < 300 ? 'ok' : d.status < 400 ? 'warn' : 'bad';
    const st = $('#http-status');
    st.textContent = `${d.status} ${d.reason}`;
    st.className = 'http-status ' + cls;
    $('#http-metrics').textContent =
      `连接 ${d.timings.connect_ms}ms · 首字节 ${d.timings.first_byte_ms}ms · 总 ${d.timings.total_ms}ms · ${d.body_len} B` +
      (d.truncated ? ' · 已截断' : '') +
      (d.redirects ? ` · 重定向 ${d.redirects} 次` : '');
    $('#http-resp-headers').innerHTML = d.headers
      .map((hd) => `<div class="row"><span class="k">${h(hd.name)}</span><span>${h(hd.value)}</span></div>`)
      .join('');
    $('#http-raw').textContent =
      (d.request_head || '') + '\n\n--- 响应头 ---\n' + (d.response_head || '');
    if (d.body_base64) state.http.bytes = b64ToBytes(d.body_base64);
    else if (d.body_text !== undefined) state.http.bytes = new TextEncoder().encode(d.body_text);
    else state.http.bytes = new Uint8Array();
    renderHttpBody();
  }

  function renderHttpError(e) {
    const d = (e && e.data) || {};
    const st = $('#http-status');
    st.textContent = '请求失败';
    st.className = 'http-status bad';
    $('#http-metrics').textContent = '';
    $('#http-resp-headers').innerHTML = '';
    $('#http-raw').textContent = '';
    $('#http-response').textContent =
      (d.message || (e && e.message) || '请求失败') + (d.detail ? '\n' + d.detail : '');
  }

  async function sendHttp() {
    const url = $('#http-url').value.trim();
    if (!url) {
      toast('URL 不能为空');
      return;
    }
    const body = $('#http-body').value;
    const enc = $('#http-body-enc').value;
    if (enc === 'hex' && body.replace(/\s+/g, '').length % 2 !== 0) {
      toast('HEX 长度必须是偶数');
      return;
    }
    const payload = {
      method: $('#http-method').value,
      url,
      headers: parseHeaderLines($('#http-headers').value),
      body,
      body_encoding: enc,
      timeout_ms: Number($('#http-timeout').value || 15000),
      follow_redirects: $('#http-follow').checked,
    };
    const st = $('#http-status');
    st.textContent = '请求中…';
    st.className = 'muted';
    try {
      const d = await api('/api/http/send', { method: 'POST', body: JSON.stringify(payload) });
      if (d.ok === false) renderHttpError({ data: d });
      else renderHttp(d);
    } catch (e) {
      renderHttpError(e);
    }
  }

  // ---------- FTP 文件 ----------

  function ftpJoin(base, name) {
    if (!base || base === '/') return '/' + name;
    return base.replace(/\/$/, '') + '/' + name;
  }

  function fmtFtpTime(v) {
    const s = String(v || '');
    if (/^\d{14}$/.test(s)) {
      return `${s.slice(0, 4)}-${s.slice(4, 6)}-${s.slice(6, 8)} ` +
        `${s.slice(8, 10)}:${s.slice(10, 12)}:${s.slice(12, 14)}`;
    }
    return s;
  }

  function bytesToB64(u8) {
    let s = '';
    for (let i = 0; i < u8.length; i += 0x8000)
      s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  }

  function openFtpPanel() {
    openTab({ kind: 'ftp', title: TOOL_TITLES.ftp });
  }

  function renderFtp() {
    const list = $('#ftp-list');
    $('#ftp-pwd').textContent = state.ftp.pwd;
    if (!state.ftp.id) {
      list.innerHTML = '<div class="muted" style="padding:14px">连接一个 FTP 服务器后显示目录</div>';
      return;
    }
    const rows = state.ftp.entries
      .filter((e) => e.name !== '.' && e.name !== '..')
      .map((e) => {
        const size = e.is_dir ? '-' : e.size;
        return `<div class="ftp-row">
          <span class="name ${e.is_dir ? 'dir' : ''}" data-name="${h(e.name)}" data-dir="${e.is_dir ? 1 : 0}">
            <span class="dir-icon">${e.is_dir ? '📁' : '📄'}</span>${h(e.name)}
          </span>
          <span class="size">${size}</span>
          <span class="muted">${h(fmtFtpTime(e.modify))}</span>
          <span class="actions">
            ${e.is_dir ? '<button class="mini-btn" data-act="enter">打开</button>' : '<button class="mini-btn" data-act="get">下载</button>'}
            <button class="mini-btn danger" data-act="del">删除</button>
          </span>
        </div>`;
      })
      .join('');
    list.innerHTML =
      '<div class="ftp-row head"><span>名称</span><span class="size">大小</span><span>修改时间</span><span class="actions">操作</span></div>' +
      (rows || '<div class="muted" style="padding:14px">空目录</div>');
  }

  async function ftpList(path) {
    if (!state.ftp.id) return;
    try {
      const entries = await api(`/api/ftp/${state.ftp.id}/list?path=${encodeURIComponent(path)}`);
      state.ftp.entries = entries;
      state.ftp.pwd = path || '/';
      renderFtp();
      $('#ftp-state').textContent = '已连接';
    } catch (e) {
      $('#ftp-state').textContent = '列表失败';
      toast(errText(e));
    }
  }

  async function ftpConnect() {
    const host = $('#ftp-host').value.trim();
    if (!host) {
      toast('主机不能为空');
      return;
    }
    $('#ftp-state').textContent = '连接中…';
    try {
      if (state.ftp.id) {
        await api(`/api/ftp/${state.ftp.id}`, { method: 'DELETE' }).catch(() => {});
        state.ftp.id = null;
        state.ftp.entries = [];
      }
      const s = await api('/api/ftp', {
        method: 'POST',
        body: JSON.stringify({
          host,
          port: Number($('#ftp-port').value || 21),
          user: $('#ftp-user').value.trim() || 'anonymous',
          password: $('#ftp-pass').value,
        }),
      });
      state.ftp.id = s.id;
      await ftpList(s.pwd || '/');
    } catch (e) {
      $('#ftp-state').textContent = '连接失败';
      toast(errText(e));
    }
  }

  async function ftpDownload(name) {
    try {
      const r = await api(`/api/ftp/${state.ftp.id}/download`, {
        method: 'POST',
        body: JSON.stringify({ remote: ftpJoin(state.ftp.pwd, name) }),
      });
      const res = await fetch('/api/fs/download?path=' + encodeURIComponent(r.local_path), {
        headers: { 'X-Auth-Token': token },
      });
      const blob = await res.blob();
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = r.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      toast(`已下载 ${r.name}（${r.size} 字节）`);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function ftpDelete(name, isDir) {
    if (!confirm(`删除 ${name}？`)) return;
    try {
      await api(`/api/ftp/${state.ftp.id}/${isDir ? 'rmdir' : 'delete'}`, {
        method: 'POST',
        body: JSON.stringify({ path: ftpJoin(state.ftp.pwd, name) }),
      });
      await ftpList(state.ftp.pwd);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function ftpMkdir() {
    const name = prompt('新目录名称');
    if (!name) return;
    try {
      await api(`/api/ftp/${state.ftp.id}/mkdir`, {
        method: 'POST',
        body: JSON.stringify({ path: ftpJoin(state.ftp.pwd, name) }),
      });
      await ftpList(state.ftp.pwd);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function ftpUploadFile(file) {
    if (!state.ftp.id) {
      toast('请先连接 FTP');
      return;
    }
    if (file.size > 3 * 1024 * 1024) {
      toast('当前版本上传限制 3MB');
      return;
    }
    const buf = new Uint8Array(await file.arrayBuffer());
    try {
      await api(`/api/ftp/${state.ftp.id}/upload`, {
        method: 'POST',
        body: JSON.stringify({
          remote: ftpJoin(state.ftp.pwd, file.name),
          data_base64: bytesToB64(buf),
        }),
      });
      toast(`已上传 ${file.name}`);
      await ftpList(state.ftp.pwd);
    } catch (e) {
      toast(errText(e));
    }
  }

  function openSmbPanel() {
    openTab({ kind: 'smb', title: TOOL_TITLES.smb });
  }

  function smbDate(ts) {
    if (!ts) return '';
    try { return new Date(ts * 1000).toLocaleString(); } catch (e) { return ''; }
  }

  function renderSmb() {
    const list = $('#smb-list');
    $('#smb-pwd').textContent = state.smb.pwd;
    if (!state.smb.id) {
      list.innerHTML = '<div class="muted" style="padding:14px">连接一个 SMB 共享后显示目录</div>';
      return;
    }
    const rows = state.smb.entries.map((e) => `<div class="ftp-row">
        <span class="name ${e.is_dir ? 'dir' : ''}" data-name="${h(e.name)}" data-dir="${e.is_dir ? 1 : 0}">
          <span class="dir-icon">${e.is_dir ? '📁' : '📄'}</span>${h(e.name)}
        </span>
        <span class="size">${e.is_dir ? '-' : e.size}</span>
        <span class="muted">${h(smbDate(e.mtime))}</span>
        <span class="actions">
          ${e.is_dir ? '<button class="mini-btn" data-act="enter">打开</button>' : '<button class="mini-btn" data-act="get">下载</button>'}
          <button class="mini-btn danger" data-act="del">删除</button>
        </span>
      </div>`).join('');
    list.innerHTML =
      '<div class="ftp-row head"><span>名称</span><span class="size">大小</span><span>修改时间</span><span class="actions">操作</span></div>' +
      (rows || '<div class="muted" style="padding:14px">空目录</div>');
  }

  async function smbList(path) {
    if (!state.smb.id) return;
    try {
      const entries = await api(`/api/smb/${state.smb.id}/list?path=${encodeURIComponent(path || '/')}`);
      state.smb.entries = entries;
      state.smb.pwd = path || '/';
      renderSmb();
      $('#smb-state').textContent = '已连接';
    } catch (e) {
      $('#smb-state').textContent = '列表失败';
      toast(errText(e));
    }
  }

  async function smbConnect() {
    const host = $('#smb-host').value.trim();
    const share = $('#smb-share').value.trim();
    if (!host || !share) {
      toast('服务器和共享名不能为空');
      return;
    }
    $('#smb-state').textContent = '连接中…';
    try {
      if (state.smb.id) {
        await api(`/api/smb/${state.smb.id}`, { method: 'DELETE' }).catch(() => {});
        state.smb.id = null;
      }
      const s = await api('/api/smb', {
        method: 'POST',
        body: JSON.stringify({
          host,
          share,
          user: $('#smb-user').value.trim(),
          password: $('#smb-pass').value,
          domain: $('#smb-domain').value.trim(),
        }),
      });
      state.smb.id = s.id;
      await smbList('/');
    } catch (e) {
      $('#smb-state').textContent = '连接失败';
      toast(errText(e));
    }
  }

  async function smbDownload(name) {
    try {
      const r = await api(`/api/smb/${state.smb.id}/download`, {
        method: 'POST',
        body: JSON.stringify({ remote: ftpJoin(state.smb.pwd, name) }),
      });
      const res = await fetch('/api/fs/download?path=' + encodeURIComponent(r.local_path), {
        headers: { 'X-Auth-Token': token },
      });
      const blob = await res.blob();
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = r.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      toast(`已下载 ${r.name}（${r.size} 字节）`);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function smbDelete(name, isDir) {
    if (!confirm(`删除 ${name}？`)) return;
    try {
      await api(`/api/smb/${state.smb.id}/${isDir ? 'rmdir' : 'delete'}`, {
        method: 'POST',
        body: JSON.stringify({ path: ftpJoin(state.smb.pwd, name) }),
      });
      await smbList(state.smb.pwd);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function smbMkdir() {
    const name = prompt('新目录名称');
    if (!name) return;
    try {
      await api(`/api/smb/${state.smb.id}/mkdir`, {
        method: 'POST',
        body: JSON.stringify({ path: ftpJoin(state.smb.pwd, name) }),
      });
      await smbList(state.smb.pwd);
    } catch (e) {
      toast(errText(e));
    }
  }

  async function smbUploadFile(file) {
    if (!state.smb.id) {
      toast('请先连接 SMB');
      return;
    }
    if (file.size > 3 * 1024 * 1024) {
      toast('当前版本上传限制 3MB');
      return;
    }
    const buf = new Uint8Array(await file.arrayBuffer());
    try {
      $('#smb-state').textContent = '上传中…';
      await api(`/api/smb/${state.smb.id}/upload`, {
        method: 'POST',
        body: JSON.stringify({
          remote: ftpJoin(state.smb.pwd, file.name),
          data_base64: bytesToB64(buf),
        }),
      });
      await smbList(state.smb.pwd);
      toast(`已上传 ${file.name}`);
    } catch (e) {
      $('#smb-state').textContent = '上传失败';
      toast(errText(e));
    }
  }

  function openTftpPanel() {
    openTab({ kind: 'tftp', title: TOOL_TITLES.tftp });
  }

  async function tftpDownload() {
    const remote = $('#tftp-remote').value.trim();
    if (!remote) {
      toast('请填写远端文件名');
      return;
    }
    const st = $('#tftp-state');
    st.textContent = '下载中…';
    try {
      const r = await api('/api/tftp/download', {
        method: 'POST',
        body: JSON.stringify({
          host: $('#tftp-host').value.trim(),
          port: Number($('#tftp-port').value || 69),
          remote,
          blksize: 1468,
        }),
      });
      const res = await fetch('/api/fs/download?path=' + encodeURIComponent(r.local_path), {
        headers: { 'X-Auth-Token': token },
      });
      const blob = await res.blob();
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = r.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
      st.textContent = `已下载 ${r.size} 字节`;
    } catch (e) {
      st.textContent = '下载失败';
      toast(errText(e));
    }
  }

  async function tftpUpload() {
    const f = $('#tftp-file').files[0];
    if (!f) {
      toast('请选择文件');
      return;
    }
    if (f.size > 3 * 1024 * 1024) {
      toast('TFTP 上传限制 3MB');
      return;
    }
    const st = $('#tftp-state');
    st.textContent = '上传中…';
    const remote = $('#tftp-up-remote').value.trim() || f.name;
    const buf = new Uint8Array(await f.arrayBuffer());
    try {
      const r = await api('/api/tftp/upload', {
        method: 'POST',
        body: JSON.stringify({
          host: $('#tftp-host').value.trim(),
          port: Number($('#tftp-port').value || 69),
          remote,
          blksize: 1468,
          data_base64: bytesToB64(buf),
        }),
      });
      st.textContent = `已上传 ${r.size} 字节`;
    } catch (e) {
      st.textContent = '上传失败';
      toast(errText(e));
    }
  }

  // ---------- 全局事件 ----------

  function connectEvents() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${location.host}/ws/events?t=${encodeURIComponent(token)}`);
    state.events = ws;
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') {
        try {
          const m = JSON.parse(e.data);
          if (m && m.t) applyServiceEvent(m);
        } catch (err) {}
      }
      clearTimeout(state.eventsTimer);
      state.eventsTimer = setTimeout(refreshServices, 300);
    };
    ws.onclose = () => {
      setTimeout(connectEvents, 2000);
    };
  }

  // ---------- 新建服务对话框 ----------

  function fillIfaces() {
    const sel = $('#f-iface');
    sel.innerHTML = state.ifaces
      .map((i) => {
        const addrs = i.addrs.map((a) => a.addr).join(', ');
        return `<option value="${h(i.id)}">${h(i.name)} — ${h(addrs || '无地址')}</option>`;
      })
      .join('');
  }

  function updateFormVisibility() {
    const kind = $('#f-kind').value;
    $('#wrap-iface').classList.toggle('hidden', kind !== 'iface');
    $('#wrap-addr').classList.toggle('hidden', kind !== 'addr');
    const fam = $('#f-family');
    if (kind === 'addr') {
      for (const opt of fam.options) {
        if (opt.value === 'dual') opt.disabled = true;
      }
      if (fam.value === 'dual') fam.value = 'v6';
    } else {
      for (const opt of fam.options) opt.disabled = false;
    }
  }

  function formBind() {
    const kind = $('#f-kind').value;
    const b = {
      kind,
      family: $('#f-family').value,
      port: Number($('#f-port').value || 0),
    };
    if (kind === 'iface') b.iface_id = $('#f-iface').value;
    if (kind === 'addr') b.addr = $('#f-addr').value.trim();
    return b;
  }

  function renderSlots(container, slots) {
    container.innerHTML = slots
      .map(
        (s) =>
          `<div class="row"><span class="${s.ok ? 'ok' : 'bad'}">${s.ok ? '成功' : '失败'}</span>
           <span>${h(s.addr)}</span>
           <span class="muted">${s.ok ? '' : h(s.hint || ('code ' + s.code))}</span></div>`
      )
      .join('');
  }

  async function testBind() {
    const box = $('#bind-result');
    box.classList.remove('hidden');
    box.textContent = '测试中…';
    try {
      const r = await api('/api/net/test-bind', {
        method: 'POST',
        body: JSON.stringify(formBind()),
      });
      renderSlots(box, r.slots || []);
      if (!r.ok) box.insertAdjacentHTML('afterbegin', `<div class="row bad">${h(r.message || '绑定失败')}</div>`);
    } catch (e) {
      box.innerHTML = `<div class="row bad">${h(errText(e))}</div>`;
    }
  }

  /* 在协议基准端口附近随机取一个没被占用的端口 */
  function suggestPort(proto) {
    const base = SERVICE_PORTS[proto] || 9000;
    const used = new Set(state.services.map((s) => s.bind && s.bind.port));
    for (let i = 0; i < 200; i++) {
      const p = base + Math.floor(Math.random() * PORT_SPREAD);
      if (p > 1024 && p < 65536 && !used.has(p)) return p;
    }
    return base;
  }

  function openServiceDialog(s) {
    state.editService = s ? s.id : null;
    $('#dlg-service').querySelector('h2').textContent = s ? '编辑监听服务' : '新建监听服务';
    $('#btn-create').textContent = s ? '保存修改' : '创建并启动';
    const proto = s ? s.proto : 'raw-tcp';
    $('#f-proto').value = proto;
    $('#f-name').value = s ? s.name : (SERVICE_DEFAULTS[proto] || '调试服务');
    $('#f-port').value = s ? s.bind.port : suggestPort(proto);
    state.portTouched = false;
    $('#f-kind').value = s ? s.bind.kind : 'iface';
    $('#f-family').value = s ? s.bind.family : 'dual';
    $('#f-addr').value = s && s.bind.kind === 'addr' ? (s.bind.addr || '') : '';
    if (s && s.bind.kind === 'iface' && s.bind.iface_id)
      $('#f-iface').value = s.bind.iface_id;
    $('#f-echo').checked = s ? !!s.echo : false;
    $('#f-ftp-root').value = s && s.options ? (s.options.root || '') : '';
    $('#f-ftp-anon').checked = s && s.options ? s.options.anonymous !== false : true;
    $('#f-ftp-ro').checked = !!(s && s.options && s.options.readonly);
    $('#ftp-options').classList.toggle('hidden',
      ['ftp', 'tftp', 'http'].indexOf(proto) < 0);
    $('#bind-result').classList.add('hidden');
    $('#bind-result').innerHTML = '';
    updateFormVisibility();
    syncServiceTiles();
    $('#dlg-service').showModal();
    $('#f-name').focus();
    $('#f-name').select();
  }

  async function createService() {
    const body = {
      name: $('#f-name').value || 'TCP 服务',
      proto: $('#f-proto').value,
      bind: formBind(),
      echo: $('#f-echo').checked,
    };
    if (body.proto === 'ftp' || body.proto === 'tftp' || body.proto === 'http') {
      body.options = {
        root: $('#f-ftp-root').value.trim(),
        readonly: $('#f-ftp-ro').checked,
        anonymous: $('#f-ftp-anon').checked,
      };
    }
    if (state.editService) {
      const id = state.editService;
      try {
        const updated = await api(`/api/services/${id}`, {
          method: 'PUT',
          body: JSON.stringify(body),
        });
        $('#dlg-service').close();
        state.editService = null;
        await refreshServices();
        showConns(id);
        toast(updated.running ? '已保存并重启' : '已保存');
      } catch (e) {
        const box = $('#bind-result');
        box.classList.remove('hidden');
        box.innerHTML = `<div class="row bad">${h(errText(e))}</div>`;
        toast(errText(e));
        await refreshServices();
      }
      return;
    }
    let created = null;
    try {
      created = await api('/api/services', { method: 'POST', body: JSON.stringify(body) });
      const started = await api(`/api/services/${created.id}/start`, { method: 'POST' });
      if (started.degraded) toast('服务已启动，但部分地址绑定失败');
      $('#dlg-service').close();
      await refreshServices();
      showConns(created.id);
    } catch (e) {
      const box = $('#bind-result');
      box.classList.remove('hidden');
      box.innerHTML = `<div class="row bad">${h(errText(e))}</div>` +
        ((e.data && e.data.slots) ? '' : '');
      if (e.data && e.data.slots) renderSlots(box, e.data.slots);
      toast(errText(e));
      if (created) refreshServices();
    }
  }

  // ---------- 事件绑定 ----------

  const sidebar = $('.sidebar');
  const sidebarBackdrop = $('#sidebar-backdrop');

  function closeSidebar() {
    if (sidebar) sidebar.classList.remove('open');
    if (sidebarBackdrop) sidebarBackdrop.classList.remove('show');
  }

  function toggleSidebar() {
    if (!sidebar) return;
    const open = !sidebar.classList.contains('open');
    sidebar.classList.toggle('open', open);
    if (sidebarBackdrop) sidebarBackdrop.classList.toggle('show', open);
  }

  $('#btn-sidebar').addEventListener('click', toggleSidebar);
  sidebarBackdrop.addEventListener('click', closeSidebar);
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeSidebar();
  });
  document.addEventListener('click', (e) => {
    if (e.target.closest('.tool-btn') || e.target.closest('#btn-new') ||
        e.target.closest('#btn-new-sess') || e.target.closest('#service-list .svc') ||
        e.target.closest('#session-list .sess')) {
      closeSidebar();
    }
  });

  $('#btn-new').addEventListener('click', () => {
    openServiceDialog(null);
  });
  $('#btn-cancel').addEventListener('click', () => $('#dlg-service').close());
  $('#dlg-service').addEventListener('close', () => {
    state.editService = null;
    $('#btn-create').textContent = '创建并启动';
    $('#dlg-service').querySelector('h2').textContent = '新建监听服务';
  });
  $('#btn-test').addEventListener('click', testBind);
  $('#btn-create').addEventListener('click', createService);
  $('#btn-pick-root').addEventListener('click', async () => {
    const btn = $('#btn-pick-root');
    btn.disabled = true;
    btn.textContent = '选择中…';
    try {
      const r = await api('/api/system/pick-folder', { method: 'POST', body: '{}' });
      if (r.ok && r.path)
        $('#f-ftp-root').value = r.path;
    } catch (e) {
      toast(errText(e));
    } finally {
      btn.disabled = false;
      btn.textContent = '选择目录…';
    }
  });
  $('#f-kind').addEventListener('change', updateFormVisibility);
  $('#f-port').addEventListener('input', () => {
    state.portTouched = true;
  });
  $('#f-proto').addEventListener('change', () => {
    syncServiceTiles();
    const proto = $('#f-proto').value;
    const name = $('#f-name');
    if (!name.value || Object.values(SERVICE_DEFAULTS).includes(name.value))
      name.value = SERVICE_DEFAULTS[proto] || '调试服务';
    if (!state.portTouched) $('#f-port').value = suggestPort(proto);
    $('#ftp-options').classList.toggle('hidden',
      ['ftp', 'tftp', 'http'].indexOf(proto) < 0);
  });
  $('#btn-tftp').addEventListener('click', openTftpPanel);
  $('#tftp-get').addEventListener('click', tftpDownload);
  $('#tftp-put').addEventListener('click', tftpUpload);
  $('#tftp-file').addEventListener('change', (e) => {
    const f = e.target.files[0];
    $('#tftp-file-name').textContent = f ? f.name : '未选择文件';
  });

  $('#btn-http').addEventListener('click', openHttpPanel);
  $('#http-send').addEventListener('click', sendHttp);
  $('#http-url').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendHttp();
  });
  document.querySelector('.resp-tabs').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-hview]');
    if (!b) return;
    state.http.view = b.dataset.hview;
    renderHttpBody();
  });

  $('#btn-ftp').addEventListener('click', openFtpPanel);
  $('#ftp-connect').addEventListener('click', ftpConnect);
  $('#ftp-refresh').addEventListener('click', () => ftpList(state.ftp.pwd));
  $('#ftp-up').addEventListener('click', () => {
    const p = state.ftp.pwd || '/';
    const i = p.lastIndexOf('/');
    ftpList(i <= 0 ? '/' : p.slice(0, i));
  });
  $('#ftp-mkdir').addEventListener('click', ftpMkdir);
  $('#ftp-upload').addEventListener('change', (e) => {
    const f = e.target.files[0];
    if (f) ftpUploadFile(f);
    e.target.value = '';
  });
  $('#ftp-list').addEventListener('click', (ev) => {
    const row = ev.target.closest('.ftp-row');
    if (!row || row.classList.contains('head')) return;
    const nameEl = row.querySelector('.name');
    if (!nameEl) return;
    const name = nameEl.dataset.name;
    const isDir = nameEl.dataset.dir === '1';
    const btn = ev.target.closest('button[data-act]');
    if (btn) {
      const act = btn.dataset.act;
      if (act === 'enter') ftpList(ftpJoin(state.ftp.pwd, name));
      else if (act === 'get') ftpDownload(name);
      else if (act === 'del') ftpDelete(name, isDir);
      return;
    }
    if (ev.target.closest('.name') && isDir) ftpList(ftpJoin(state.ftp.pwd, name));
  });

  $('#btn-smb').addEventListener('click', openSmbPanel);
  $('#smb-connect').addEventListener('click', smbConnect);
  $('#smb-refresh').addEventListener('click', () => smbList(state.smb.pwd));
  $('#smb-up').addEventListener('click', () => {
    const p = state.smb.pwd || '/';
    const i = p.lastIndexOf('/');
    smbList(i <= 0 ? '/' : p.slice(0, i));
  });
  $('#smb-mkdir').addEventListener('click', smbMkdir);
  $('#smb-upload').addEventListener('change', (e) => {
    const f = e.target.files[0];
    if (f) smbUploadFile(f);
    e.target.value = '';
  });
  $('#smb-list').addEventListener('click', (ev) => {
    const row = ev.target.closest('.ftp-row');
    if (!row || row.classList.contains('head')) return;
    const nameEl = row.querySelector('.name');
    if (!nameEl) return;
    const name = nameEl.dataset.name;
    const isDir = nameEl.dataset.dir === '1';
    const btn = ev.target.closest('button[data-act]');
    if (btn) {
      const act = btn.dataset.act;
      if (act === 'enter') smbList(ftpJoin(state.smb.pwd, name));
      else if (act === 'get') smbDownload(name);
      else if (act === 'del') smbDelete(name, isDir);
      return;
    }
    if (ev.target.closest('.name') && isDir) smbList(ftpJoin(state.smb.pwd, name));
  });

  $('#btn-new-sess').addEventListener('click', () => {
    fillSessionIfaces();
    fillSerialPorts();
    updateSessionSource();
    syncProtoTransport();
    $('#s-host').value = '';
    $('#s-family').disabled = false;
    $('#s-family').value = 'dual';
    syncSessionFamily();
    $('#dlg-session').showModal();
    $('#s-host').focus();
  });
  $('#s-host').addEventListener('input', syncSessionFamily);

  $('#f-proto-tiles').addEventListener('click', (ev) => {
    const b = ev.target.closest('.proto-tile');
    if (!b) return;
    $('#f-proto').value = b.dataset.proto;
    $('#f-proto').dispatchEvent(new Event('change', { bubbles: true }));
  });

  $('#s-mode-tiles').addEventListener('click', (ev) => {
    const b = ev.target.closest('.proto-tile');
    if (!b) return;
    const m = b.dataset.mode;
    const t = $('#s-transport');
    const pr = $('#s-proto');
    if (m === 'ssh') t.value = 'ssh';
    else if (m === 'serial') t.value = 'serial';
    else {
      t.value = m === 'udp' ? 'udp' : 'tcp';
      pr.value = m === 'telnet' ? 'telnet' : 'raw';
    }
    pr.dispatchEvent(new Event('change', { bubbles: true }));
    t.dispatchEvent(new Event('change', { bubbles: true }));
  });
  $('#s-cancel').addEventListener('click', () => $('#dlg-session').close());
  $('#s-create').addEventListener('click', createSession);
  $('#s-source').addEventListener('change', updateSessionSource);
  $('#s-transport').addEventListener('change', syncProtoTransport);
  $('#s-proto').addEventListener('change', syncProtoTransport);
  $('#s-transport').addEventListener('change', () => {
    if ($('#s-transport').value === 'ssh' && $('#s-port').value === '80')
      $('#s-port').value = '22';
  });
  $('#s-proto').addEventListener('change', () => {
    if ($('#s-proto').value === 'telnet' && $('#s-port').value === '80')
      $('#s-port').value = '23';
  });
  $('#s-refresh-ports').addEventListener('click', fillSerialPorts);

  $('#session-list').addEventListener('click', async (ev) => {
    const btn = ev.target.closest('button[data-sact]');
    const card = ev.target.closest('.sess');
    const id = Number((btn && btn.dataset.id) || (card && card.dataset.sess));
    if (!id) return;
    const sess = state.sessions.find((x) => x.id === id);
    if (btn && btn.dataset.sact === 'disconnect') {
      try {
        await api(`/api/sessions/${id}/disconnect`, { method: 'POST' });
        /* 不关闭终端标签：保留此前的输出，标签会自己变成「已断开」 */
        await refreshSessions();
        toast('已断开，输出保留');
      } catch (e) {
        toast(errText(e));
      }
      return;
    }
    if (btn && btn.dataset.sact === 'del') {
      if (!confirm('删除这个会话？')) return;
      try {
        const connId = sess && sess.conn_id;
        await api(`/api/sessions/${id}`, { method: 'DELETE' });
        closeTabsForConn(connId);
        closeTabsForSession(id);
        await refreshSessions();
      } catch (e) {
        toast(errText(e));
      }
      return;
    }
    openSession(id);
  });

  $('#service-list').addEventListener('click', async (ev) => {
    const btn = ev.target.closest('button[data-act]');
    const card = ev.target.closest('.svc');
    const id = Number((btn && btn.dataset.id) || (card && card.dataset.id));
    if (!id) return;
    if (!btn) {
      openServiceDetail(id);
      showConns(id);
      return;
    }
    const act = btn.dataset.act;
    try {
      if (act === 'start') await api(`/api/services/${id}/start`, { method: 'POST' });
      else if (act === 'stop') await api(`/api/services/${id}/stop`, { method: 'POST' });
      else if (act === 'del') {
        if (!confirm('删除这个服务？')) return;
        await api(`/api/services/${id}`, { method: 'DELETE' });
        if (state.currentService === id) state.currentService = null;
      } else if (act === 'conns') {
        openServiceLog(id);
        showConns(id);
        return;
      } else if (act === 'edit') {
        const s = state.services.find((x) => x.id === id);
        if (s) openServiceDialog(s);
        return;
      }
      await refreshServices();
      await closeTerminalIfGone();
    } catch (e) {
      toast(errText(e));
    }
  });

  $('#conn-list').addEventListener('click', (ev) => {
    const el = ev.target.closest('.conn');
    if (!el) return;
    connectTerminal(Number(el.dataset.conn));
    document.querySelectorAll('.conn').forEach((x) => x.classList.toggle('active', x === el));
  });

  $('#btn-clear').addEventListener('click', clearActiveTerminal);
  $('#term-mode-btn').addEventListener('click', () => {
    const tab = activeTermTab();
    if (!tab) return;
    tab.view = tab.view === 'term' ? 'log' : 'term';
    paintTerminal(tab);
    applyTermView(tab);
  });

  /* 快捷发送栏 */
  $('#quick-send').addEventListener('click', () => quickSend());
  $('#quick-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      quickSend();
    }
  });
  $('#quick-save').addEventListener('click', async () => {
    const value = $('#quick-input').value.trim();
    if (!value) {
      toast('输入框是空的');
      return;
    }
    if (state.presets.includes(value)) {
      toast('这条已经是预置了');
      return;
    }
    if (state.presets.length >= 24) {
      toast('预置最多 24 条');
      return;
    }
    try {
      await savePresets(state.presets.concat([value]));
      toast('已存为预置');
    } catch (e) {
      toast(errText(e));
    }
  });
  $('#quick-presets').addEventListener('click', (ev) => {
    const btn = ev.target.closest('.quick-preset');
    if (!btn) return;
    quickSend(state.presets[Number(btn.dataset.idx)]);
  });
  $('#quick-presets').addEventListener('contextmenu', async (ev) => {
    const btn = ev.target.closest('.quick-preset');
    if (!btn) return;
    ev.preventDefault();
    const idx = Number(btn.dataset.idx);
    const cmd = state.presets[idx];
    if (!confirm(`删除预置「${cmd}」？`)) return;
    try {
      await savePresets(state.presets.filter((_, i) => i !== idx));
    } catch (e) {
      toast(errText(e));
    }
  });
  window.addEventListener('resize', () => {
    const tab = activeTermTab();
    if (!tab || tab.view !== 'term') return;
    const inst = termInst.get(tab.id);
    if (!inst) return;
    try { inst.fit.fit(); } catch (e) {}
  });
  $('#send-btn').addEventListener('click', sendInput);
  $('#send-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      sendInput();
    }
  });
  $('#opt-hex').addEventListener('change', (e) => {
    state.term.hex = e.target.checked;
    repaintTerminal();
    const tab = activeTermTab();
    if (tab) termSys(tab, state.term.hex ? 'HEX 视图' : '文本视图');
  });
  $('#opt-ts').addEventListener('change', (e) => {
    state.term.ts = e.target.checked;
    repaintTerminal();
  });

  $('#log-clear').addEventListener('click', () => {
    const tab = activeTab();
    if (!tab || tab.kind !== 'log') return;
    tab.rows = [];
    paintLog(tab);
  });
  $('#log-follow').addEventListener('change', () => {
    const tab = activeTab();
    if (tab && tab.kind === 'log') paintLog(tab);
  });

  /* ---- 服务详情页操作 ---- */
  $('#svc-toggle').addEventListener('click', async () => {
    const tab = activeTab();
    if (!tab || tab.kind !== 'svc') return;
    const svc = state.services.find((s) => s.id === tab.svcId);
    if (!svc) return;
    try {
      await api(`/api/services/${svc.id}/${svc.running ? 'stop' : 'start'}`, { method: 'POST' });
      await refreshServices();
      paintServiceDetail(tab);
    } catch (e) {
      toast(errText(e));
    }
  });
  $('#svc-edit').addEventListener('click', () => {
    const tab = activeTab();
    const svc = tab && tab.kind === 'svc' ? state.services.find((s) => s.id === tab.svcId) : null;
    if (svc) openServiceDialog(svc);
  });
  $('#svc-del').addEventListener('click', async () => {
    const tab = activeTab();
    if (!tab || tab.kind !== 'svc') return;
    const svc = state.services.find((s) => s.id === tab.svcId);
    if (!svc || !confirm(`删除服务「${svc.name}」？`)) return;
    try {
      await api(`/api/services/${svc.id}`, { method: 'DELETE' });
      closeTab(tab.id);
      await refreshServices();
    } catch (e) {
      toast(errText(e));
    }
  });
  $('#svc-log-open').addEventListener('click', () => {
    const tab = activeTab();
    if (tab && tab.kind === 'svc') openServiceLog(tab.svcId);
  });

  /* 运行时长每秒走一格 */
  setInterval(() => {
    const tab = activeTab();
    if (tab && tab.kind === 'svc') paintServiceDetail(tab);
  }, 1000);

  $('#tabs').addEventListener('click', (ev) => {
    const closeBtn = ev.target.closest('.tab-close');
    if (closeBtn) {
      ev.stopPropagation();
      closeTab(Number(closeBtn.dataset.close));
      return;
    }
    const tab = ev.target.closest('.tab');
    if (tab) activateTab(Number(tab.dataset.tab));
  });

  // ---------- 启动 ----------

  (async function init() {
    try {
      state.meta = await api('/api/meta');
      $('#meta-line').textContent = `v${state.meta.version} · ${state.meta.os} · :${state.meta.port}`;
      state.ifaces = await api('/api/net/interfaces');
      fillIfaces();
      updateFormVisibility();
      await refreshServices();
      await refreshSessions();
      await refreshPresets();
      $('#status-line').textContent = '就绪';
      connectEvents();
      setInterval(() => {
        refreshServices();
        refreshSessions();
      }, 3000);
    } catch (e) {
      $('#status-line').textContent = '初始化失败';
      document.body.insertAdjacentHTML(
        'beforeend',
        `<div class="toast" style="display:block">${h(errText(e))}</div>`
      );
    }
  })();
})();

