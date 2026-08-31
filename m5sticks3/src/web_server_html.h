#pragma once
// Dashboard HTML/CSS/JS for DeviceWebServer, split out of web_server.h so
// the markup doesn't drown out the request-handling logic. Single static
// page, no build step - fetch()-driven against the /api/* JSON endpoints.

const char DeviceWebServer::kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AI Meeting Buddy</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #0b0e14;
    --panel: #131826;
    --panel2: #1a2032;
    --border: #262d40;
    --text: #eef1f8;
    --muted: #8b93a7;
    --accent: #6ea8fe;
    --good: #3ddc97;
    --warn: #f5b94d;
    --crit: #ff6b6b;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: radial-gradient(circle at 20% -10%, #1c2438 0%, var(--bg) 55%);
    color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    padding: 24px 16px 64px;
  }
  .wrap { max-width: 980px; margin: 0 auto; }
  header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; flex-wrap: wrap; gap: 12px; }
  h1 { font-size: 22px; margin: 0; letter-spacing: -0.02em; }
  h1 span { color: var(--muted); font-weight: 400; font-size: 14px; margin-left: 8px; }
  .pill { display: inline-flex; align-items: center; gap: 6px; padding: 6px 12px; border-radius: 999px; background: var(--panel2); border: 1px solid var(--border); font-size: 13px; color: var(--muted); }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--good); }
  .dot.warn { background: var(--warn); }
  .dot.crit { background: var(--crit); }

  section { margin-bottom: 32px; }
  h2 { font-size: 14px; text-transform: uppercase; letter-spacing: 0.08em; color: var(--muted); margin: 0 0 12px; }

  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px; }
  .card { background: linear-gradient(160deg, var(--panel), var(--panel2)); border: 1px solid var(--border); border-radius: 14px; padding: 16px; }
  .card .label { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.06em; margin-bottom: 6px; }
  .card .value { font-size: 24px; font-weight: 600; letter-spacing: -0.01em; }
  .card .sub { color: var(--muted); font-size: 12px; margin-top: 4px; }
  .card.good .value { color: var(--good); }
  .card.warn .value { color: var(--warn); }
  .card.crit .value { color: var(--crit); }

  .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 14px; overflow: hidden; }

  table { width: 100%; border-collapse: collapse; font-size: 14px; }
  th, td { text-align: left; padding: 12px 16px; border-bottom: 1px solid var(--border); }
  th { color: var(--muted); font-weight: 500; font-size: 12px; text-transform: uppercase; letter-spacing: 0.05em; }
  tr:last-child td { border-bottom: none; }
  .muted { color: var(--muted); }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 999px; font-size: 11px; background: rgba(61,220,151,0.15); color: var(--good); }
  .badge.pending { background: rgba(245,185,77,0.15); color: var(--warn); }

  .btn { display: inline-flex; align-items: center; gap: 6px; padding: 7px 14px; border-radius: 8px; border: 1px solid var(--border); background: var(--panel2); color: var(--text); font-size: 13px; cursor: pointer; text-decoration: none; }
  .btn:hover { border-color: var(--accent); }
  .btn.danger:hover { border-color: var(--crit); color: var(--crit); }
  .btn.primary { background: var(--accent); border-color: var(--accent); color: #0b0e14; font-weight: 600; }
  .row-actions { display: flex; gap: 8px; }

  .empty { padding: 32px; text-align: center; color: var(--muted); }

  form.add-wifi { display: flex; gap: 8px; padding: 16px; flex-wrap: wrap; border-top: 1px solid var(--border); }
  form.add-wifi input { flex: 1; min-width: 140px; padding: 9px 12px; border-radius: 8px; border: 1px solid var(--border); background: var(--bg); color: var(--text); font-size: 14px; }
  form.add-wifi input:focus { outline: none; border-color: var(--accent); }

  .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); background: var(--panel2); border: 1px solid var(--border); padding: 10px 18px; border-radius: 10px; font-size: 13px; opacity: 0; pointer-events: none; transition: opacity .2s; }
  .toast.show { opacity: 1; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>AI Meeting Buddy <span id="ver"></span></h1>
    <div class="pill"><span class="dot" id="wifiDot"></span><span id="wifiLabel">connecting…</span></div>
  </header>

  <section>
    <h2>System</h2>
    <div class="cards" id="statCards"></div>
  </section>

  <section>
    <h2>Recordings</h2>
    <div class="panel">
      <table id="recTable" style="display:none">
        <thead><tr><th>Name</th><th>Size</th><th>Status</th><th></th></tr></thead>
        <tbody id="recBody"></tbody>
      </table>
      <div class="empty" id="recEmpty">No recordings yet.</div>
    </div>
  </section>

  <section>
    <h2>WiFi Networks</h2>
    <div class="panel">
      <table id="wifiTable" style="display:none">
        <thead><tr><th>SSID</th><th></th><th></th></tr></thead>
        <tbody id="wifiBody"></tbody>
      </table>
      <div class="empty" id="wifiEmpty" style="display:none">No saved networks.</div>
      <form class="add-wifi" id="addWifiForm">
        <input type="text" id="newSsid" placeholder="Network name (SSID)" required>
        <input type="password" id="newPass" placeholder="Password">
        <button type="submit" class="btn primary">Add / Update</button>
      </form>
    </div>
  </section>
</div>
<div class="toast" id="toast"></div>

<script>
function fmtBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024*1024) return (n/1024).toFixed(1) + ' KB';
  return (n/1024/1024).toFixed(2) + ' MB';
}
function fmtUptime(ms) {
  const s = Math.floor(ms/1000);
  const h = Math.floor(s/3600), m = Math.floor((s%3600)/60);
  return h > 0 ? h + 'h ' + m + 'm' : m + 'm';
}
function toast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.add('show');
  setTimeout(() => t.classList.remove('show'), 2000);
}

async function loadStatus() {
  const r = await fetch('/api/status');
  const s = await r.json();
  document.getElementById('ver').textContent = 'v' + s.deviceVersion;

  const dot = document.getElementById('wifiDot');
  const label = document.getElementById('wifiLabel');
  dot.className = 'dot ' + (s.wifiConnected ? 'good' : 'crit');
  label.textContent = s.wifiConnected ? (s.wifiSsid + ' · ' + s.wifiIp) : 'offline';

  const heapPct = s.totalHeap ? Math.round(100 * s.freeHeap / s.totalHeap) : 0;
  const sdPct = s.sdTotalBytes ? Math.round(100 * (s.sdTotalBytes - s.sdUsedBytes) / s.sdTotalBytes) : 0;

  const cards = [
    { label: 'Battery', value: s.battery + '%' + (s.charging ? ' ⚡' : ''), cls: s.battery < 20 ? 'crit' : (s.battery < 40 ? 'warn' : 'good') },
    { label: 'Wi-Fi signal', value: s.wifiConnected ? s.wifiRssi + ' dBm' : '—', sub: s.wifiConnected ? 'RSSI' : 'not connected', cls: s.wifiConnected ? 'good' : 'crit' },
    { label: 'Free heap', value: fmtBytes(s.freeHeap), sub: heapPct + '% free', cls: heapPct < 15 ? 'warn' : '' },
    { label: 'Free PSRAM', value: fmtBytes(s.freePsram), cls: '' },
    { label: 'SD card', value: s.sdOk ? fmtBytes(s.sdTotalBytes - s.sdUsedBytes) + ' free' : 'missing', sub: s.sdOk ? sdPct + '% free' : '', cls: s.sdOk ? '' : 'crit' },
    { label: 'Pending uploads', value: s.pendingUploads, cls: s.pendingUploads > 0 ? 'warn' : 'good' },
    { label: 'Uptime', value: fmtUptime(s.uptimeMs), cls: '' },
  ];
  document.getElementById('statCards').innerHTML = cards.map(c =>
    `<div class="card ${c.cls}"><div class="label">${c.label}</div><div class="value">${c.value}</div>${c.sub ? `<div class="sub">${c.sub}</div>` : ''}</div>`
  ).join('');
}

async function loadRecordings() {
  const r = await fetch('/api/recordings');
  const recs = await r.json();
  const table = document.getElementById('recTable');
  const body = document.getElementById('recBody');
  const empty = document.getElementById('recEmpty');
  if (!recs.length) {
    table.style.display = 'none';
    empty.style.display = 'block';
    return;
  }
  table.style.display = '';
  empty.style.display = 'none';
  body.innerHTML = recs.map(rec => `
    <tr>
      <td>${rec.name}</td>
      <td class="muted">${fmtBytes(rec.sizeBytes)}</td>
      <td>${rec.uploaded ? '<span class="badge">uploaded</span>' : '<span class="badge pending">pending</span>'}</td>
      <td class="row-actions">
        <a class="btn" href="/download?name=${encodeURIComponent(rec.name)}">Download</a>
        <button class="btn danger" onclick="deleteRecording('${rec.name}')">Delete</button>
      </td>
    </tr>`).join('');
}

async function deleteRecording(name) {
  if (!confirm('Delete ' + name + '?')) return;
  const r = await fetch('/api/recordings/delete', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: 'name=' + encodeURIComponent(name) });
  if (r.ok) { toast('Deleted ' + name); loadRecordings(); } else { toast('Delete failed'); }
}

async function loadWifi() {
  const r = await fetch('/api/wifi');
  const data = await r.json();
  const table = document.getElementById('wifiTable');
  const body = document.getElementById('wifiBody');
  const empty = document.getElementById('wifiEmpty');
  if (!data.networks.length) {
    table.style.display = 'none';
    empty.style.display = 'block';
    return;
  }
  table.style.display = '';
  empty.style.display = 'none';
  body.innerHTML = data.networks.map(n => `
    <tr>
      <td>${n.ssid}${n.ssid === data.preferred ? ' <span class="badge">preferred</span>' : ''}</td>
      <td></td>
      <td class="row-actions">
        <button class="btn danger" onclick="deleteWifi('${n.ssid}')">Remove</button>
      </td>
    </tr>`).join('');
}

async function deleteWifi(ssid) {
  if (!confirm('Remove saved network "' + ssid + '"?')) return;
  const r = await fetch('/api/wifi/delete', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: 'ssid=' + encodeURIComponent(ssid) });
  if (r.ok) { toast('Removed ' + ssid); loadWifi(); } else { toast('Remove failed'); }
}

document.getElementById('addWifiForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const ssid = document.getElementById('newSsid').value;
  const pass = document.getElementById('newPass').value;
  if (!ssid) return;
  const r = await fetch('/api/wifi', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pass) });
  if (r.ok) {
    toast('Saved ' + ssid);
    document.getElementById('newSsid').value = '';
    document.getElementById('newPass').value = '';
    loadWifi();
  } else {
    toast('Save failed');
  }
});

function refreshAll() {
  loadStatus();
  loadRecordings();
  loadWifi();
}
refreshAll();
setInterval(loadStatus, 5000);
</script>
</body>
</html>
)HTML";
