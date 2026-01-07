#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include "AppGlobals.h"
#include "mode_abstraction.h"

// --- Helper to get common CSS ---
const char *COMMON_CSS = R"rawliteral(
:root {
    --bg: #000000;
    --card: #111111;
    --accent: #00ff88;
    --text: #ffffff;
    --sub: #888888;
    --err: #ff4444;
}
* { margin: 0; padding: 0; box-sizing: border-box; -webkit-font-smoothing: antialiased; }
body { 
    background: var(--bg); 
    color: var(--text); 
    font-family: -apple-system, BlinkMacSystemFont, 'Inter', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
    padding: 20px;
    max-width: 600px;
    margin: 0 auto;
    line-height: 1.5;
}
h1 { font-size: 28px; font-weight: 800; letter-spacing: -1px; margin-bottom: 20px; color: var(--accent); }
.card {
    background: var(--card);
    border: 1px solid #222;
    padding: 20px;
    border-radius: 16px;
    margin-bottom: 20px;
    box-shadow: 0 4px 20px rgba(0,0,0,0.5);
}
input, select, textarea {
    width: 100%;
    background: #1a1a1a;
    border: 1px solid #333;
    color: white;
    padding: 14px;
    font-size: 16px;
    border-radius: 12px;
    outline: none;
    transition: all 0.2s;
    margin-bottom: 15px;
}
input:focus, select:focus { border-color: var(--accent); box-shadow: 0 0 0 2px rgba(0,255,136,0.1); }
button {
    width: 100%;
    background: var(--accent);
    color: #000;
    border: none;
    padding: 14px;
    font-size: 16px;
    font-weight: 700;
    border-radius: 12px;
    cursor: pointer;
    transition: all 0.2s;
}
button:active { transform: scale(0.98); opacity: 0.9; }
button:disabled { opacity: 0.5; cursor: not-allowed; }
.btn-secondary { background: #333; color: #fff; }
.btn-danger { background: var(--err); color: #fff; }
.links { text-align: center; margin-top: 30px; opacity: 0.7; font-size: 14px; }
.links a { color: var(--text); text-decoration: none; margin: 0 10px; border-bottom: 1px solid #444; }
)rawliteral";

// --- Main Status/Reboot Page ---
const char *INDEX_HTML_TEMPLATE = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Digital Librarian - Dashboard</title>
    <style>
        %CSS%
        .stat-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-bottom: 20px; }
        .stat-card { background: #111; padding: 15px; border-radius: 12px; border: 1px solid #222; text-align: center; }
        .stat-val { font-size: 24px; font-weight: 800; color: var(--accent); display: block; }
        .stat-label { font-size: 12px; color: #666; text-transform: uppercase; letter-spacing: 1px; }
    </style>
</head>
<body>
    <h1>Librarian Dashboard</h1>
    
    <div class="card">
        <div class="stat-grid">
            <div class="stat-card"><span class="stat-val" id="count">0</span><span class="stat-label">Items</span></div>
            <div class="stat-card"><span class="stat-val" id="mode">...</span><span class="stat-label">Mode</span></div>
        </div>
        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 15px;">
            <button onclick="location.href='/browse'">&#128241; Remote</button>
            <button class="btn-secondary" onclick="location.href='/scan'">&#128247; Scanner</button>
            <button class="btn-secondary" onclick="location.href='/link'">&#128444; Covers</button>
            <button class="btn-secondary" onclick="location.href='/backup'">&#128190; Backup</button>
        </div>
    </div>

    <div class="card" style="border-color: #331111">
        <button class="btn-danger" onclick="if(confirm('Reboot device?')) { location.href='/restart'; } return false;">Reboot System</button>
    </div>

    <div class="card" style="border: 1px solid #1a3a2a;">
         <h3 style="color: #00ff88; margin-bottom: 10px; font-size: 16px;">Diagnostics</h3>
         <button class="btn-secondary" onclick="runTests()">Run Unit Tests</button>
    </div>

    <script>
        function runTests() {
            if(!confirm("Run storage unit tests? This may take a few seconds.")) return;
            // Assuming web_pin is 'cd1234' or needs to be asked. Ideally we ask user or store it.
            // For now, we prompt:
            const pin = prompt("Enter Web PIN:", "cd1234");
            if (!pin) return;
            
            fetch('/api/tests/run?pin=' + encodeURIComponent(pin), {method: 'POST'})
            .then(r => {
                if(r.status === 401) throw new Error("Unauthorized (Wrong PIN)");
                return r.text();
            })
            .then(text => {
                alert("TEST RESULTS:\n\n" + text);
            })
            .catch(e => alert("Error: " + e));
        }
    </script>

    <div style="margin-top: 40px; border-top: 1px solid #333; padding-top: 20px;">
        <h3 style="text-align:center; color:#fff; font-size: 14px; margin-bottom: 15px; text-transform: uppercase; letter-spacing: 1px;">Navigation</h3>
        <div style="display: grid; grid-template-columns: repeat(7, 1fr); gap: 10px;">
            <button onclick="location.href='/'" style="font-size: 13px; padding: 10px;">&#127968; Dashboard</button>
            <button onclick="location.href='/scan'" style="font-size: 13px; padding: 10px;">&#128247; Scanner</button>
            <button onclick="location.href='/browse'" style="font-size: 13px; padding: 10px;">&#128241; Browse</button>
            <button onclick="location.href='/link'" style="font-size: 13px; padding: 10px;">&#128444; Covers</button>
            <button onclick="location.href='/backup'" style="font-size: 13px; padding: 10px;">&#128190; Backup</button>
            <button onclick="location.href='/manual'" style="font-size: 13px; padding: 10px;">&#128214; Manuals</button>
            <button onclick="location.href='/errors'" style="font-size: 13px; padding: 10px;">&#128681; Errors</button>
        </div>
    </div>

    <script>
        fetch('/api/status').then(r=>r.json()).then(d=>{
            document.getElementById('count').textContent = d.cdCount + d.bookCount;
            document.getElementById('mode').textContent = d.currentMode == 0 ? "CD" : "BOOK";
        });
    </script>
</body>
</html>
)rawliteral";

#endif
