#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include "AppGlobals.h"
#include "mode_abstraction.h"

// --- Helper to get common CSS ---
const char *COMMON_CSS = R"rawliteral(
:root {
    color-scheme: dark;
    --bg: #07111d;
    --bg-deep: #030913;
    --card: #10243a;
    --card-strong: #17344f;
    --line: #2d6688;
    --line-soft: rgba(83, 199, 255, .2);
    --accent: #f6cf4a;
    --accent-strong: #ffdc62;
    --cyan: #53c7ff;
    --text: #f4f9fc;
    --sub: #9bb4c6;
    --ok: #55d69e;
    --warn: #ffad42;
    --err: #ff655f;
    --shadow: 0 18px 50px rgba(0, 0, 0, .32);
}
* { margin: 0; padding: 0; box-sizing: border-box; -webkit-font-smoothing: antialiased; }
html { min-height: 100%; background: var(--bg-deep); }
body { 
    min-height: 100vh;
    background:
      radial-gradient(circle at 12% 0%, rgba(83,199,255,.13), transparent 32rem),
      radial-gradient(circle at 100% 15%, rgba(246,207,74,.08), transparent 26rem),
      linear-gradient(160deg, var(--bg), var(--bg-deep));
    color: var(--text); 
    font-family: -apple-system, BlinkMacSystemFont, 'Inter', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
    padding: 24px;
    max-width: 780px;
    margin: 0 auto;
    line-height: 1.55;
}
h1 { font-size: clamp(30px, 7vw, 48px); line-height: 1.02; font-weight: 850; letter-spacing: -1.5px; color: var(--text); }
h2 { font-size: 20px; line-height: 1.25; }
h3 { font-size: 16px; }
.hero { padding: 28px 4px 22px; }
.hero-row, .section-head, .inline-row { display: flex; align-items: center; justify-content: space-between; gap: 14px; }
.eyebrow { color: var(--cyan); font-size: 12px; font-weight: 800; letter-spacing: .16em; text-transform: uppercase; margin-bottom: 8px; }
.subtitle, .muted { color: var(--sub); }
.subtitle { margin-top: 10px; max-width: 560px; }
.status-pill { display: inline-flex; align-items: center; gap: 7px; flex: none; padding: 8px 11px; border: 1px solid var(--line-soft); border-radius: 999px; background: rgba(7,17,29,.7); color: var(--sub); font-size: 12px; font-weight: 750; }
.status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--ok); box-shadow: 0 0 12px var(--ok); }
.card {
    background: linear-gradient(145deg, rgba(23,52,79,.96), rgba(12,31,50,.96));
    border: 1px solid var(--line-soft);
    padding: 22px;
    border-radius: 20px;
    margin-bottom: 16px;
    box-shadow: var(--shadow);
}
.section-head { margin-bottom: 16px; }
.section-kicker { color: var(--accent); font-size: 12px; font-weight: 800; letter-spacing: .11em; text-transform: uppercase; }
.action-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 12px; }
.action-link { min-height: 76px; display: flex; align-items: center; gap: 13px; padding: 15px; border-radius: 15px; border: 1px solid var(--line-soft); background: rgba(7,17,29,.48); color: var(--text); text-decoration: none; transition: transform .16s, border-color .16s, background .16s; }
.action-link:hover { border-color: var(--cyan); background: rgba(83,199,255,.08); transform: translateY(-1px); }
.action-icon { width: 42px; height: 42px; display: grid; place-items: center; flex: none; border-radius: 12px; background: var(--card-strong); color: var(--accent); font-size: 21px; }
.action-copy { min-width: 0; }
.action-title { display: block; font-size: 15px; font-weight: 800; }
.action-desc { display: block; margin-top: 2px; color: var(--sub); font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
input, select, textarea {
    width: 100%;
    min-height: 48px;
    background: rgba(4,13,24,.74);
    border: 1px solid var(--line);
    color: var(--text);
    padding: 13px 14px;
    font-size: 16px;
    border-radius: 13px;
    outline: none;
    transition: border-color .16s, box-shadow .16s, background .16s;
}
input:focus, select:focus, textarea:focus { border-color: var(--accent); background: rgba(8,25,42,.95); box-shadow: 0 0 0 3px rgba(246,207,74,.14); }
label { display: block; margin: 0 0 7px; color: var(--sub); font-size: 12px; font-weight: 750; letter-spacing: .03em; }
.field { margin-bottom: 16px; }
button {
    width: 100%;
    min-height: 48px;
    background: var(--accent);
    color: #18200b;
    border: none;
    padding: 13px 16px;
    font-size: 16px;
    font-weight: 800;
    border-radius: 13px;
    cursor: pointer;
    transition: transform .15s, filter .15s, opacity .15s;
}
button:hover { filter: brightness(1.06); }
button:active { transform: scale(0.985); }
button:disabled { opacity: 0.5; cursor: not-allowed; }
.btn-secondary { background: var(--card-strong); color: var(--text); border: 1px solid var(--line); }
.btn-danger { background: var(--err); color: #fff; }
.btn-quiet { background: rgba(7,17,29,.55); color: var(--text); border: 1px solid var(--line-soft); }
.feedback { display: none; margin-top: 14px; padding: 12px 14px; border-radius: 12px; border: 1px solid var(--line-soft); background: rgba(7,17,29,.6); color: var(--sub); font-size: 14px; }
.feedback.show { display: block; }
.feedback.success { border-color: rgba(85,214,158,.5); color: var(--ok); }
.feedback.error { border-color: rgba(255,101,95,.55); color: #ffaaa6; }
.spinner { display: inline-block; width: 16px; height: 16px; border: 2px solid rgba(255,255,255,.25); border-top-color: currentColor; border-radius: 50%; animation: spin .8s linear infinite; vertical-align: -3px; margin-right: 7px; }
@keyframes spin { to { transform: rotate(360deg); } }
.links { text-align: center; margin-top: 30px; opacity: 0.7; font-size: 14px; }
.links a { color: var(--text); text-decoration: none; margin: 0 10px; border-bottom: 1px solid #444; }
.site-nav { margin-top: 28px; padding: 20px 0 8px; border-top: 1px solid var(--line-soft); }
.site-nav h3 { margin-bottom: 12px; color: var(--sub); font-size: 11px; letter-spacing: .14em; text-align: center; text-transform: uppercase; }
.nav-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(92px, 1fr)); gap: 8px; }
.nav-link { min-height: 44px; display: grid; place-items: center; padding: 9px; border-radius: 11px; border: 1px solid var(--line-soft); background: rgba(16,36,58,.7); color: var(--text); text-decoration: none; font-size: 12px; font-weight: 750; text-align: center; }
.nav-link:hover { border-color: var(--cyan); }
@media (max-width: 560px) {
  body { padding: 16px; }
  .hero { padding-top: 20px; }
  .hero-row { align-items: flex-start; }
  .card { padding: 17px; border-radius: 17px; }
  .action-grid { grid-template-columns: 1fr; }
  .action-link { min-height: 68px; }
  .nav-grid { grid-template-columns: repeat(3, minmax(0,1fr)); }
}
@media (prefers-reduced-motion: reduce) { *, *::before, *::after { scroll-behavior: auto !important; animation-duration: .01ms !important; transition-duration: .01ms !important; } }
)rawliteral";

// --- Main Status/Reboot Page ---
const char *INDEX_HTML_TEMPLATE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta charset="UTF-8">
    <title>Digital Librarian - Dashboard</title>
    <style>
        %CSS%
        .stat-grid { display: grid; grid-template-columns: repeat(4, minmax(0,1fr)); gap: 10px; }
        .stat-card { min-width: 0; padding: 15px 10px; border-radius: 14px; border: 1px solid var(--line-soft); background: rgba(4,13,24,.55); text-align: center; }
        .stat-val { display: block; color: var(--accent); font-size: clamp(20px,5vw,28px); font-weight: 850; line-height: 1.1; overflow: hidden; text-overflow: ellipsis; }
        .stat-label { display: block; margin-top: 6px; color: var(--sub); font-size: 10px; font-weight: 800; letter-spacing: .11em; text-transform: uppercase; }
        .system-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 12px; color: var(--sub); font-size: 13px; }
        .system-row span { padding: 9px 11px; border-radius: 10px; background: rgba(4,13,24,.4); }
        .admin-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        @media(max-width:560px){.stat-grid{grid-template-columns:1fr 1fr}.admin-grid{grid-template-columns:1fr}.system-row{grid-template-columns:1fr}}
    </style>
</head>
<body>
    <header class="hero">
      <div class="hero-row">
        <div>
          <div class="eyebrow">Your collection, at a glance</div>
          <h1>Digital Librarian</h1>
        </div>
        <span class="status-pill" id="connection"><span class="status-dot"></span><span id="connectionText">Connecting</span></span>
      </div>
      <p class="subtitle">Browse, add, organize, and protect your collection from any device on this network.</p>
    </header>

    <section class="card">
        <div class="section-head"><div><div class="section-kicker">Library</div><h2>Collection overview</h2></div><span class="muted" id="lastUpdated">Updating…</span></div>
        <div class="stat-grid">
            <div class="stat-card"><span class="stat-val" id="count">—</span><span class="stat-label">All items</span></div>
            <div class="stat-card"><span class="stat-val" id="cdCount">—</span><span class="stat-label">CDs</span></div>
            <div class="stat-card"><span class="stat-val" id="bookCount">—</span><span class="stat-label">Books</span></div>
            <div class="stat-card"><span class="stat-val" id="mode">—</span><span class="stat-label">Current view</span></div>
        </div>
        <div class="system-row"><span>Free memory: <strong id="heap">—</strong></span><span>Uptime: <strong id="uptime">—</strong></span></div>
    </section>

    <section class="card">
        <div class="section-head"><div><div class="section-kicker">Shortcuts</div><h2>What would you like to do?</h2></div></div>
        <div class="action-grid">
            <a class="action-link" href="/browse"><span class="action-icon">&#128241;</span><span class="action-copy"><span class="action-title">Browse collection</span><span class="action-desc">Search, select, filter, and edit</span></span></a>
            <a class="action-link" href="/scan"><span class="action-icon">&#128247;</span><span class="action-copy"><span class="action-title">Add with a code</span><span class="action-desc">Scan one or several barcodes</span></span></a>
            <a class="action-link" href="/link"><span class="action-icon">&#128444;</span><span class="action-copy"><span class="action-title">Update cover art</span><span class="action-desc">Attach an image to a record</span></span></a>
            <a class="action-link" href="/backup"><span class="action-icon">&#128190;</span><span class="action-copy"><span class="action-title">Backup and restore</span><span class="action-desc">Protect your library data</span></span></a>
        </div>
    </section>

    <section class="card">
      <div class="section-head"><div><div class="section-kicker">Device</div><h2>Maintenance</h2></div></div>
      <div class="admin-grid"><button class="btn-secondary" id="testButton" onclick="runTests()">Run diagnostics</button><button class="btn-danger" id="restartButton" onclick="restartDevice()">Restart device</button></div>
      <div class="feedback" id="feedback" role="status" aria-live="polite"></div>
    </section>

    <script>
        const feedback=document.getElementById('feedback');
        function showFeedback(message,type='') { feedback.textContent=message; feedback.className='feedback show '+type; }
        async function runTests() {
            if(!confirm("Run storage unit tests? This may take a few seconds.")) return;
            const pin = prompt("Enter Web PIN:");
            if (!pin) return;
            const button=document.getElementById('testButton');button.disabled=true;button.innerHTML='<span class="spinner"></span>Running diagnostics';
            try { const r=await fetch('/api/tests/run',{method:'POST',headers:{'X-Auth-Pin':pin}});const text=await r.text();if(!r.ok)throw new Error(r.status===401?'Incorrect PIN':text);showFeedback(text,r.ok?'success':'error'); }
            catch(e){showFeedback(e.message,'error');}
            finally{button.disabled=false;button.textContent='Run diagnostics';}
        }

        async function restartDevice() {
            if(!confirm('Reboot device?')) return;
            const pin = prompt('Enter Web PIN:');
            if(!pin) return;
            const button=document.getElementById('restartButton');button.disabled=true;button.innerHTML='<span class="spinner"></span>Restarting';
            try{const r=await fetch('/restart',{method:'POST',headers:{'X-Auth-Pin':pin}});const text=await r.text();if(!r.ok)throw new Error(r.status===401?'Incorrect PIN':text||'Restart failed');showFeedback('Device is restarting. Reconnect in a moment.','success');}
            catch(e){showFeedback(e.message,'error');button.disabled=false;button.textContent='Restart device';}
        }

        async function updateStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error();const d=await r.json();document.getElementById('count').textContent=d.cdCount+d.bookCount;document.getElementById('cdCount').textContent=d.cdCount;document.getElementById('bookCount').textContent=d.bookCount;document.getElementById('mode').textContent=d.currentMode===0?'CDs':d.currentMode===1?'Books':'All';document.getElementById('heap').textContent=Math.round(d.heap/1024)+' KB';const mins=Math.floor(d.uptime/60);document.getElementById('uptime').textContent=mins<60?mins+' min':Math.floor(mins/60)+'h '+(mins%60)+'m';document.getElementById('lastUpdated').textContent='Updated now';document.getElementById('connectionText').textContent='Online';document.getElementById('connection').style.opacity='1';}catch(e){document.getElementById('connectionText').textContent='Offline';document.querySelector('.status-dot').style.background='var(--err)';document.getElementById('lastUpdated').textContent='Could not refresh';}}
        updateStatus();setInterval(updateStatus,15000);
    </script>

    <nav class="site-nav" aria-label="Web interface navigation">
        <h3>Navigation</h3>
        <div class="nav-grid">
            <a class="nav-link" href="/">&#127968; Home</a>
            <a class="nav-link" href="/scan">&#128247; Add</a>
            <a class="nav-link" href="/browse">&#128241; Browse</a>
            <a class="nav-link" href="/link">&#128444; Covers</a>
            <a class="nav-link" href="/backup">&#128190; Backup</a>
            <a class="nav-link" href="/manual">&#128214; Manual</a>
            <a class="nav-link" href="/errors">&#128681; Health</a>
        </div>
    </nav>
</body>
</html>
)rawliteral";

#endif
