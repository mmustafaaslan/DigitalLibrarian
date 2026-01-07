#include <Arduino.h>
#include <ArduinoJson.h> // v6.x
#include <ESPmDNS.h>
#include <FastLED.h> // v3.6.0
#include <HTTPClient.h>
#include <SD.h>
#include <Waveshare_ST7262_LVGL.h> // LVGL v8.3
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h> // For detailed heap analysis

#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "Core_Data.h"
#include "ErrorHandler.h"
#include "MediaManager.h"
#include "NavigationCache.h"
#include "NetworkManager.h"
#include "Storage.h"
#include "StorageTests.h"
#include "UIManager.h"
#include "Utils.h"
#include "WebInterface.h"
#include "mode_abstraction.h"

// ========================================
// GLOBAL OBJECTS
// ========================================
WebServer server(80);
File uploadFile;
SemaphoreHandle_t libraryMutex = NULL;
SemaphoreHandle_t i2cMutex = NULL;

// ========================================
// HELPER FUNCTIONS
// ========================================
void logMemoryUsage(const char *label) {
  Serial.printf("[%s] Free Heap: %u | Min Free: %u | Largest Block: %u\n",
                label, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                ESP.getMaxAllocHeap());
}

void printDetailedMemoryStats() {
  Serial.println("\n--- DETAILED MEMORY ANALYSIS ---");
  Serial.printf("Total Free Heap: %u\n", ESP.getFreeHeap());
  Serial.printf("Total Free PSRAM: %u\n", ESP.getFreePsram());

  Serial.println("\n[INTERNAL RAM] Heap Info:");
  heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);

  Serial.println("\n[PSRAM] Heap Info:");
  heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
  Serial.println("--------------------------------\n");
}

String getCommonCSS() { return String(COMMON_CSS); }
String getWebFooter() {
  String f = "<div style='margin-top: 40px; border-top: 1px solid #333; "
             "padding-top: 20px;'>";
  f += "<h3 style='text-align:center; color:#fff; font-size: 14px; "
       "margin-bottom: 15px; text-transform: uppercase; letter-spacing: "
       "1px;'>Navigation</h3>";
  f += "<div style='display: grid; grid-template-columns: repeat(7, 1fr); "
       "gap: 10px;'>";
  f += "<button onclick=\"location.href='/'\" style='font-size: "
       "13px; padding: 10px;'>&#127968; Dashboard</button>";
  f += "<button onclick=\"location.href='/scan'\" style='font-size: "
       "13px; padding: 10px;'>&#128247; Scanner</button>";
  f += "<button onclick=\"location.href='/browse'\" style='font-size: "
       "13px; padding: 10px;'>&#128241; Browse</button>";
  f += "<button onclick=\"location.href='/link'\" style='font-size: "
       "13px; padding: 10px;'>&#128444; Covers</button>";
  f += "<button onclick=\"location.href='/backup'\" style='font-size: "
       "13px; padding: 10px;'>&#128190; Backup</button>";
  f += "<button onclick=\"location.href='/manual'\" style='font-size: "
       "13px; padding: 10px;'>&#128214; Manuals</button>";
  f += "<button onclick=\"location.href='/errors'\" style='font-size: "
       "13px; padding: 10px;'>&#128681; Errors</button>";
  f += "</div></div>";
  return f;
}

void sendHTMLPage(const char *title, String body, String script = "") {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1, "
          "maximum-scale=1'>";
  html += "<meta charset='UTF-8'><title>" + String(title) + "</title>";
  html += "<style>" + getCommonCSS() + "</style></head><body>" + body;
  if (script.length() > 0)
    html += "<script>" + script + "</script>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ========================================
// WEB HANDLERS
// ========================================

void setupWebHandlers() {
  // 1. Dashboard
  server.on("/", HTTP_GET, []() {
    String html = String(INDEX_HTML_TEMPLATE);
    html.replace("%CSS%", getCommonCSS());
    server.send(200, "text/html", html);
  });

  // 2. Status API
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    doc["cdCount"] = cdLibrary.size();
    doc["bookCount"] = bookLibrary.size();
    doc["currentMode"] = (int)currentMode;
    doc["heap"] = ESP.getFreeHeap();
    doc["uptime"] = millis() / 1000;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // 2.5. Error Log API
  server.on("/api/errors", HTTP_GET, []() {
    DynamicJsonDocument doc(4096);

    // System health
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["minFreeHeap"] = ESP.getMinFreeHeap();
    doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
    doc["uptime"] = millis() / 1000;
    doc["memoryLow"] = ErrorHandler::isMemoryLow();

    // Recent errors
    JsonArray errors = doc.createNestedArray("errors");
    const auto &recentErrors = ErrorHandler::getRecentErrors();

    for (const auto &error : recentErrors) {
      JsonObject errObj = errors.createNestedObject();
      errObj["timestamp"] = error.timestamp;
      errObj["level"] = (int)error.level;
      errObj["category"] = (int)error.category;
      errObj["message"] = error.message;
      if (error.context.length() > 0) {
        errObj["context"] = error.context;
      }
    }

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // 2.6. Clear Error Log API
  server.on("/api/errors/clear", HTTP_POST, []() {
    ErrorHandler::clearRecentErrors();
    ErrorHandler::logInfo(ERR_CAT_SYSTEM, "Error log cleared via web API",
                          "/api/errors/clear");
    server.send(200, "application/json", "{\"status\":\"cleared\"}");
  });

  // 2.7. Run Unit Tests (Dev Only)
  server.on("/api/tests/run", HTTP_ANY, []() {
    if (server.arg("pin") != web_pin) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    String results = StorageTests::runTests();
    server.send(200, "text/plain", results);
  });

  // 2.8. detailed heap analysis
  server.on("/api/debug/heap", HTTP_GET, []() {
    printDetailedMemoryStats();
    server.send(200, "text/plain",
                "Detailed heap info printed to Serial Console.");
  });

  // 3. Remote Control API
  server.on("/api/control", HTTP_ANY, []() {
    String action = server.arg("action");

    // ledpreview doesn't need auth (temporary visual only)
    if (action != "ledpreview") {
      // Validate PIN for all other actions
      if (server.arg("pin") != web_pin) {
        server.send(401, "text/plain", "Unauthorized");
        return;
      }
    }

    if (action == "random") {
      selectRandomWithEffect();
      server.send(200, "text/plain", "Random selected");

    } else if (action == "select") {
      int id = server.arg("id").toInt();
      if (id >= 0 && id < getItemCount()) {
        setCurrentItemIndex(id);
        lvgl_port_lock(-1);
        update_item_display();
        lvgl_port_unlock();
        server.send(200, "text/plain", "Selected " + String(id));
      } else {
        server.send(400, "text/plain", "Invalid ID");
      }

    } else if (action == "edit") {
      int id = server.arg("id").toInt();
      Serial.printf(">> WEB EDIT: ID %d\n", id); // DEBUG LOGGING

      if (id >= 0 && id < getItemCount()) {
        ensureItemDetailsLoaded(id);
        ItemView item = getItemAt(id);
        String oldUniqueID = item.uniqueID;

        if (server.hasArg("title"))
          item.title = server.arg("title");
        if (server.hasArg("artist"))
          item.artistOrAuthor = server.arg("artist");
        if (server.hasArg("genre"))
          item.genre = server.arg("genre");
        if (server.hasArg("year"))
          item.year = server.arg("year").toInt();
        if (server.hasArg("fav"))
          item.favorite = (server.arg("fav") == "true");
        if (server.hasArg("uniqueID"))
          item.uniqueID = server.arg("uniqueID");

        if (server.hasArg("ledIndex")) {
          String lStr = server.arg("ledIndex");
          lStr.replace(" ", ","); // Fix: Handle space delimiter
          Serial.printf(">> WEB PARSE LEDs: '%s'\n", lStr.c_str());

          item.ledIndices.clear();
          while (lStr.length() > 0) {
            int comma = lStr.indexOf(',');
            if (comma == -1) {
              lStr.trim();
              if (lStr.length() > 0)
                item.ledIndices.push_back(lStr.toInt());
              break;
            } else {
              String part = lStr.substring(0, comma);
              part.trim();
              if (part.length() > 0)
                item.ledIndices.push_back(part.toInt());
              lStr = lStr.substring(comma + 1);
            }
          }
          if (item.ledIndices.empty())
            item.ledIndices.push_back(0);
        }

        if (server.hasArg("barcode"))
          item.codecOrIsbn = server.arg("barcode");
        if (server.hasArg("notes"))
          item.notes = server.arg("notes");

        Serial.printf(">> WEB SAVE: Title='%s', LEDs=%d\n", item.title.c_str(),
                      (int)item.ledIndices.size());
        setItem(id, item);
        saveLibrary();

        // DEEP SAVE: Ensure detail file on SD is also updated with new
        // LEDs/notes
        switch (currentMode) {
        case MODE_CD:
          Storage.saveCD(cdLibrary[id], oldUniqueID.c_str());
          break;
        case MODE_BOOK:
          Storage.saveBook(bookLibrary[id], oldUniqueID.c_str());
          break;
        default:
          break;
        }

        if (getCurrentItemIndex() == id) {
          lvgl_port_lock(-1);
          update_item_display();
          lvgl_port_unlock();
        }
        server.send(200, "text/plain", "Saved");
      } else {
        server.send(400, "text/plain", "Invalid ID");
      }

    } else if (action == "ledupdate") {
      int id = server.arg("id").toInt();
      Serial.printf(">> WEB LED UPDATE: ID %d\n", id);

      if (id >= 0 && id < getItemCount()) {
        ItemView item = getItemAt(id); // Use getItemAt to respect abstraction
        String lStr = server.arg("leds");
        lStr.replace(" ", ","); // Fix: Handle space delimiter

        item.ledIndices.clear();
        while (lStr.length() > 0) {
          int comma = lStr.indexOf(',');
          if (comma == -1) {
            lStr.trim();
            if (lStr.length() > 0)
              item.ledIndices.push_back(lStr.toInt());
            break;
          } else {
            String part = lStr.substring(0, comma);
            part.trim();
            if (part.length() > 0)
              item.ledIndices.push_back(part.toInt());
            lStr = lStr.substring(comma + 1);
          }
        }

        setItem(id, item);
        saveLibrary();

        // DEEP SAVE
        switch (currentMode) {
        case MODE_CD:
          Storage.saveCD(cdLibrary[id]);
          break;
        case MODE_BOOK:
          Storage.saveBook(bookLibrary[id]);
          break;
        default:
          break;
        }

        if (getCurrentItemIndex() == id && edit_item_index == id) {
          lvgl_port_lock(-1);
          String ledStr = "";
          for (size_t i = 0; i < item.ledIndices.size(); i++) {
            ledStr += String(item.ledIndices[i]);
            if (i < item.ledIndices.size() - 1)
              ledStr += ", ";
          }
          if (ta_led_index)
            lv_textarea_set_text(ta_led_index, ledStr.c_str());
          lvgl_port_unlock();
        }
        server.send(200, "text/plain", "LEDs Updated");
      } else {
        server.send(400, "text/plain", "Invalid ID");
      }

    } else if (action == "ledpreview") {
      // Live preview LEDs from web UI
      if (!led_master_on) {
        server.send(200, "text/plain", "Preview skipped (LEDs off)");
        return;
      }

      String lStr = server.arg("leds");

      // Activate preview mode for 10 seconds
      previewModeUntil = millis() + 10000;

      // Don't clear - keep current item's LEDs, just overlay preview
      // First, restore current item's LEDs
      FastLED.clear();
      int curIdx = getCurrentItemIndex();
      if (curIdx >= 0 && curIdx < getItemCount()) {
        ItemView currentItem = getItemAt(curIdx);
        for (int ledIdx : currentItem.ledIndices) {
          if (ledIdx >= 0 && ledIdx < led_count) {
            leds[ledIdx] = COLOR_SELECTED; // Show current item in green
          }
        }
      }

      // Now overlay preview LEDs in yellow
      while (lStr.length() > 0) {
        int comma = lStr.indexOf(',');
        int ledNum = -1;
        if (comma == -1) {
          if (lStr.length() > 0)
            ledNum = lStr.toInt();
          lStr = "";
        } else {
          ledNum = lStr.substring(0, comma).toInt();
          lStr = lStr.substring(comma + 1);
        }

        if (ledNum >= 0 && ledNum < led_count) {
          leds[ledNum] = COLOR_TEMPORARY; // Preview in yellow (overrides green
                                          // if same LED)
        }
      }

      FastLED.show();
      if (led_use_wled)
        forceUpdateWLED();
      server.send(200, "text/plain", "Preview OK");

    } else if (action == "applyfilter") {
      // Apply filters from web interface
      filter_genre = server.arg("genre");
      String decade_str = server.arg("decade");
      filter_decade = decade_str.length() > 0 ? decade_str.toInt() : 0;
      filter_favorites_only = server.arg("favorites") == "true";
      filter_active = true;
      update_filtered_leds();
      server.send(200, "text/plain", "Filters applied");

    } else if (action == "clearfilter") {
      // Clear filters from web interface
      filter_active = false;
      filter_genre = "";
      filter_decade = 0;
      filter_favorites_only = false;
      lvgl_port_lock(-1);
      update_item_display();
      lvgl_port_unlock();
      server.send(200, "text/plain", "Filters cleared");

    } else {
      server.send(400, "text/plain", "Unknown Action");
    }
  });

  // LED Selector Web UI
  server.on("/led-select", HTTP_GET, []() {
    int cdId = server.arg("cd").toInt();
    if (cdId < 0 || cdId >= getItemCount()) {
      server.send(400, "text/plain", "Invalid CD ID");
      return;
    }

    ItemView cd = getItemAt(cdId);
    String selectedLEDs = "";
    for (size_t i = 0; i < cd.ledIndices.size(); i++) {
      selectedLEDs += String(cd.ledIndices[i]);
      if (i < cd.ledIndices.size() - 1)
        selectedLEDs += ",";
    }

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' "
            "content='width=device-width,initial-scale=1,maximum-scale=1,user-"
            "scalable=no'>";
    html += "<title>LED Selector - " + cd.title + "</title>";
    html += "<style>";
    html += "*{margin:0;padding:0;box-sizing:border-box}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe "
            "UI',Roboto,Arial,sans-serif;background:#0a0a0a;color:#fff;padding-"
            "bottom:80px}";
    html += ".header{position:sticky;top:0;background:linear-gradient(135deg,#"
            "1a1a1a,#0f0f0f);padding:15px;border-bottom:2px solid "
            "#00ff88;box-shadow:0 4px 12px rgba(0,255,136,0.2);z-index:100}";
    html += ".cd-info{margin-bottom:12px}.cd-title{font-size:20px;font-weight:"
            "700;color:#00ff88;margin-bottom:4px}.cd-artist{font-size:14px;"
            "color:#aaa;opacity:0.9}";
    html += ".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}";
    html += ".btn{padding:10px "
            "16px;border:none;border-radius:8px;font-weight:600;font-size:13px;"
            "cursor:pointer;transition:all "
            "0.2s;flex:1;min-width:80px;text-align:center}";
    html += ".btn-primary{background:#00ff88;color:#000}.btn-primary:active{"
            "background:#00dd77;transform:scale(0.98)}";
    html += ".btn-secondary{background:#333;color:#fff}.btn-secondary:active{"
            "background:#444;transform:scale(0.98)}";
    html += ".btn-danger{background:#ff4444;color:#fff}.btn-danger:active{"
            "background:#dd3333;transform:scale(0.98)}";
    html += ".search-box{width:100%;padding:12px;background:#1a1a1a;border:2px "
            "solid "
            "#333;border-radius:8px;color:#fff;font-size:15px;margin-top:10px}";
    html += ".search-box:focus{outline:none;border-color:#00ff88}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax("
            "50px,1fr));gap:6px;padding:15px;max-width:100%}";
    html += ".led{aspect-ratio:1;border-radius:8px;border:2px solid "
            "#333;background:#1a1a1a;display:flex;align-items:center;justify-"
            "content:center;font-size:11px;font-weight:700;cursor:pointer;"
            "transition:all "
            "0.15s;user-select:none;-webkit-tap-highlight-color:transparent}";
    html += ".led.selected{background:#00ff88;color:#000;border-color:#00ff88;"
            "box-shadow:0 0 12px rgba(0,255,136,0.6)}";
    html += ".led:active{transform:scale(0.92)}";
    html += ".led:hover{border-color:#00ff88;transform:scale(1.05)}";
    html += ".footer{position:fixed;bottom:0;left:0;right:0;background:linear-"
            "gradient(135deg,#1a1a1a,#0f0f0f);padding:15px;border-top:2px "
            "solid #00ff88;box-shadow:0 -4px 12px "
            "rgba(0,255,136,0.2);display:flex;gap:10px}";
    html += ".count{text-align:center;padding:10px;background:#1a1a1a;border-"
            "radius:8px;font-size:14px;color:#00ff88;font-weight:600;flex:1}";
    html += "@media(max-width:600px){.grid{grid-template-columns:repeat(auto-"
            "fill,minmax(45px,1fr));gap:5px}.led{font-size:10px}}";
    html += "</style></head><body>";

    html += "<div class='header'>";
    html += "<div class='cd-info'><div class='cd-title'>" + cd.title +
            "</div><div class='cd-artist'>" + cd.artistOrAuthor +
            "</div></div>";
    html += "<div class='actions'>";
    html += "<button class='btn btn-secondary' "
            "onclick='selectRange()'>Range</button>";
    html +=
        "<button class='btn btn-secondary' onclick='clearAll()'>Clear</button>";
    html += "</div>";
    html +=
        "<input type='text' class='search-box' id='search' placeholder='Jump "
        "to LED # or range (e.g. 10 or 10-20)' onkeyup='handleSearch(event)'>";
    html += "</div>";

    html += "<div class='grid' id='grid'></div>";

    html += "<div class='footer'>";
    html += "<div class='count'><div "
            "style='font-size:11px;opacity:0.7'>SELECTED</div><div "
            "id='count'>0</div></div>";
    html += "<button class='btn btn-primary' onclick='save()' "
            "style='flex:2'>SAVE & CLOSE</button>";
    html += "</div>";

    html += "<script>";
    html += "const cdId=" + String(cdId) + ";";
    html += "const totalLEDs=" + String(led_count) + ";";
    html += "let selected=new Set([" + selectedLEDs + "]);";
    html += "const grid=document.getElementById('grid');";
    html += "const countEl=document.getElementById('count');";

    html += "function createLED(i){";
    html += "const led=document.createElement('div');";
    html += "led.className='led'+(selected.has(i)?' selected':'');";
    html += "led.textContent=i;";
    html += "led.onclick=()=>toggle(i);";
    html += "return led;";
    html += "}";

    html += "function render(){";
    html += "grid.innerHTML='';";
    html += "for(let i=0;i<totalLEDs;i++)grid.appendChild(createLED(i));";
    html += "updateCount();";
    html += "}";

    html += "function toggle(i){";
    html += "if(selected.has(i))selected.delete(i);else selected.add(i);";
    html += "event.target.classList.toggle('selected');";
    html += "updateCount();";
    html += "preview();";
    html += "}";

    html += "function updateCount(){countEl.textContent=selected.size;}";

    html += "function selectAll(){selected=new "
            "Set([...Array(totalLEDs).keys()]);render();preview();}";
    html += "function clearAll(){selected.clear();render();preview();}";
    html += "function invertSelection(){const newSet=new Set();for(let "
            "i=0;i<totalLEDs;i++)if(!selected.has(i))newSet.add(i);selected="
            "newSet;render();preview();}";

    html += "function selectRange(){";
    html += "const range=prompt('Enter range (e.g., 10-20):');";
    html += "if(!range)return;";
    html += "const parts=range.split('-').map(s=>parseInt(s.trim()));";
    html += "if(parts.length===2&&!isNaN(parts[0])&&!isNaN(parts[1])){";
    html += "for(let "
            "i=Math.min(...parts);i<=Math.max(...parts);i++)if(i>=0&&i<"
            "totalLEDs)selected.add(i);";
    html += "render();preview();";
    html += "}}";

    html += "function handleSearch(e){";
    html += "if(e.key!=='Enter')return;";
    html += "const val=e.target.value.trim();";
    html += "if(!val)return;";
    html += "if(val.includes('-')){const "
            "parts=val.split('-').map(s=>parseInt(s.trim()));";
    html += "if(parts.length===2&&!isNaN(parts[0])&&!isNaN(parts[1])){";
    html += "for(let "
            "i=Math.min(...parts);i<=Math.max(...parts);i++)if(i>=0&&i<"
            "totalLEDs)selected.add(i);";
    html += "render();preview();e.target.value='';}}";
    html += "else{const "
            "num=parseInt(val);if(!isNaN(num)&&num>=0&&num<totalLEDs){selected."
            "add(num);render();preview();document.querySelector('.led:nth-"
            "child('+(num+1)+')').scrollIntoView({block:'center',behavior:'"
            "smooth'});e.target.value='';}}";
    html += "}";

    html += "let previewTimer=null;";
    html += "function preview(){";
    html += "clearTimeout(previewTimer);";
    html += "previewTimer=setTimeout(()=>{";
    html += "const leds=Array.from(selected).join(',');";
    html += "console.log('Preview LEDs:',leds);";
    html += "fetch('/api/"
            "control',{method:'POST',headers:{'Content-Type':'application/"
            "x-www-form-urlencoded'},body:'action=ledpreview&leds='+leds})";
    html += ".then(r=>r.text()).then(msg=>console.log('Preview:',msg))";
    html += ".catch(e=>console.error('Preview error:',e));";
    html += "},100);";
    html += "}";

    html += "function save(){";
    html += "const leds=Array.from(selected).sort((a,b)=>a-b).join(',');";
    html += "fetch('/api/"
            "control',{method:'POST',headers:{'Content-Type':'application/"
            "x-www-form-urlencoded'},body:'action=ledupdate&id='+cdId+'&leds='+"
            "leds})";
    html += ".then(r=>r.text()).then(()=>{";
    html += "console.log('Sending broadcast:', leds);";
    html += "const msg={type:'led-update',leds:leds};";
    html += "if(window.opener){window.opener.postMessage(msg,'*');}";
    html += "try{const bc=new "
            "BroadcastChannel('led_channel');bc.postMessage(msg);bc.close();}"
            "catch(e){console.log('BC Err',e);}";
    html += "alert('LEDs saved!');window.close();";
    html += "}).catch(e=>alert('Error: '+e));";
    html += "}";

    html += "render();";
    html += "</script></body></html>";

    server.send(200, "text/html", html);
  });

  // 4. Scanner Tool (Full Featured)
  server.on("/scan", HTTP_GET, []() {
    String pinArg = server.arg("pin");
    String codeArg = server.arg("code");
    if (codeArg.length() == 0)
      codeArg = server.arg("barcode");

    // Check authentication server-side
    bool isAuthenticated = (pinArg == web_pin);
    String displayAuth = isAuthenticated ? "display:none !important;" : "";

    String html = "";
    html += "<!DOCTYPE html>";
    html += "<html>";
    html += "<head>";
    html += "    <meta name=\"viewport\" content=\"width=device-width, "
            "initial-scale=1, maximum-scale=1\">";
    html += "    <meta charset=\"UTF-8\">";
    html += "    <title>Scanner</title>";
    html += "    <style>";
    html += "        :root { --bg: #000000; --card: #111111; --accent: "
            "#00ff88; --text: #ffffff; --sub: #666666; --err: #ff4444; }";
    html += "        * { margin: 0; padding: 0; box-sizing: border-box; "
            "-webkit-font-smoothing: antialiased; }";
    html +=
        "        body { background: var(--bg); color: var(--text); "
        "font-family: -apple-system, BlinkMacSystemFont, 'Inter', Roboto, "
        "sans-serif; display: flex; flex-direction: column; align-items: "
        "center; justify-content: center; min-height: 100vh; padding: 20px; }";
    html += "        .container { width: 100%; max-width: 400px; }";
    html += "        h1 { font-size: 24px; font-weight: 600; margin-bottom: "
            "8px; text-align: center; }";
    html += "        p.subtitle { color: var(--sub); font-size: 14px; "
            "text-align: center; margin-bottom: 40px; }";
    html += "        .input-group { position: relative; margin-bottom: 20px; }";
    html += "        input, textarea { width: 100%; background: var(--card); "
            "border: 1px "
            "solid #333; color: white; padding: 16px; font-size: 16px; "
            "border-radius: 12px; outline: none; display: block; font-family: "
            "inherit; }";
    html += "        input { text-align: center; }";
    html += "        textarea { text-align: left; resize: vertical; "
            "min-height: 120px; font-family: monospace; }";
    html +=
        "        input:focus, textarea:focus { border-color: var(--accent); }";
    html += "        button { width: 100%; background: var(--accent); color: "
            "#000; border: none; padding: 16px; font-size: 16px; font-weight: "
            "700; border-radius: 12px; cursor: pointer; }";
    html += "        button:disabled { opacity: 0.5; background: var(--sub); }";
    html += "        .result { margin-bottom: 20px; padding: 20px; "
            "border-radius: 12px; background: var(--card); text-align: center; "
            "display: none; border: 1px solid #333; }";
    html += "        .links { margin-top: 40px; text-align: center; }";
    html += "        a { color: var(--sub); text-decoration: none; font-size: "
            "13px; }";
    html += "    </style>";
    html += "</head>";
    html += "<body>";
    html += "    <div class=\"container\">";
    html += "        <h1>Add " + getModeName() + "</h1>";
    String codeLabel = getCodeLabel();
    if (codeLabel.endsWith(":"))
      codeLabel = codeLabel.substring(0, codeLabel.length() - 1); // remove ':'
    html += "        <p class=\"subtitle\">Enter " + codeLabel + " number</p>";
    html += "        <div id=\"result\"></div>";

    html += "        <form id=\"scanForm\">";
    html += "            <div class=\"input-group\">";
    String placeholder = "Scan or paste " + codeLabel + "s (one per line)";
    html += "                <textarea id=\"barcode\" "
            "placeholder=\"" +
            placeholder +
            "\" required "
            "autocomplete=\"off\" rows=\"5\">" +
            codeArg + "</textarea>";
    html += "            </div>";
    html += "            <button type=\"submit\">Lookup</button>";
    html += "        </form>";

    html += "    </div>"; // This closing div is for the container, it should
                          // remain.
    html += getWebFooter();
    html += "</body></html>";

    // Client-side Logic (Main App Only)
    html += "<script>";
    html += "    const PIN = '" + pinArg + "';";
    html += "    if(PIN) localStorage.setItem('web_pin', PIN);";

    html += "    function processQueue(lines, idx, res, btn) {";
    html += "       if(idx >= lines.length) {";
    html += "           btn.innerText = 'Lookup'; btn.disabled = false;";
    html += "           document.getElementById('barcode').value = '';";
    html += "           return;";
    html += "       }";
    html += "       var code = lines[idx];";
    html +=
        "       btn.innerText = 'Processing ' + (idx + 1) + '/' + lines.length "
        "+ '...';";
    html += "       var itemDiv = document.createElement('div');";
    html += "       itemDiv.style.borderBottom = '1px solid #333'; "
            "itemDiv.style.marginBottom = '10px'; itemDiv.style.paddingBottom "
            "= '10px';";
    html += "       itemDiv.innerHTML = '<div>Scanning: <b>' + code + "
            "'</b>...</div>';";
    html += "       res.prepend(itemDiv);";
    html += "       var xhr = new XMLHttpRequest();";
    html += "       xhr.open('GET', '/api/lookup?barcode=' + "
            "encodeURIComponent(code) + '&pin=' + "
            "localStorage.getItem('web_pin'), true);";
    html += "       xhr.onload = function() {";
    html += "          if(xhr.status === 409) {";
    html += "             var d = JSON.parse(xhr.responseText);";
    html += "             if(confirm(`Duplicate found for "
            "${code}:\\n${d.title}\\nby "
            "${d.artist}\\n\\nAdd copy anyway?`)) {";
    html += "                var x2 = new XMLHttpRequest();";
    html += "                x2.open('GET', '/api/lookup?barcode=' + "
            "encodeURIComponent(code) + '&pin=' + "
            "localStorage.getItem('web_pin') + '&force=true', true);";
    html += "                x2.onload = function(){ disp(itemDiv, x2, code); "
            "setTimeout(function(){ processQueue(lines, idx+1, res, btn); }, "
            "2000); };";
    html += "                x2.onerror = function(){ "
            "itemDiv.innerHTML='Error'; setTimeout(function(){ "
            "processQueue(lines, idx+1, res, btn); }, 2000); "
            "};";
    html += "                x2.send();";
    html += "                return;";
    html += "             } else {";
    html +=
        "                itemDiv.innerHTML = '<div>Code: <b>' + code + "
        "'</b> - <span style=\"color:#888\">Skipped duplicate</span></div>';";
    html += "                setTimeout(function(){ processQueue(lines, idx+1, "
            "res, btn); }, 2000);";
    html += "             }";
    html += "          } else {";
    html += "             disp(itemDiv, xhr, code);";
    html += "             setTimeout(function(){ processQueue(lines, idx+1, "
            "res, btn); }, 2000);";
    html += "          }";
    html += "       };";
    html += "       xhr.onerror = function() { itemDiv.innerHTML='<div "
            "style=\"color:red\">Network Error</div>'; setTimeout(function(){ "
            "processQueue(lines, "
            "idx+1, res, btn); }, 2000); };";
    html += "       xhr.send();";
    html += "    }";
    html += "    function disp(el, x, code) {";
    html += "       if(x.status === 200) {";
    html += "          var d = JSON.parse(x.responseText);";
    html += "          el.innerHTML = '<div>Code: <b>' + code + '</b> - <span "
            "style=\"color:#00ff88\">✅ Added</span> <b>' + d.title + "
            "'</b></div>';";
    html += "       } else { el.innerHTML = '<div style=\"color:#f44\">Code: "
            "<b>' + code + '</b> - ' + x.responseText + '</div>'; }";
    html += "    }";
    html += "    document.getElementById('scanForm').onsubmit = function(e) {";
    html += "        e.preventDefault();";
    html +=
        "        var txt = document.getElementById('barcode').value.trim();";
    html += "        if(!txt) return;";
    html += "        var lines = "
            "txt.split(/[\\n,]+/).map(s=>s.trim()).filter(s=>s.length>0);";
    html += "        if(lines.length===0) return;";
    html += "        var res = document.getElementById('result');";
    html += "        var btn = document.querySelector('button');";
    html += "        btn.disabled = true; res.style.display='block'; "
            "res.innerHTML='';";
    html += "        processQueue(lines, 0, res, btn);";
    html += "    };";
    html +=
        "    document.getElementById('barcode').addEventListener('keydown', "
        "function(e) {";
    html += "       if(e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); "
            "document.getElementById('scanForm').dispatchEvent(new "
            "Event('submit')); }";
    html += "    });";

    // Auto-submit if authorized and code present
    if (isAuthenticated && codeArg.length() > 0) {
      html += "    setTimeout(function(){ var e = new Event('submit'); "
              "document.getElementById('scanForm').dispatchEvent(e); }, 300);";
    }

    html += "</script>";

    // Login Overlay
    html += "<div id='login' "
            "style='position:fixed;top:0;left:0;width:100%;height:100%;"
            "background:#000;z-index:9999;display:flex;flex-direction:column;"
            "align-items:center;justify-content:center;" +
            displayAuth + "'>";
    html += "    <h2>Security Check</h2><br>";
    html += "    <input type='password' id='pin' placeholder='Enter PIN' "
            "style='width:200px;text-align:center;margin-bottom:10px'>";
    html += "    <button onclick='sP()' style='width:200px'>Unlock</button>";
    html += "</div>";

    html += "<script>";
    html += "function sP(){";
    html += "  var p = document.getElementById('pin').value;";
    html += "  localStorage.setItem('web_pin', p);";
    html += "  location.reload();";
    html += "}";
    html += "if(localStorage.getItem('web_pin')) {";
    html += "  var logDiv = document.getElementById('login');";
    html += "  if(logDiv) logDiv.style.display='none';";
    html += "}";
    html += "</script>";

    html += "</body></html>";
    server.send(200, "text/html", html);
  });

  // Manual Cover Link Page
  server.on("/link", HTTP_GET, []() {
    // 1. Security Check (Standard UI)
    if (server.arg("pin") != web_pin) {
      String html =
          "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width, initial-scale=1'><title>Login</title>";
      html +=
          "<style>* { box-sizing: border-box; } "
          "body{background:#000;color:#fff;font-family:sans-serif;display:flex;"
          "justify-content:center;align-items:center;height:100vh;margin:0;}";
      html += ".card{background:#111;padding:30px;border-radius:10px;border:"
              "1px solid #333;text-align:center;width:90%;max-width:320px;}";
      html += "input{width:100%;padding:12px;margin:15px "
              "0;background:#222;border:1px solid "
              "#444;color:#fff;border-radius:5px;text-align:center;font-size:"
              "18px;outline:none;}";
      html += "input:focus{border-color:#00ff88;}";
      html += "button{width:100%;padding:12px;background:#00ff88;color:#000;"
              "border:none;border-radius:5px;font-weight:bold;cursor:pointer;"
              "font-size:16px;text-transform:uppercase;}";
      html += "</style></head><body>";
      html += "<div class='card'>";
      html += "<h2 style='color:#00ff88;margin-top:0'>SECURE LOGIN</h2>";
      html += "<p style='color:#888;font-size:14px'>Enter PIN to access Cover "
              "Tool</p>";
      html += "<form><input type='password' name='pin' placeholder='PIN' "
              "autofocus>";
      html += "<button type='submit'>UNLOCK</button></form>";
      html += "</div></body></html>";
      server.send(401, "text/html", html);
      return;
    }

    String pinArg = server.arg("pin");
    String html = "";
    html += "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, "
            "initial-scale=1, maximum-scale=1'>";
    html += "<meta charset='UTF-8'><title>" + getModeName() + " Art</title>";
    html += "<style>";
    html += ":root { --bg: #000000; --card: #111111; --accent: #00ff88; "
            "--text: #ffffff; --sub: #666666; --err: #ff4444; }";
    html += "* { margin: 0; padding: 0; box-sizing: border-box; "
            "-webkit-font-smoothing: antialiased; }";
    html += "body { background: var(--bg); color: var(--text); font-family: "
            "sans-serif; display: flex; flex-direction: column; align-items: "
            "center; justify-content: center; min-height: 100vh; padding: "
            "20px; }";
    html += ".container { width: 100%; max-width: 400px; }";
    html += "h1 { font-size: 24px; font-weight: 600; margin-bottom: 8px; "
            "text-align: center; color: var(--accent); }";
    html += ".subtitle { color: var(--sub); font-size: 14px; text-align: "
            "center; margin-bottom: 40px; }";
    html += "input { width: 100%; background: var(--card); border: 1px solid "
            "#333; color: white; padding: 16px; font-size: 16px; "
            "border-radius: 12px; outline: none; margin-bottom: 20px; }";
    html += "input:focus { border-color: var(--accent); }";
    html += "button { width: 100%; background: var(--accent); color: #000; "
            "border: none; padding: 16px; font-size: 16px; font-weight: 700; "
            "border-radius: 12px; cursor: pointer; }";
    html += "button:disabled { opacity: 0.5; }";
    html += ".result { margin-top: 30px; padding: 20px; border-radius: 12px; "
            "background: var(--card); text-align: center; border: 1px solid "
            "#222; }";
    html += ".success { border-color: var(--accent); color: var(--accent); }";
    html += ".error { border-color: var(--err); color: var(--err); }";
    html += ".links { margin-top: 40px; text-align: center; }";
    html += "a { color: var(--sub); text-decoration: none; }";
    html += "</style></head><body>";

    html += "<div class='container'>";
    html += "<h1>Update " + getModeName() + " Art</h1>";
    html += "<p class='subtitle'>Enter Unique ID and image URL</p>";
    html += "<div id='result' style='display:none'></div>";
    html += "<form id='linkForm'>";
    html += "<input type='text' id='target_id' placeholder='Unique ID' "
            "required autocomplete='off' style='text-align:center'>";
    html += "<input type='text' id='url' "
            "placeholder='https://example.com/image.jpg' required "
            "autocomplete='off'>";
    html += "<button type='submit'>Update " + getModeName() + " Art</button>";
    html += "</form>";

    html += "</div>";

    html += "<script>";
    html += "    const PIN = '" + pinArg + "';"; // Server-injected PIN
    html += "    if(PIN) localStorage.setItem('web_pin', PIN);";

    html += "    function sP(){ ";
    html += "        var p = document.getElementById('pin').value;";
    html += "        localStorage.setItem('web_pin', p);";
    html += "        var url = new URL(window.location.href);";
    html += "        url.searchParams.set('pin', p);";
    html += "        window.location.href = url.toString();";
    html += "    }";

    html += "document.getElementById('linkForm').onsubmit = function(e) {";
    html += "  e.preventDefault();";
    html += "  var url = document.getElementById('url').value.trim();";
    html += "  var tid = document.getElementById('target_id').value.trim();";
    html += "  var res = document.getElementById('result');";
    html += "  var btn = document.querySelector('button');";
    html += "  if(url.length < 5 || tid.length < 1) return;";
    html += "  btn.innerText = 'Downloading...'; btn.disabled = true; "
            "res.style.display = 'none';";
    html += "  var finalUrl = '/api/setcover?url=' + encodeURIComponent(url);";
    html += "  finalUrl += '&id=' + encodeURIComponent(tid) + '&pin=' + "
            "localStorage.getItem('web_pin');";
    html += "  var xhr = new XMLHttpRequest();";
    html += "  xhr.open('GET', finalUrl, true);";
    html += "  xhr.onload = function() {";
    html += "    btn.innerText = 'Update Cover'; btn.disabled = false; "
            "res.style.display = 'block';";
    html += "    if (xhr.status === 401) { localStorage.removeItem('web_pin'); "
            "location.reload(); return; }";
    html += "    if (xhr.status === 200) {";
    html += "      res.className = 'result success';";
    html += "      var data = JSON.parse(xhr.responseText);";
    html += "      res.innerHTML = `<h2>Cover "
            "Updated</h2><h3>${data.title}</h3><p>${data.artist}</p><p "
            "style='margin-top:8px'>${data.year} • ${data.genre}</p>`;";
    html += "      document.getElementById('url').value = '';";
    html += "    } else {";
    html += "      res.className = 'result error'; res.innerText = "
            "xhr.responseText;";
    html += "    }";
    html += "  };";
    html += "  xhr.send();";
    html += "};";

    html += "    var savedPin = localStorage.getItem('web_pin');";
    html += "    if (!PIN && savedPin) {";
    html += "        if (document.getElementById('login').style.display !== "
            "'none') {";
    html += "             window.location.href = '/link?pin=' + savedPin;";
    html += "        }";
    html += "    }";
    html += "</script>";

    html += getWebFooter();
    html += "</body></html>";

    server.send(200, "text/html", html);
  });

  // API to update cover from URL
  server.on("/api/setcover", HTTP_GET, []() {
    if (server.arg("pin") != web_pin) {
      server.send(401, "text/plain", "Unauthorized: Invalid PIN");
      return;
    }
    String url = server.arg("url");
    // Auto-resize Apple images to optimal 240x240
    if (url.indexOf("100x100") > 0)
      url.replace("100x100", "240x240");
    if (url.indexOf("200x200") > 0)
      url.replace("200x200", "240x240");
    if (url.indexOf("600x600") > 0)
      url.replace("600x600", "240x240");

    String targetID = server.arg("id");

    if (url.length() == 0 || targetID.length() == 0) {
      server.send(400, "text/plain", "Missing URL or ID");
      return;
    }

    if (getItemCount() == 0) {
      server.send(400, "text/plain",
                  "No " + getModeNamePlural() + " in library");
      return;
    }

    int targetIndex = -1;
    // Linear search for ID (RAM only for speed)
    for (int i = 0; i < getItemCount(); i++) {
      if (getItemAtRAM(i).uniqueID == targetID) {
        targetIndex = i;
        break;
      }
    }

    if (targetIndex == -1) {
      server.send(404, "text/plain",
                  getModeName() + " not found (ID mismatch)");
      return;
    }

    // Use unified view
    ItemView item = getItemAt(targetIndex);

    Serial.printf("Manual Cover Update for: %s (Index %d)\n",
                  item.title.c_str(), targetIndex);
    Serial.printf("URL: %s\n", url.c_str());

    // Ensure filename exists
    if (item.coverFile.length() == 0 || item.coverFile == "cover_default.jpg") {
      if (item.uniqueID.length() == 0)
        item.uniqueID = String(millis()) + "_" + String(random(9999));

      String prefix = getUidPrefix();
      item.coverFile = prefix + item.uniqueID + ".jpg";
    }

    // Try download
    if (AppNetworkManager::downloadCoverImage(url, item.coverFile)) {
      // Success
      item.coverUrl = url;

      // Update Target Item
      setItem(targetIndex, item);
      saveLibrary();

      // Only refresh screen if we updated the CURRENTLY visible item
      if (targetIndex == getCurrentItemIndex()) {
        lvgl_port_lock(-1);
        load_and_show_cover(item.coverFile);
        lvgl_port_unlock();
      }

      StaticJsonDocument<256> doc;
      doc["title"] = item.title;
      doc["artist"] = item.artistOrAuthor;
      doc["year"] = item.year;
      doc["genre"] = item.genre;
      String json;
      serializeJson(doc, json);
      server.send(200, "application/json", json);
    } else {
      server.send(500, "text/plain", "Download Failed");
    }
  });

  // 5. Metadata Lookup API (Updated with Duplicate Check)
  server.on("/api/lookup", HTTP_GET, []() {
    if (server.arg("pin") != web_pin) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    String code = server.arg("barcode");
    bool force = (server.arg("force") == "true");

    // 1. Check for duplicate (unless forced)
    if (!force) {
      for (int i = 0; i < getItemCount(); i++) {
        ItemView item = getItemAtRAM(i);
        // Compare barcodes. Logic: if item.barcode is valid and matches
        // Also check if barcode length is reasonable
        if (item.codecOrIsbn == code && code.length() > 3) {
          // Duplicate found!
          StaticJsonDocument<256> doc;
          doc["title"] = item.title;
          doc["artist"] = item.artistOrAuthor;
          String json;
          serializeJson(doc, json);
          server.send(409, "application/json", json); // 409 Conflict
          return;
        }
      }
    }

    // 2. Lookup & Add
    ItemView out;
    if (fetchModeMetadata(code, out)) {
      // Assign Unique ID
      // Assign Unique ID if not already set by fetcher
      if (out.uniqueID.length() == 0) {
        out.uniqueID = String(millis()) + "_" + String(random(9999));
      }

      // Assign LED (If not already set by metadata fetcher)
      if (out.ledIndices.empty()) {
        out.ledIndices.push_back(getNextLedIndex());
      }

      // Add
      addItemToLibrary(out);
      saveLibrary();

      // Select & Show
      setCurrentItemIndex(getItemCount() - 1);
      lvgl_port_lock(-1);
      update_item_display();
      lvgl_port_unlock();

      StaticJsonDocument<256> doc;
      doc["title"] = out.title;
      doc["artist"] = out.artistOrAuthor;
      doc["year"] = out.year;
      doc["genre"] = out.genre;
      String json;
      serializeJson(doc, json);
      server.send(200, "application/json", json);
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  // 6. Remote Browser (Full Featured)
  server.on("/browse", HTTP_GET, []() {
    String pinArg = server.arg("pin");
    bool isAuthenticated = (pinArg == web_pin);
    String displayAuth = isAuthenticated ? "display:none !important;" : "";

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", ""); // Start chunked transfer

    // 1. Send HEAD and STYLES
    String chunk = "<!DOCTYPE html><html><head>";
    chunk +=
        "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    chunk += "<title>Remote " + getModeName() + " Control</title>";
    chunk += "<style>";
    chunk += "* { box-sizing: border-box; }";
    chunk += "body { background: #000; color: #fff; font-family: sans-serif; "
             "padding: 20px; padding-bottom: 80px; max-width: 600px; margin: 0 "
             "auto; }";
    chunk +=
        ".cd { background: #111; border: 1px solid #333; padding: 15px; "
        "margin-bottom: 10px; border-radius: 8px; display: flex; align-items: "
        "center; justify-content: space-between; gap: 10px; }";
    chunk += ".cd-info { flex: 1; min-width: 0; padding-right: 10px; overflow: "
             "hidden; }";
    chunk += ".cd:active { background: #222; }";
    chunk +=
        ".cd h3 { margin: 0; font-size: 16px; color: #00ff88; white-space: "
        "nowrap; overflow: hidden; text-overflow: ellipsis; }";
    chunk +=
        ".cd p { margin: 4px 0 0; color: #888; font-size: 14px; white-space: "
        "nowrap; overflow: hidden; text-overflow: ellipsis; }";

    // Buttons
    chunk += ".btn-group { display: flex; align-items: center; gap: 8px; "
             "flex-shrink: 0; }";
    chunk += ".btn-go { padding: 10px 18px; background: #00ff88; color: #000; "
             "border: none; border-radius: 6px; font-weight: bold; cursor: "
             "pointer; text-transform: uppercase; font-size: 14px; }";
    chunk +=
        ".btn-edit { padding: 10px 18px; background: #ff8800; color: #fff; "
        "border: none; border-radius: 6px; font-weight: bold; cursor: "
        "pointer; text-transform: uppercase; font-size: 14px; }";
    chunk += ".btn-edit:hover { background: #ff9922; }";

    chunk += "input { width: 100%; padding: 12px; margin-bottom: 20px; "
             "background: #222; border: 1px solid #444; color: white; "
             "border-radius: 8px; font-size: 16px; outline: none; }";
    chunk += "input:focus { border-color: #00ff88; }";

    // Modal CSS
    chunk +=
        ".modal { display:none; position:fixed; top:0; left:0; width:100%; "
        "height:100%; background:rgba(0,0,0,0.9); z-index:1000; padding:20px; "
        "box-sizing:border-box; overflow-y:auto; }";
    chunk += ".modal-content { background:#111; padding:20px; border:1px solid "
             "#333; border-radius:10px; max-width:500px; margin:20px auto; }";
    chunk += "label { display:block; margin-bottom:5px; color:#888; "
             "font-size:12px; }";

    chunk += "</style></head><body>";
    server.sendContent(chunk);

    // 2. Send BODY Content (Headers & Filters)
    chunk = "<h1>Library Remote</h1>";
    chunk += "<input type='text' id='search' placeholder='Search...' "
             "onkeyup='filter()'>";

    // Filter Controls Container
    chunk += "<div style='display:flex; gap:10px; margin-bottom:20px; "
             "flex-wrap:wrap;'>";

    // Genre Filter
    chunk += "<select id='filterGenre' onchange='filter()' "
             "style='padding:10px; background:#222; color:white; border:1px "
             "solid #444; border-radius:6px; height:44px;'>";
    chunk += "<option value=''>All Genres</option>";
    server.sendContent(chunk);

    // Build unique genre list (Streamed)
    std::vector<String> web_genres;
    int totalItems = getItemCount();
    for (int i = 0; i < totalItems; i++) {
      ItemView item = getItemAtRAM(i);
      bool found = false;
      for (const auto &g : web_genres)
        if (g.equalsIgnoreCase(item.genre))
          found = true;
      if (!found && item.genre.length() > 0) {
        web_genres.push_back(item.genre);
        String safeG = escapeHTML(item.genre);
        String opt = "<option value=\"" + safeG + "\">" + safeG + "</option>";
        server.sendContent(opt);
      }
    }
    server.sendContent("</select>");

    // Decade & Favorites Filters
    chunk = "<select id='filterDecade' onchange='filter()' "
            "style='padding:10px; background:#222; color:white; border:1px "
            "solid #444; border-radius:6px; height:44px;'>";
    chunk += "<option value=''>All Decades</option>";
    chunk += "<option value='60'>60s</option><option "
             "value='70'>70s</option><option value='80'>80s</option>";
    chunk += "<option value='90'>90s</option><option "
             "value='00'>2000s</option><option "
             "value='10'>2010s</option><option value='20'>2020s</option>";
    chunk += "</select>";

    chunk += "<label style='display:flex; align-items:center; gap:8px; "
             "padding:10px; background:#222; border:1px solid #444; "
             "border-radius:6px; height:44px;'>";
    chunk += "<input type='checkbox' id='filterFav' onchange='filter()' "
             "style='width:auto; margin:0;'>";
    chunk += "<span>&#9733; Favorites Only</span></label>";
    chunk += "</div>";

    chunk += "<div id='list'></div>";

    // EDIT MODAL
    chunk += "<div id='edit-modal' class='modal'><div "
             "class='modal-content'><h2>Edit Details</h2>";
    chunk += "<input type='hidden' id='edit-id'>";
    chunk += "<label>Title</label><input id='edit-title'>";
    String artistLabel = getArtistOrAuthorLabel();
    chunk += "<label>" + artistLabel + "</label><input id='edit-artist'>";
    chunk += "<label>Genre</label><input id='edit-genre'>";
    chunk += "<label>Year</label><input id='edit-year'>";
    chunk += "<label>Unique ID</label><input id='edit-uniqueID'>";
    chunk += "<label>LED Index <span "
             "style='font-size:11px;opacity:0.6'>(comma-separated, e.g., "
             "10,11,12)</span></label>";
    chunk += "<div style='display:flex;gap:8px;align-items:stretch'>";
    chunk += "<input id='edit-ledIndex' type='text' style='flex:1;padding:10px "
             "12px;font-size:14px'>";
    chunk +=
        "<button onclick='openLEDSelector()' type='button' style='padding:10px "
        "16px;background:#00ff88;color:#000;border:none;border-radius:6px;font-"
        "weight:bold;cursor:pointer;white-space:nowrap'>🎯 SELECT "
        "LEDs</button>";
    chunk += "</div>";
    String barcodeLabel = getCodeLabel();
    if (barcodeLabel.endsWith(":"))
      barcodeLabel =
          barcodeLabel.substring(0, barcodeLabel.length() - 1); // remove ':'
    chunk += "<label>" + barcodeLabel + "</label><input id='edit-barcode'>";
    chunk += "<label>Notes</label><input id='edit-notes'>";
    chunk += "<label style='display:flex;align-items:center;gap:10px'><input "
             "type='checkbox' id='edit-fav' style='width:auto;margin:0'> "
             "Favorite</label>";
    chunk += "<div style='display:flex; gap:10px; margin-top:20px;'>";
    chunk += "<button onclick='saveEdit()' style='flex:1; padding:12px; "
             "background:#00ff88; color:#000; border:none; border-radius:6px; "
             "font-weight:bold; cursor:pointer;'>SAVE CHANGES</button>";
    chunk += "<button "
             "onclick=\"document.getElementById('edit-modal').style.display='"
             "none'\" style='flex:1; padding:12px; background:#444; "
             "color:#fff; border:none; border-radius:6px; font-weight:bold; "
             "cursor:pointer;'>CANCEL</button>";
    chunk += "</div>";
    chunk += "</div></div>";

    server.sendContent(chunk);

    // 3. Send SCRIPT - Start
    chunk = "<script>";
    chunk += "window.onerror = function(msg, url, line, col, error) { "
             "alert('JS Error: ' + msg); return false; };";
    chunk += "const PIN = '" + escapeJSON(pinArg) + "';"; // ESCAPED PIN
    chunk += "if(PIN) localStorage.setItem('web_pin', PIN);";
    chunk += "const library = [";
    server.sendContent(chunk);

    // 4. Send LIBRARY JSON Data (Streamed item by item)
    int totalCount = getItemCount();
    for (int i = 0; i < totalCount; i++) {
      ItemView iv = getItemAtRAM(i);
      String item = "{";
      item += "\"id\":" + String(i) + ",";
      item += "\"title\":\"" + escapeJSON(iv.title) + "\",";
      item += "\"artist\":\"" + escapeJSON(iv.artistOrAuthor) + "\",";
      item += "\"year\":" + String(iv.year) + ",";
      item += "\"genre\":\"" + escapeJSON(iv.genre) + "\",";
      item += "\"uniqueID\":\"" + escapeJSON(iv.uniqueID) + "\",";
      String lArr = "[";
      for (size_t k = 0; k < iv.ledIndices.size(); k++) {
        lArr += String(iv.ledIndices[k]);
        if (k < iv.ledIndices.size() - 1)
          lArr += ",";
      }
      lArr += "]";
      item += "\"ledIndices\":" + lArr + ",";

      // Legacy compatibility: ledIndex = first element or -1
      int lFirst = iv.ledIndices.empty() ? -1 : iv.ledIndices[0];
      item += "\"ledIndex\":" + String(lFirst) + ",";
      item += "\"barcode\":\"" + escapeJSON(iv.codecOrIsbn) + "\",";
      item += "\"notes\":\"" + escapeJSON(iv.notes) + "\",";
      item += "\"favorite\":" + String(iv.favorite ? "true" : "false");
      item += "},";
      server.sendContent(item);
      if (i % 10 == 0)
        yield();
    }
    server.sendContent("];");

    // 5. Send Rest of SCRIPT (Render logic)
    chunk = "const list = document.getElementById('list');";
    chunk += "function render(items) {";
    chunk += "  list.innerHTML = '';";
    chunk += "  items.forEach(cd => {";
    chunk += "    const div = document.createElement('div');";
    chunk += "    div.className = 'cd';";
    chunk +=
        "    const safeT = cd.title.replace(/\"/g, '&quot;');"; // Extra safety
    chunk += "    div.innerHTML = `<div "
             "class='cd-info'><h3>${cd.title}</h3><p>${cd.artist}</p></div>";
    chunk += "                     <div class='btn-group'>";
    chunk += "                       <button class='btn-edit' "
             "onclick='event.stopPropagation(); edit(${cd.id})'>EDIT</button>";
    chunk += "                       <button class='btn-go' "
             "onclick='event.stopPropagation(); select(${cd.id})'>GO</button>";
    chunk += "                     </div>`;";
    chunk += "    list.appendChild(div);";
    chunk += "  });";
    chunk += "}";
    chunk += "render(library);"; // Initial Render

    // EDIT & SAVE LOGIC
    chunk += "function edit(id) { ";
    chunk += "select(id); "; // Select the CD on main UI first!
    chunk += "const cd = library.find(c=>c.id==id); "
             "if(!cd)return; ";
    chunk += "document.getElementById('edit-id').value=id; "
             "document.getElementById('edit-title').value=cd.title; ";
    chunk += "document.getElementById('edit-artist').value=cd.artist; "
             "document.getElementById('edit-genre').value=cd.genre; ";
    chunk += "document.getElementById('edit-year').value=cd.year; ";
    chunk += "document.getElementById('edit-uniqueID').value=cd.uniqueID; ";
    chunk += "document.getElementById('edit-ledIndex').value=cd.ledIndices?cd."
             "ledIndices.join(','):cd.ledIndex; "; // Support both formats
    chunk += "document.getElementById('edit-barcode').value=cd.barcode; ";
    chunk += "document.getElementById('edit-notes').value=cd.notes; ";
    chunk += "document.getElementById('edit-fav').checked=cd.favorite; ";
    chunk += "document.getElementById('edit-modal').style.display='block'; ";
    chunk += "setTimeout(()=>{const "
             "f=document.getElementById('edit-ledIndex');if(f){f.oninput="
             "previewEditLEDs;previewEditLEDs();}},100); ";
    chunk += "}";

    // LED Selector Integration
    chunk += "let ledSelectorWindow=null;";
    chunk += "function openLEDSelector(){";
    chunk += "const id=document.getElementById('edit-id').value;";
    chunk += "if(!id && id!==0){alert('Please save the CD first');return;}";
    chunk += "ledSelectorWindow=window.open('/"
             "led-select?cd='+id,'LEDSelector','width=800,height=600');";
    chunk += "}";

    // Listen for LED selections from child window
    chunk += "function handleLedUpdate(leds){";
    chunk += "console.log('Received LED update:', leds);";
    chunk += "const el=document.getElementById('edit-ledIndex');";
    chunk += "if(el){el.value=leds; previewEditLEDs();}";
    chunk += "const id=document.getElementById('edit-id').value;";
    chunk += "const cd=library.find(c=>c.id==id);";
    chunk += "if(cd){cd.ledIndices=leds.split(',').map(n=>parseInt(n));}";
    chunk += "}";

    chunk += "window.addEventListener('message',function(e){";
    chunk += "if(e.data && "
             "e.data.type==='led-update'){handleLedUpdate(e.data.leds);}";
    chunk += "});";

    chunk +=
        "try{const bc=new BroadcastChannel('led_channel');bc.onmessage=(e)=>{";
    chunk += "if(e.data && "
             "e.data.type==='led-update'){handleLedUpdate(e.data.leds);}";
    chunk += "};}catch(e){console.log('BC Setup Error',e);}";

    // Real-time LED preview for edit field
    chunk += "let previewEditTimer=null;";
    chunk += "function previewEditLEDs(){";
    chunk += "clearTimeout(previewEditTimer);";
    chunk += "previewEditTimer=setTimeout(()=>{";
    chunk += "const leds=document.getElementById('edit-ledIndex').value;";
    chunk += "if(!leds)return;";
    chunk += "fetch('/api/"
             "control',{method:'POST',headers:{'Content-Type':'application/"
             "x-www-form-urlencoded'},body:'action=ledpreview&leds='+leds})";
    chunk += ".catch(e=>console.error('Preview error:',e));";
    chunk += "},200);";
    chunk += "}";

    // Attach to LED Index field
    chunk += "document.addEventListener('DOMContentLoaded',function(){";
    chunk += "const ledField=document.getElementById('edit-ledIndex');";
    chunk +=
        "if(ledField){ledField.addEventListener('input',previewEditLEDs);}";
    chunk += "});";
    chunk += "";

    chunk += "function saveEdit() {";
    chunk += "  var id=document.getElementById('edit-id').value; var "
             "t=document.getElementById('edit-title').value; ";
    chunk += "  var a=document.getElementById('edit-artist').value; var "
             "g=document.getElementById('edit-genre').value; ";
    chunk += "  var y=document.getElementById('edit-year').value; var "
             "f=document.getElementById('edit-fav').checked; ";
    chunk += "  var uid=document.getElementById('edit-uniqueID').value; ";
    chunk += "  var li=document.getElementById('edit-ledIndex').value; ";
    chunk += "  var bc=document.getElementById('edit-barcode').value; ";
    chunk += "  var n=document.getElementById('edit-notes').value; ";

    chunk +=
        "  doAction('edit', "
        "'&id='+id+'&title='+encodeURIComponent(t)+'&artist='+"
        "encodeURIComponent(a)+'&genre='+encodeURIComponent(g)+'&year='+y+"
        "'&fav='+f+'&uniqueID='+encodeURIComponent(uid)+'&ledIndex='+li+'&"
        "barcode='+encodeURIComponent(bc)+'&notes='+encodeURIComponent(n)); ";

    chunk += "  var cd = library.find(c=>c.id==id); if(cd){ cd.title=t; "
             "cd.artist=a; cd.genre=g; cd.year=y; cd.favorite=f; "
             "cd.uniqueID=uid; cd.ledIndex=li; cd.barcode=bc; cd.notes=n; "
             "render(library); } ";
    chunk += "  document.getElementById('edit-modal').style.display='none'; }";

    chunk += "function filter() {";
    chunk +=
        "  const q = document.getElementById('search').value.toLowerCase();";
    chunk += "  const genreFilter = "
             "document.getElementById('filterGenre').value.toLowerCase();";
    chunk +=
        "  const decadeFilter = document.getElementById('filterDecade').value;";
    chunk +=
        "  const favFilter = document.getElementById('filterFav').checked;";
    chunk += "  const filtered = library.filter(cd => {";
    chunk += "    if (q && !cd.title.toLowerCase().includes(q) && "
             "!cd.artist.toLowerCase().includes(q)) return false;";
    chunk += "    if (genreFilter && cd.genre.toLowerCase() !== genreFilter) "
             "return false;";
    chunk += "    if (decadeFilter) {";
    chunk += "      const cdDecade = Math.floor(cd.year / 10) * 10;";
    chunk += "      let targetDecade = parseInt(decadeFilter);";
    chunk += "      if (targetDecade < 50) targetDecade += 2000; else "
             "targetDecade += 1900;";
    chunk += "      if (cdDecade !== targetDecade) return false;";
    chunk += "    }";
    chunk += "    if (favFilter && !cd.favorite) return false;";
    chunk += "    return true;";
    chunk += "  });";
    chunk += "  render(filtered);";
    chunk += "  const hasFilters = genreFilter || decadeFilter || favFilter;";

    // SECURE ACTION CALL
    chunk += "  if (hasFilters) {";
    chunk += "    const genre = document.getElementById('filterGenre').value;";
    chunk +=
        "    doAction('applyfilter', '&genre=' + encodeURIComponent(genre) + "
        "'&decade=' + decadeFilter + '&favorites=' + favFilter);";
    chunk += "  } else {";
    chunk += "    doAction('clearfilter');";
    chunk += "  }";
    chunk += "}";

    chunk += "function select(id) { doAction('select', '&id='+id); }";

    // SECURE doAction
    chunk += "function doAction(act, params='') {";
    chunk += "  var p = localStorage.getItem('web_pin');";
    chunk += "  if (!p) return;";
    chunk +=
        "  fetch('/api/control?pin=' + encodeURIComponent(p) + '&action=' + "
        "encodeURIComponent(act) + params).then(r=>console.log(r.status));";
    chunk += "}";

    chunk += "</script>";

    // Login Overlay
    chunk += "<div id='login' "
             "style='position:fixed;top:0;left:0;width:100%;height:100%;"
             "background:#000;z-index:9999;display:flex;align-items:center;"
             "justify-content:center;" +
             displayAuth + "'>";
    chunk += "<div "
             "style='background:#111;padding:30px;border-radius:10px;border:"
             "1px solid #333;text-align:center;width:90%;max-width:320px;'>";
    chunk += "<h2 style='color:#00ff88;margin-top:0'>SECURE LOGIN</h2>";
    chunk +=
        "<p style='color:#888;font-size:14px'>Enter PIN to access Library</p>";
    chunk += "<input type='password' id='pin' placeholder='PIN' "
             "style='width:100%;padding:12px;margin:15px "
             "0;background:#222;border:1px solid "
             "#444;color:#fff;border-radius:5px;text-align:center;font-size:"
             "18px;outline:none;' autofocus>";
    chunk += "<button onclick='sP()' "
             "style='width:100%;padding:12px;background:#00ff88;color:#000;"
             "border:none;border-radius:5px;font-weight:bold;cursor:pointer;"
             "font-size:16px;text-transform:uppercase;'>UNLOCK</button>";
    chunk += "</div></div>";

    chunk +=
        "<script>function sP(){ var p = document.getElementById('pin').value; "
        "localStorage.setItem('web_pin', p); location.reload(); }";
    chunk += "if(localStorage.getItem('web_pin')) { var logDiv = "
             "document.getElementById('login'); if(logDiv) "
             "logDiv.style.display='none'; }</script>";

    chunk += getWebFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    server.sendContent(""); // End chunked transfer
  });

  // 4. Backup & Restore
  server.on("/backup", HTTP_GET, []() {
    if (server.arg("pin") != web_pin) {
      String html =
          "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width, initial-scale=1'><title>Login</title>";
      html +=
          "<style>* { box-sizing: border-box; } "
          "body{background:#000;color:#fff;font-family:sans-serif;display:flex;"
          "justify-content:center;align-items:center;height:100vh;margin:0;}";
      html += ".card{background:#111;padding:30px;border-radius:10px;border:"
              "1px solid #333;text-align:center;width:90%;max-width:320px;}";
      html += "input{width:100%;padding:12px;margin:15px "
              "0;background:#222;border:1px solid "
              "#444;color:#fff;border-radius:5px;text-align:center;font-size:"
              "18px;outline:none;}";
      html += "input:focus{border-color:#00ff88;} "
              "button{width:100%;padding:12px;background:#00ff88;color:#000;"
              "border:none;border-radius:5px;font-weight:bold;cursor:pointer;"
              "font-size:16px;text-transform:uppercase;}";
      html +=
          "</style></head><body><div class='card'><h2 "
          "style='color:#00ff88;margin-top:0'>SECURE LOGIN</h2><p "
          "style='color:#888;font-size:14px'>Enter PIN to access backups</p>";
      html += "<form><input type='password' name='pin' placeholder='PIN' "
              "autofocus><button "
              "type='submit'>UNLOCK</button></form></div></body></html>";
      server.send(401, "text/html", html);
      return;
    }

    String html =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Backup & "
        "Restore</title><meta name='viewport' content='width=device-width, "
        "initial-scale=1'></head>";
    html += "<body style='font-family:sans-serif; background:#000; color:#fff; "
            "padding:20px; max-width:600px; margin:0 auto;'>";
    html += "<h1 style='color:#00ff88'>Backup & Restore</h1>";
    String libFile = getLibraryFileName();
    html +=
        "<p>Manage your library database (<code>" + libFile + "</code>).</p>";

    // Export (JSONL)
    html += "<div style='background:#111; padding:20px; border:1px solid #333; "
            "border-radius:10px; margin-bottom:20px;'>";
    html += "<h2>⬇️ Export Library</h2><p style='color:#ccc'>Download all data "
            "as a single .jsonl file (Line-delimited JSON).</p>";
    html += "<a href='/api/export_backup?pin=" + String(web_pin) +
            "' download='library_backup.jsonl'><button style='padding:12px "
            "25px; background:#00ff88; color:#000; border:none; "
            "border-radius:5px; font-weight:bold; cursor:pointer;'>EXPORT "
            "FULL BACKUP</button></a>";
    html += "</div>";

    // Import (JSONL)
    html += "<div style='background:#111; padding:20px; border:1px solid #333; "
            "border-radius:10px;'>";
    html += "<h2>⬆️ Import Backup</h2><p style='color:#ffaa00'>⚠️ Warning: "
            "Adds/Overwrites items from the backup file.</p>";
    html += "<form method='POST' action='/api/import_backup?pin=" +
            String(web_pin) + "' enctype='multipart/form-data'>";
    html +=
        "<input type='file' name='data' accept='.jsonl' onchange='var "
        "b=document.getElementById(\"btnR\"); b.disabled=!this.files.length; "
        "b.style.opacity=this.files.length?1:0.5; "
        "b.style.cursor=this.files.length?\"pointer\":\"not-allowed\";' "
        "style='margin-bottom:15px; width:100%; padding:10px; background:#222; "
        "border:1px solid #444; border-radius:5px;'><br>";
    html +=
        "<input type='submit' id='btnR' value='RESTORE FROM BACKUP' disabled "
        "style='padding:12px 25px; background:#ff4444; color:#fff; "
        "border:none; border-radius:5px; font-weight:bold; "
        "cursor:not-allowed; opacity:0.5;'>";
    html += "</form></div>";

    html += "<div style='margin-top: 40px; border-top: 1px solid #333; "
            "padding-top: 20px;'>";
    html += "<h3 style='text-align:center; color:#fff; font-size: 14px; "
            "margin-bottom: 15px; text-transform: uppercase; letter-spacing: "
            "1px;'>Navigation</h3>";
    html += "<div style='display: grid; grid-template-columns: repeat(5, 1fr); "
            "gap: 10px;'>";
    html += "<button onclick=\"location.href='/browse'\" style='font-size: "
            "13px; padding: 10px;'>&#128241; Remote</button>";
    html += "<button onclick=\"location.href='/scan'\" style='font-size: 13px; "
            "padding: 10px;'>&#128247; Scanner</button>";
    html += "<button onclick=\"location.href='/link'\" style='font-size: 13px; "
            "padding: 10px;'>&#128444; Covers</button>";
    html += "<button onclick=\"location.href='/backup'\" style='font-size: "
            "13px; padding: 10px;'>&#128190; Backup</button>";
    html += "<button onclick=\"location.href='/manual'\" style='font-size: "
            "13px; padding: 10px;'>&#128214; Manuals</button>";
    html += "</div></div>";
    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  // 5. Export Backup (JSONL)
  server.on("/api/export_backup", HTTP_GET, []() {
    if (server.arg("pin") != web_pin)
      return server.send(401, "text/plain", "Unauthorized");

    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"library_backup.jsonl\"");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/ndjson", ""); // Newline Delimited JSON

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    // Export CDs
    for (const auto &item : cdLibrary) {
      CD fullCD;
      if (Storage.loadCDDetail(item.uniqueID.c_str(), fullCD)) {
        DynamicJsonDocument doc(2048);
        doc["type"] = "cd";
        JsonObject data = doc.createNestedObject("data");
        data["title"] = fullCD.title.c_str();
        data["artist"] = fullCD.artist.c_str();
        data["genre"] = fullCD.genre.c_str();
        data["year"] = fullCD.year;
        data["uniqueID"] = fullCD.uniqueID.c_str();
        data["coverFile"] = fullCD.coverFile.c_str();
        data["favorite"] = fullCD.favorite;
        data["notes"] = fullCD.notes.c_str();
        data["barcode"] = fullCD.barcode.c_str();
        data["trackCount"] = fullCD.trackCount;
        data["totalDurationMs"] = fullCD.totalDurationMs;
        data["releaseMbid"] = fullCD.releaseMbid.c_str();

        // Tracks are stored separately and not included in basic backup

        // Export CD
        String line;
        serializeJson(doc, line);
        server.sendContent(line + "\n");

        // NEW: Export Tracklist if available
        if (fullCD.releaseMbid.length() > 0) {
          TrackList *tl = Storage.loadTracklist(fullCD.releaseMbid.c_str());
          if (tl) {
            DynamicJsonDocument tlDoc(16384); // Larger buffer for tracklist
            tlDoc["type"] = "tracklist";
            tlDoc["mbid"] = fullCD.releaseMbid;
            JsonObject tlData = tlDoc.createNestedObject("data");
            tlData["cdTitle"] = tl->cdTitle.c_str();
            tlData["cdArtist"] = tl->cdArtist.c_str();
            tlData["fetchedAt"] = tl->fetchedAt.c_str();
            JsonArray tracks = tlData.createNestedArray("tracks");
            for (const auto &t : tl->tracks) {
              JsonObject tObj = tracks.createNestedObject();
              tObj["trackNo"] = t.trackNo;
              tObj["title"] = t.title.c_str();
              tObj["durationMs"] = t.durationMs;
              tObj["recordingMbid"] = t.recordingMbid.c_str();
              tObj["isFav"] = t.isFavoriteTrack;

              JsonObject lyr = tObj.createNestedObject("lyrics");
              lyr["status"] = t.lyrics.status.c_str();
              lyr["path"] = t.lyrics.path.c_str();
              lyr["fetchedAt"] = t.lyrics.fetchedAt.c_str();
              lyr["lang"] = t.lyrics.lang.c_str();
            }
            String tlLine;
            serializeJson(tlDoc, tlLine);
            server.sendContent(tlLine + "\n");
            delete tl;
          }
        }
      }
    }

    // Export Books
    for (const auto &item : bookLibrary) {
      Book fullBook;
      if (Storage.loadBookDetail(item.uniqueID.c_str(), fullBook)) {
        DynamicJsonDocument doc(2048);
        doc["type"] = "book";
        JsonObject data = doc.createNestedObject("data");
        data["title"] = fullBook.title.c_str();
        data["author"] = fullBook.author.c_str();
        data["genre"] = fullBook.genre.c_str();
        data["year"] = fullBook.year;
        data["uniqueID"] = fullBook.uniqueID.c_str();
        data["coverFile"] = fullBook.coverFile.c_str();
        data["favorite"] = fullBook.favorite;
        data["notes"] = fullBook.notes.c_str();
        data["isbn"] = fullBook.isbn.c_str();
        data["pageCount"] = fullBook.pageCount;
        data["publisher"] = fullBook.publisher.c_str();

        String line;
        serializeJson(doc, line);
        server.sendContent(line + "\n");
      }
    }

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    server.sendContent("");
  });

  // 6. Import Backup (JSONL)
  server.on(
      "/api/import_backup", HTTP_POST,
      []() {
        // 1. Process the saved file
        if (sdExpander)
          sdExpander->digitalWrite(SD_CS, LOW);
        File file = SD.open("/restore.jsonl", FILE_READ);
        if (!file) {
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
          return server.send(500, "text/plain", "Restore file missing");
        }

        int importCount = 0;
        int tracklistCount = 0;

        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() == 0)
            continue;

          DynamicJsonDocument doc(
              16384); // Unified buffer for both small and large records
          DeserializationError error = deserializeJson(doc, line);
          if (!error) {
            String type = doc["type"].as<String>();
            JsonObject data = doc["data"];

            if (type == "cd") {
              CD cd;
              cd.title = (const char *)(data["title"] | "");
              cd.artist = (const char *)(data["artist"] | "");
              cd.genre = (const char *)(data["genre"] | "");
              cd.year = data["year"] | 0;
              cd.uniqueID = (const char *)(data["uniqueID"] | "");
              cd.coverFile = (const char *)(data["coverFile"] | "");
              cd.favorite = data["favorite"] | false;
              cd.notes = (const char *)(data["notes"] | "");
              cd.barcode = (const char *)(data["barcode"] | "");
              cd.trackCount = data["trackCount"] | 0;
              cd.totalDurationMs = data["totalDurationMs"] | 0;
              cd.releaseMbid = (const char *)(data["releaseMbid"] | "");

              Storage.saveCD(cd);
              importCount++;
            } else if (type == "tracklist") {
              String mbid = doc["mbid"] | "";
              if (mbid.length() > 0) {
                TrackList tl;
                tl.releaseMbid = mbid.c_str();
                tl.cdTitle = (const char *)(data["cdTitle"] | "");
                tl.cdArtist = (const char *)(data["cdArtist"] | "");
                tl.fetchedAt = (const char *)(data["fetchedAt"] | "");
                JsonArray tracks = data["tracks"];
                for (JsonObject tObj : tracks) {
                  Track t;
                  t.trackNo = tObj["trackNo"] | 0;
                  t.title = (const char *)(tObj["title"] | "");
                  t.durationMs = tObj["durationMs"] | 0;
                  t.recordingMbid = (const char *)(tObj["recordingMbid"] | "");
                  t.isFavoriteTrack = tObj["isFav"] | false;

                  JsonObject lyr = tObj["lyrics"];
                  t.lyrics.status = (const char *)(lyr["status"] | "unchecked");
                  t.lyrics.path = (const char *)(lyr["path"] | "");
                  t.lyrics.fetchedAt = (const char *)(lyr["fetchedAt"] | "");
                  t.lyrics.lang = (const char *)(lyr["lang"] | "");

                  tl.tracks.push_back(t);
                }
                Storage.saveTracklist(mbid.c_str(), &tl);
                tracklistCount++;
              }
            } else if (type == "book") {
              Book book;
              book.title = data["title"] | "";
              book.author = data["author"] | "";
              book.genre = data["genre"] | "";
              book.year = data["year"] | 0;
              book.uniqueID = data["uniqueID"] | "";
              book.coverFile = data["coverFile"] | "";
              book.favorite = data["favorite"] | false;
              book.notes = data["notes"] | "";
              book.isbn = data["isbn"] | "";
              book.pageCount = data["pageCount"] | 0;
              book.publisher = data["publisher"] | "";
              Storage.saveBook(book);
              importCount++;
            }
          }
        }
        file.close();
        SD.remove("/restore.jsonl");
        if (sdExpander)
          sdExpander->digitalWrite(SD_CS, HIGH);

        server.send(200, "text/html",
                    "<html><head><meta http-equiv='refresh' "
                    "content='10;url=/backup'></head>"
                    "<body "
                    "style='background:#000;color:#00ff88;font-family:sans-"
                    "serif;text-align:center;'>"
                    "<h1>Import Success!</h1><p>Imported " +
                        String(importCount) + " items and " +
                        String(tracklistCount) +
                        " tracklists.</p>"
                        "<p>Device is restarting to apply "
                        "changes...</p></body></html>");
        delay(2000);
        ESP.restart();
      },
      []() {
        // 2. Upload Handler
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          if (server.arg("pin") != web_pin)
            return;
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, LOW);
          if (SD.exists("/restore.jsonl"))
            SD.remove("/restore.jsonl");
          uploadFile = SD.open("/restore.jsonl", FILE_WRITE);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (uploadFile)
            uploadFile.write(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
          if (uploadFile) {
            uploadFile.flush();
            uploadFile.close();
          }
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          if (uploadFile)
            uploadFile.close();
          if (SD.exists("/restore.jsonl"))
            SD.remove("/restore.jsonl");
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
        }
      });

  // 3. User Manual
  server.on("/manual", HTTP_GET, []() {
    if (server.arg("pin") != web_pin) {
      String html =
          "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width, initial-scale=1'><title>Login</title>";
      html +=
          "<style>* { box-sizing: border-box; } "
          "body{background:#000;color:#fff;font-family:sans-serif;display:flex;"
          "justify-content:center;align-items:center;height:100vh;margin:0;}";
      html += ".card{background:#111;padding:30px;border-radius:10px;border:"
              "1px solid #333;text-align:center;width:90%;max-width:320px;}";
      html += "input{width:100%;padding:12px;margin:15px "
              "0;background:#222;border:1px solid "
              "#444;color:#fff;border-radius:5px;text-align:center;font-size:"
              "18px;outline:none;}";
      html += "input:focus{border-color:#00ff88;} "
              "button{width:100%;padding:12px;background:#00ff88;color:#000;"
              "border:none;border-radius:5px;font-weight:bold;cursor:pointer;"
              "font-size:16px;text-transform:uppercase;}";
      html +=
          "</style></head><body><div class='card'><h2 "
          "style='color:#00ff88;margin-top:0'>SECURE LOGIN</h2><p "
          "style='color:#888;font-size:14px'>Enter PIN to access Manual</p>";
      html += "<form><input type='password' name='pin' placeholder='PIN' "
              "autofocus><button "
              "type='submit'>UNLOCK</button></form></div></body></html>";
      server.send(401, "text/html", html);
      return;
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    String chunk =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Digital "
        "Librarian Manual</title><meta name='viewport' "
        "content='width=device-width, initial-scale=1'>";
    chunk +=
        "<style>:root { --bg: #000; --text: #ddd; --accent: #00ff88; --card: "
        "#111; } body { font-family: sans-serif; line-height: 1.6; max-width: "
        "800px; margin: 0 auto; padding: 20px; background: var(--bg); color: "
        "var(--text); } h1, h2 { color: var(--accent); border-bottom: 1px "
        "solid #333; padding-bottom: 10px; } h3 { color: #fff; margin-top: "
        "25px; } .btn { display: inline-block; padding: 8px 15px; background: "
        "var(--accent); color: #000; text-decoration: none; border-radius: "
        "5px; font-weight: bold; margin: 5px; } code { background: #222; "
        "padding: 2px 6px; border-radius: 4px; font-family: monospace; color: "
        "#ff8800; } ul { padding-left: 20px; } li { margin-bottom: 8px; } "
        "strong { color: #fff; } @media print { :root { --bg: #fff; --text: "
        "#000; --accent: #000; --card: #fff; } body { color: #000; background: "
        "#fff; padding: 0; } h1, h2, h3, strong { color: #000; border-color: "
        "#000; } .no-print { display: none; } code { border: 1px solid #ccc; "
        "background: #eee; color: #000; } }</style></head><body>";
    server.sendContent(chunk);
    chunk =
        "<div style='display:flex; justify-content:space-between; "
        "align-items:center;'><h1>Digital Librarian Manual v2.1</h1><button "
        "class='btn no-print' onclick='window.print()'>🖨️ Print</button></div>";
    chunk +=
        "<h2>1. Getting Started</h2><p><strong>Power On:</strong> Connect to "
        "USB-C power.</p><p><strong>WiFi:</strong> Connect specifically to "
        "'My_Extender' if using default.</p>";
    server.sendContent(chunk);
    chunk = "<h2>2. Device Interface</h2><ul><li><strong>Search:</strong> Top "
            "Left</li><li><strong>Add (+):</strong> New "
            "Entry/Scan</li><li><strong>Tracklist (List):</strong> Tracks & "
            "Lyrics</li><li><strong>Random:</strong> "
            "Shuffle</li><li><strong>Filter:</strong> "
            "Genre/Decade</li><li><strong>Sync:</strong> Refresh "
            "SD</li><li><strong>Eye:</strong> Toggle LED</li></ul>";
    server.sendContent(chunk);
    chunk = "<h2>3. Web Features</h2><h3>📱 Remote (/browse)</h3><p>Full "
            "control.</p><h3>📷 Scanner (/scan)</h3><p>Barcode "
            "scanning.</p><h3>🎨 Covers (/link)</h3><p>Manual artwork.</p>";
    server.sendContent(chunk);
    chunk =
        "<h2>4. Advanced</h2><ul><li><strong>Lyrics:</strong> "
        "Synced lyrics from LRCLib.</li><li><strong>Favorites:</strong> "
        "Magenta "
        "LEDs.</li><li><strong>Backups:</strong> Full JSON Export/Import.</li>"
        "<li><strong>Storage:</strong> Database stored in <code>/db/</code> "
        "folder.</li></ul>";

    chunk += "<div class='no-print'>";
    chunk += getWebFooter();
    chunk += "</div></body></html>";
    server.sendContent(chunk);
    server.sendContent("");
  });

  // Error Dashboard Page
  server.on("/errors", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html +=
        "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Error Dashboard</title>";
    html += "<style>";
    html += ":root{--bg:#000;--text:#ddd;--accent:#00ff88;--card:#111;--warn:#"
            "ff8800;--error:#ff0044;}";
    html += "body{margin:0;padding:20px;background:var(--bg);color:var(--text);"
            "font-family:sans-serif;}";
    html += "h1{color:var(--accent);margin:0 0 20px 0;}";
    html += ".container{max-width:1200px;margin:0 auto;}";
    html += ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax("
            "200px,1fr));gap:15px;margin-bottom:30px;}";
    html += ".stat-card{background:var(--card);padding:20px;border-radius:8px;"
            "border:1px solid #333;}";
    html += ".stat-card h3{margin:0 0 10px 0;font-size:14px;color:#888;}";
    html += ".stat-card "
            ".value{font-size:28px;font-weight:bold;color:var(--accent);}";
    html += ".stat-card.warn .value{color:var(--warn);}";
    html += ".stat-card.error .value{color:var(--error);}";
    html += ".controls{margin-bottom:20px;display:flex;gap:10px;align-items:"
            "center;}";
    html += "button{background:var(--accent);color:#000;border:none;padding:"
            "10px 20px;border-radius:5px;cursor:pointer;font-weight:bold;}";
    html += "button:hover{opacity:0.8;}";
    html += "button.danger{background:var(--error);}";
    html += ".error-table{background:var(--card);border-radius:8px;overflow:"
            "hidden;border:1px solid #333;}";
    html += "table{width:100%;border-collapse:collapse;}";
    html += "th{background:#222;padding:12px;text-align:left;font-size:12px;"
            "color:#888;text-transform:uppercase;}";
    html += "td{padding:12px;border-top:1px solid #333;}";
    html += ".badge{display:inline-block;padding:4px "
            "8px;border-radius:4px;font-size:11px;font-weight:bold;}";
    html += ".badge.info{background:#0088ff;color:#fff;}";
    html += ".badge.warn{background:var(--warn);color:#000;}";
    html += ".badge.error{background:var(--error);color:#fff;}";
    html += ".badge.fatal{background:#ff00ff;color:#fff;}";
    html += ".empty{text-align:center;padding:40px;color:#666;}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>📊 Error Dashboard</h1>";

    html += "<div class='stats'>";
    html += "<div class='stat-card'><h3>Free Heap</h3><div class='value' "
            "id='freeHeap'>-</div></div>";
    html += "<div class='stat-card'><h3>Min Free Heap</h3><div class='value' "
            "id='minHeap'>-</div></div>";
    html += "<div class='stat-card'><h3>Uptime</h3><div class='value' "
            "id='uptime'>-</div></div>";
    html += "<div class='stat-card'><h3>Error Count</h3><div class='value' "
            "id='errorCount'>-</div></div>";
    html += "</div>";

    html += "<div class='controls'>";
    html += "<button onclick='refresh()'>🔄 Refresh</button>";
    html += "<button onclick='clearErrors()' class='danger'>🗑️ Clear "
            "Errors</button>";
    html += "<label><input type='checkbox' id='autoRefresh'> Auto-refresh "
            "(5s)</label>";
    html += "</div>";

    html += "<div class='error-table'>";
    html += "<table><thead><tr>";
    html += "<th>Time</th><th>Level</th><th>Category</th><th>Message</"
            "th><th>Context</th>";
    html += "</tr></thead><tbody id='errorTable'></tbody></table>";
    html += "</div>";

    html += "<p "
            "style='text-align:center;margin-top:20px;color:#666;font-size:"
            "12px;'>Last updated: <span id='lastUpdate'>-</span></p>";
    html += "</div>";

    html += "<script>";
    html += "const levels=['INFO','WARN','ERROR','FATAL'];";
    html += "const "
            "categories=['NETWORK','STORAGE','API','PARSING','MEMORY','"
            "HARDWARE','SYSTEM'];";
    html += "let autoRefreshInterval=null;";

    html += "async function loadErrors(){";
    html += "  const res=await fetch('/api/errors');";
    html += "  const data=await res.json();";
    html += "  "
            "document.getElementById('freeHeap').textContent=(data.freeHeap/"
            "1024).toFixed(1)+'KB';";
    html += "  "
            "document.getElementById('minHeap').textContent=(data.minFreeHeap/"
            "1024).toFixed(1)+'KB';";
    html += "  "
            "document.getElementById('uptime').textContent=Math.floor(data."
            "uptime/60)+'m';";
    html +=
        "  "
        "document.getElementById('errorCount').textContent=data.errors.length;";
    html += "  const tbody=document.getElementById('errorTable');";
    html += "  if(data.errors.length===0){";
    html += "    tbody.innerHTML='<tr><td colspan=5 class=empty>No errors "
            "logged ✅</td></tr>';";
    html += "  }else{";
    html += "    tbody.innerHTML=data.errors.map(e=>{";
    html += "      const time=new Date(e.timestamp).toLocaleTimeString();";
    html += "      const level=levels[e.level]||'?';";
    html += "      const cat=categories[e.category]||'?';";
    html += "      const badge=level.toLowerCase();";
    html +=
        "      return `<tr><td>${time}</td><td><span class='badge "
        "${badge}'>${level}</span></td><td>${cat}</td><td>${e.message}</td><td "
        "style='color:#888;font-size:11px'>${e.context||'-'}</td></tr>`;";
    html += "    }).reverse().join('');";
    html += "  }";
    html += "  document.getElementById('lastUpdate').textContent=new "
            "Date().toLocaleTimeString();";
    html += "}";

    html += "async function clearErrors(){";
    html += "  if(!confirm('Clear all error logs?'))return;";
    html += "  await fetch('/api/errors/clear',{method:'POST'});";
    html += "  loadErrors();";
    html += "}";

    html += "function refresh(){loadErrors();}";

    html +=
        "document.getElementById('autoRefresh').addEventListener('change',e=>{";
    html += "  if(e.target.checked){";
    html += "    autoRefreshInterval=setInterval(loadErrors,5000);";
    html += "  }else{";
    html += "    clearInterval(autoRefreshInterval);";
    html += "    autoRefreshInterval=null;";
    html += "  }";
    html += "});";

    html += "loadErrors();";
    html += "</script>";

    // Add global footer
    html += "<div style='margin-top: 40px; border-top: 1px solid #333; "
            "padding-top: 20px;'>";
    html += "<h3 style='text-align:center; color:#fff; font-size: 14px; "
            "margin-bottom: 15px; text-transform: uppercase; letter-spacing: "
            "1px;'>Navigation</h3>";
    html += "<div style='display: grid; grid-template-columns: repeat(7, 1fr); "
            "gap: 10px;'>";
    html += "<button onclick=\"location.href='/'\" style='font-size: 13px; "
            "padding: 10px;'>&#127968; Dashboard</button>";
    html += "<button onclick=\"location.href='/scan'\" style='font-size: 13px; "
            "padding: 10px;'>&#128247; Scanner</button>";
    html += "<button onclick=\"location.href='/browse'\" style='font-size: "
            "13px; padding: 10px;'>&#128241; Browse</button>";
    html += "<button onclick=\"location.href='/link'\" style='font-size: 13px; "
            "padding: 10px;'>&#128444; Covers</button>";
    html += "<button onclick=\"location.href='/backup'\" style='font-size: "
            "13px; padding: 10px;'>&#128190; Backup</button>";
    html += "<button onclick=\"location.href='/manual'\" style='font-size: "
            "13px; padding: 10px;'>&#128214; Manuals</button>";
    html += "<button onclick=\"location.href='/errors'\" style='font-size: "
            "13px; padding: 10px;'>&#128681; Errors</button>";
    html += "</div></div>";

    html += "</body></html>";

    server.send(200, "text/html", html);
  });

  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });
}

// ========================================
// ARDUINO SETUP
// ========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  logMemoryUsage("BOOT START");

  logMemoryUsage("BOOT START");

  libraryMutex = xSemaphoreCreateRecursiveMutex();
  i2cMutex = xSemaphoreCreateRecursiveMutex();

  // 1. Settings
  loadSettings();

  // Error Handler Init
  ErrorHandler::init();
  ErrorHandler::logInfo(ERR_CAT_SYSTEM, "Digital Librarian starting up",
                        "setup");

  // Background Worker will be started after hardware is ready (at the end of
  // setup)

  // 2. Hardware Initialize - PRE-INIT STRATEGY
  Serial.println("Pre-Init: Asserting Touch Reset...");
  i2c_config_t i2c_conf = {.mode = I2C_MODE_MASTER,
                           .sda_io_num = EXAMPLE_I2C_SDA_PIN,
                           .scl_io_num = EXAMPLE_I2C_SCL_PIN,
                           .sda_pullup_en = GPIO_PULLUP_ENABLE,
                           .scl_pullup_en = GPIO_PULLUP_ENABLE,
                           .master = {.clk_speed = 100000},
                           .clk_flags = 0};

  // 2. Singleton IO Expander
  if (!sdExpander) {
    sdExpander =
        new ESP_IOExpander_CH422G(I2C_NUM_0, EXAMPLE_I2C_ADDR, &i2c_conf);
    sdExpander->init();
    sdExpander->begin();
  }
  sdExpander->multiPinMode(0xFF, OUTPUT);
  sdExpander->digitalWrite(TP_RST, HIGH);
  sdExpander->digitalWrite(USB_SEL, LOW);

  // Note: We MUST NOT delete it here and we MUST NOT re-create it in lcd_init
  // or after it. Shared usage is handled by the shared header macro.

  Serial.println("Main Init: lcd_init...");
  lcd_init();

  Serial.println("Post-Init: Configuring Shared Expander...");
  sdExpander->digitalWrite(LCD_BL, backlight_on ? HIGH : LOW);
  sdExpander->digitalWrite(LCD_RST, HIGH);

  // Select SD for initialization
  sdExpander->digitalWrite(SD_CS, LOW);

  // 3. SD Card & Persistence
  Serial.println("SD Card Init...");
  SPI.setHwCs(false);
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_SS);
  if (SD.begin(SD_SS)) {
    Serial.println("✅ SD Card Mounted");
    Storage.begin();

    Serial.println("Creating loading screen...");
    // Show loading screen before syncing library
    lvgl_port_lock(-1);
    Serial.println("LVGL lock acquired");

    // Create dark background
    lv_obj_t *loading_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(loading_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(loading_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(loading_bg, 0, 0);
    lv_obj_set_style_radius(loading_bg, 0, 0); // No rounded corners
    lv_obj_clear_flag(loading_bg, LV_OBJ_FLAG_SCROLLABLE);
    Serial.println("Background created");

    // Create loading label with current mode's theme color
    lv_obj_t *loading_label = lv_label_create(loading_bg);
    lv_label_set_text(loading_label, "Loading Library...\nPlease wait");
    lv_obj_set_style_text_align(loading_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(loading_label,
                                lv_color_hex(getCurrentThemeColor()), 0);
    lv_obj_set_style_text_font(loading_label, &lv_font_montserrat_26, 0);
    lv_obj_center(loading_label);
    Serial.println("Label created");

    lvgl_port_unlock();
    Serial.println("LVGL lock released");

    delay(100); // Give time for screen to actually update

    Serial.println("Syncing Library from Storage...");
    MediaManager::syncFromStorage();
    Serial.println("✅ Library Sync Complete");

    // Initialize navigation cache for fast browsing
    Serial.println("Initializing navigation cache...");
    initNavigationCache();
    rebuildNavigationCache(getCurrentItemIndex());
    Serial.println("✅ Navigation cache ready");

    // Remove loading screen
    Serial.println("Removing loading screen...");
    lvgl_port_lock(-1);
    lv_obj_del(loading_bg); // This will also delete the label
    lvgl_port_unlock();
    Serial.println("Loading screen removed");
  } else {
    Serial.println("❌ SD Card Mount FAILED!");
  }
  sdExpander->digitalWrite(SD_CS, HIGH);

  // 4. LEDs
  leds = (CRGB *)malloc(sizeof(CRGB) * led_count);
  FastLED.addLeds<WS2812B, LED_PIN, COLOR_ORDER>(leds, led_count);
  FastLED.setBrightness(led_brightness);
  FastLED.clear(true);

  // 5. Network
  Serial.println("Network Init...");
  AppNetworkManager::init();

  Serial.println("Connecting to WiFi...");
  if (AppNetworkManager::tryConnectToSavedNetworks()) {
    Serial.println("WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(AppNetworkManager::getLocalIP());
  } else {
    Serial.println("WiFi Connection Failed - Starting AP...");
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP("DigitalLibrarian_Setup", "password");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // MDNS
  Serial.println("MDNS Begin...");
  if (mdns_name.length() == 0)
    mdns_name = "digitallibrarian";
  MDNS.begin(mdns_name.c_str());

  Serial.println("Web Handlers...");
  setupWebHandlers();

  Serial.println("Server Begin...");
  server.begin();

  // 6. UI
  Serial.println("UI Setup...");
  lvgl_port_lock(-1);

  // Clear the screen to remove any loading elements
  lv_obj_clean(lv_scr_act());

  setupMainUI();

  Serial.println("Initial Update...");
  update_item_display();
  lvgl_port_unlock();

  logMemoryUsage("BOOT COMPLETE");

  // 7. Start Background Worker (Last step to ensure no bus contention during
  // boot)
  Serial.println("Starting Background Worker...");
  BackgroundWorker::begin();
}

// ========================================
// ARDUINO LOOP
// ========================================
void loop() {
  server.handleClient();

  // Screen Saver Logic
  unsigned long timeout_ms = setting_screensaver_min * 60 * 1000;
  bool should_be_off = (setting_screensaver_min > 0) &&
                       (lv_disp_get_inactive_time(NULL) > timeout_ms);

  if (should_be_off && !is_screen_off) {
    Serial.println("💤 Entering Screen Saver Mode...");
    is_screen_off = true;
    if (i2cMutex &&
        xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(100)) == pdPASS) {
      if (sdExpander)
        sdExpander->digitalWrite(LCD_BL, LOW);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    FastLED.clear();
    FastLED.show();
  } else if (!should_be_off && is_screen_off) {
    Serial.println("☀️ Waking up...");
    is_screen_off = false;
    if (i2cMutex &&
        xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(100)) == pdPASS) {
      if (sdExpander)
        sdExpander->digitalWrite(LCD_BL, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    }
    update_item_display();
  }

  delay(5);

  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 2000) {
    lastHeartbeat = millis();
    // Serial.println("[HEARTBEAT] Main loop running..."); // Debug removed
  }
}