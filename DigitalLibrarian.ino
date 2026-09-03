#include "waveshare_sd_card.h" // Custom header for SD card pin definitions
#include <Arduino.h>           // Core Arduino Library
#include <ArduinoJson.h>       // Verified with 7.4.2
#include <ESPmDNS.h>           // mDNS Support (http://mylibrary.local)
#include <FastLED.h>           // LED Strip Control
#include <HTTPClient.h>        // ESP32 Standard Library
#include <Preferences.h>       // For persistent WiFi credentials
#include <SD.h>                // ESP32 Standard Library
#include <TJpg_Decoder.h>      // v1.1.0
#include <Waveshare_ST7262_LVGL.h> // Waveshare BSP v1.0.0
#include <WebServer.h>             // For phone barcode scanner
#include <WiFi.h>                  // ESP32 Standard Library
#include <algorithm>
#include <cstdlib>
#include <esp_heap_caps.h>         // For detailed heap analysis
#include <esp_system.h>
#include <lvgl.h>                  // Verified with 8.4.0

#include "AppGlobals.h"       // Global State & Settings
#include "BackgroundWorker.h" // Scheduled Network/IO worker
#include "Core_Data.h"        // CD/Book Data Structures
#include "ErrorHandler.h"     // System-wide Error Logging
#include "MediaManager.h"     // API Clients (MusicBrainz, Google Books)
#include "NavigationCache.h"  // Smart Caching for Smooth UI
#include "NetworkManager.h"   // WiFi & Connection Management
#include "RuntimeDiagnostics.h" // Reboot-surviving operation trace
#include "Storage.h"          // SD Card Database Operations
#include "StorageTests.h"     // Integrity Checks on Boot
#include "UIManager.h"        // LVGL Interface Logic
#include "Utils.h"            // String & Helper Functions
#include "WebInterface.h"     // Remote Control Web Server
#include "mode_abstraction.h" // Polymorphic Mode Handling

// ========================================
// GLOBAL OBJECTS
// ========================================
WebServer server(80);
SemaphoreHandle_t libraryMutex = NULL;
SemaphoreHandle_t i2cMutex = NULL;
SemaphoreHandle_t ledMutex = NULL;
static bool backupUploadAuthorized = false;
static bool backupUploadFailed = false;
static size_t backupUploadBytes = 0;
static constexpr size_t MAX_BACKUP_UPLOAD_BYTES = 2 * 1024 * 1024;

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

String getCookieValue(const String &cookieHeader, const String &name) {
  const String prefix = name + "=";
  int start = 0;
  while (start < cookieHeader.length()) {
    while (start < cookieHeader.length() &&
           (cookieHeader[start] == ' ' || cookieHeader[start] == ';'))
      start++;
    int end = cookieHeader.indexOf(';', start);
    if (end < 0)
      end = cookieHeader.length();
    String cookie = cookieHeader.substring(start, end);
    if (cookie.startsWith(prefix)) {
      String value = cookie.substring(prefix.length());
      value.trim();
      return value;
    }
    start = end + 1;
  }
  return "";
}

// The SD chip-select is driven through the shared CH422G expander rather than
// a native GPIO. Immediately after the LCD/touch sequence, the card can still
// be settling and a single SD.begin() occasionally reports no card. Retry with
// a clean SPI bus and progressively lower clocks; never create or modify
// library files until a real card is confirmed.
bool mountLibrarySdCard() {
  static constexpr uint32_t frequencies[] = {4000000, 2000000, 1000000};
  SPI.setHwCs(false);

  for (size_t attempt = 0;
       attempt < sizeof(frequencies) / sizeof(frequencies[0]); ++attempt) {
    if (attempt > 0) {
      SD.end();
      SPI.end();
    }

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    delay(100 + (attempt * 150));

    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_SS);
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);
    delay(30);

    Serial.printf("SD mount attempt %u/3 at %lu Hz...\n",
                  (unsigned)(attempt + 1),
                  (unsigned long)frequencies[attempt]);
    if (SD.begin(SD_SS, SPI, frequencies[attempt]) &&
        SD.cardType() != CARD_NONE) {
      Serial.printf("SD card detected: %llu MB\n",
                    SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }

    Serial.printf("SD mount attempt %u failed\n", (unsigned)(attempt + 1));
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
  }

  SD.end();
  return false;
}

String webSessionToken = "";
String webSessionPinSnapshot = "";
IPAddress webSessionClientIp;
bool webSessionClientBound = false;
uint32_t webSessionIssuedAt = 0;
static constexpr uint32_t WEB_SESSION_MAX_AGE_MS = 15UL * 60UL * 1000UL;
static bool webAuthRequestThrottled = false;

static WebAuthThrottleEntry webAuthThrottle[4];

static bool timeBefore(uint32_t now, uint32_t deadline) {
  return deadline != 0 && (int32_t)(deadline - now) > 0;
}

static WebAuthThrottleEntry &webAuthEntryFor(const IPAddress &ip) {
  int oldest = 0;
  for (int i = 0; i < 4; i++) {
    if (webAuthThrottle[i].used && webAuthThrottle[i].ip == ip)
      return webAuthThrottle[i];
    if (!webAuthThrottle[i].used)
      return webAuthThrottle[i];
    if (webAuthThrottle[i].lastSeen < webAuthThrottle[oldest].lastSeen)
      oldest = i;
  }
  webAuthThrottle[oldest] = WebAuthThrottleEntry{};
  return webAuthThrottle[oldest];
}

static bool constantTimePinEquals(const String &candidate,
                                  const String &expected) {
  const size_t maxLength = std::max(candidate.length(), expected.length());
  size_t difference = candidate.length() ^ expected.length();
  for (size_t i = 0; i < maxLength; i++) {
    const uint8_t left = i < candidate.length() ? candidate[i] : 0;
    const uint8_t right = i < expected.length() ? expected[i] : 0;
    difference |= left ^ right;
  }
  return difference == 0;
}

static uint32_t webAuthRetryAfterSeconds() {
  WebAuthThrottleEntry &entry = webAuthEntryFor(server.client().remoteIP());
  const uint32_t now = millis();
  if (!entry.used || !timeBefore(now, entry.blockedUntil))
    return 0;
  return ((entry.blockedUntil - now) + 999) / 1000;
}

static bool webPinAttemptAllowed(const IPAddress &ip) {
  WebAuthThrottleEntry &entry = webAuthEntryFor(ip);
  entry.ip = ip;
  entry.used = true;
  entry.lastSeen = millis();
  return !timeBefore(entry.lastSeen, entry.blockedUntil);
}

static void recordWebPinResult(const IPAddress &ip, bool success) {
  WebAuthThrottleEntry &entry = webAuthEntryFor(ip);
  entry.ip = ip;
  entry.used = true;
  entry.lastSeen = millis();
  if (success) {
    entry.failures = 0;
    entry.blockedUntil = 0;
    return;
  }
  if (entry.failures < 255)
    entry.failures++;
  uint32_t delayMs = 0;
  if (entry.failures >= 5)
    delayMs = 30000;
  else if (entry.failures >= 2)
    delayMs = 1000UL << (entry.failures - 2);
  entry.blockedUntil = delayMs ? entry.lastSeen + delayMs : 0;
}

void ensureWebSessionToken() {
  if (webSessionToken.length() == 32 && webSessionPinSnapshot == web_pin)
    return;

  char token[33];
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  webSessionToken = token;
  webSessionPinSnapshot = web_pin;
  webSessionClientBound = false;
  webSessionIssuedAt = 0;
}

bool webRequestAuthorized(bool allowFormPin = false) {
  ensureWebSessionToken();
  webAuthRequestThrottled = false;
  const IPAddress remoteIp = server.client().remoteIP();
  String suppliedPin = server.header("X-Auth-Pin");
  if (allowFormPin && suppliedPin.length() == 0 && server.hasArg("pin"))
    suppliedPin = server.arg("pin");

  const String cookieToken =
      getCookieValue(server.header("Cookie"), "DL_AUTH");
  bool pinValid = false;
  if (suppliedPin.length() > 0) {
    if (!webPinAttemptAllowed(remoteIp)) {
      webAuthRequestThrottled = true;
      return false;
    }
    pinValid = constantTimePinEquals(suppliedPin, web_pin);
    recordWebPinResult(remoteIp, pinValid);
  }
  const uint32_t now = millis();
  const bool sessionValid =
      cookieToken.length() > 0 && cookieToken == webSessionToken &&
      webSessionClientBound && webSessionClientIp == remoteIp &&
      timeBefore(now, webSessionIssuedAt + WEB_SESSION_MAX_AGE_MS);
  if (pinValid) {
    webSessionClientIp = remoteIp;
    webSessionClientBound = true;
    webSessionIssuedAt = now;
    server.sendHeader("Set-Cookie", "DL_AUTH=" + webSessionToken +
                                       "; Path=/; Max-Age=900; HttpOnly; "
                                       "SameSite=Strict");
  }
  return pinValid || sessionValid;
}

bool requireWebAuth() {
  if (webRequestAuthorized())
    return true;
  server.sendHeader("Cache-Control", "no-store");
  if (webAuthRequestThrottled) {
    server.sendHeader("Retry-After", String(webAuthRetryAfterSeconds()));
    server.send(429, "text/plain", "Too many PIN attempts. Try again later.");
  } else {
    server.send(401, "text/plain", "Unauthorized");
  }
  return false;
}

void sendWebLoginPage(const String &destination, const String &feature) {
  String safeDestination = escapeJSON(destination);
  safeDestination.replace("'", "\\'");
  String safeFeature = escapeHTML(feature);
  String html =
      "<!DOCTYPE html><html><head><meta name='viewport' "
      "content='width=device-width,initial-scale=1'><meta charset='UTF-8'>"
      "<title>Digital Librarian Login</title><style>";
  html += getCommonCSS();
  html +=
      "body{min-height:100vh;display:flex;align-items:center;justify-content:"
      "center}.login-card{width:100%;max-width:390px}.lock-mark{width:54px;"
      "height:54px;display:grid;place-items:center;margin-bottom:18px;border:1px "
      "solid var(--line);border-radius:16px;background:var(--card-strong);color:"
      "var(--accent);font-size:24px}.login-card h1{font-size:34px}.login-card "
      ".subtitle{margin-bottom:22px}.login-card button{margin-top:14px}.error{"
      "color:#ffaaa6;min-height:24px;margin-top:12px;font-size:13px}.privacy-"
      "note{margin-top:18px;color:var(--sub);font-size:11px;text-align:center}"
      "</style></head><body><main class='card login-card'><div class='lock-"
      "mark'>&#128274;</div><div class='eyebrow'>Protected tool</div><h1>Unlock "
      "web access</h1><p class='subtitle'>Enter the web PIN to open the " +
      safeFeature +
      ".</p><form id='loginForm'><label for='pin'>Web PIN</label><input id='pin' name='pin' "
      "type='password' inputmode='text' autocomplete='current-password' "
      "autocapitalize='none' spellcheck='false' "
      "placeholder='Enter PIN' autofocus required><button type='submit'>Unlock "
      "this tool</button></form><div id='error' class='error' role='alert'></div>"
      "<p class='privacy-note'>Your PIN stays on this local device.</p></main><script>"
      "document.getElementById('loginForm').onsubmit=async function(e){e."
      "preventDefault();const b=this.querySelector('button'),p=document."
      "getElementById('pin').value,err=document.getElementById('error');b.disabled=true;"
      "b.innerHTML='<span class=\"spinner\"></span>Checking PIN';err.textContent='';try{const r=await fetch('/api/"
      "auth',{method:'POST',headers:{'Content-Type':'application/x-www-form-"
      "urlencoded'},body:'pin='+encodeURIComponent(p)});if(r.ok){location."
      "replace('" +
      safeDestination +
      "');return;}const retry=r.headers.get('Retry-After');document."
      "getElementById('error').textContent=r.status===429?'Too many attempts. "
      "Try again in '+(retry||'a few')+' seconds.':'Incorrect PIN';}catch(e){"
      "err.textContent='Could not reach the device. Check WiFi and try again.';}"
      "finally{b.disabled=false;b.textContent='Unlock this tool';}};</script>"
      "</body></html>";
  server.sendHeader("Cache-Control", "no-store");
  server.send(401, "text/html; charset=utf-8", html);
}

bool parseLedIndices(const String &input, std::vector<int> &result) {
  result.clear();
  String remaining = input;
  remaining.replace(' ', ',');
  while (remaining.length() > 0) {
    int comma = remaining.indexOf(',');
    String token = comma < 0 ? remaining : remaining.substring(0, comma);
    remaining = comma < 0 ? "" : remaining.substring(comma + 1);
    token.trim();
    if (token.isEmpty())
      continue;
    char *end = nullptr;
    long value = strtol(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0' || value < 0 ||
        value >= led_count)
      return false;
    if (std::find(result.begin(), result.end(), (int)value) == result.end())
      result.push_back((int)value);
  }
  return true;
}

bool validateWebItem(const ItemView &item, int editedIndex, String &error) {
  String title = item.title;
  String uniqueID = item.uniqueID;
  title.trim();
  uniqueID.trim();
  if (title.isEmpty()) {
    error = "Title is required";
    return false;
  }
  if (uniqueID.isEmpty() || sanitizeFilename(uniqueID).isEmpty()) {
    error = "A valid unique ID is required";
    return false;
  }
  if (item.year != 0 && (item.year < 1000 || item.year > 2100)) {
    error = "Year must be blank or between 1000 and 2100";
    return false;
  }
  const String safeID = sanitizeFilename(uniqueID);
  for (int i = 0; i < getItemCount(); i++) {
    if (i == editedIndex)
      continue;
    ItemView other = getItemAtRAM(i);
    if (other.uniqueID == uniqueID || sanitizeFilename(other.uniqueID) == safeID) {
      error = "Unique ID is already in use";
      return false;
    }
  }
  return true;
}

bool persistItemAt(int index, const String &oldUniqueID) {
  if (libraryMutex &&
      xSemaphoreTakeRecursive(libraryMutex, pdMS_TO_TICKS(5000)) != pdPASS)
    return false;
  bool saved = false;
  switch (currentMode) {
  case MODE_CD: {
    if (index >= 0 && index < (int)cdLibrary.size()) {
      CD candidate = cdLibrary[index];
      if (libraryMutex)
        xSemaphoreGiveRecursive(libraryMutex);
      return Storage.saveCD(candidate, oldUniqueID.c_str());
    }
    break;
  }
  case MODE_BOOK: {
    if (index >= 0 && index < (int)bookLibrary.size()) {
      Book candidate = bookLibrary[index];
      if (libraryMutex)
        xSemaphoreGiveRecursive(libraryMutex);
      return Storage.saveBook(candidate, oldUniqueID.c_str());
    }
    break;
  }
  default:
    break;
  }
  if (libraryMutex)
    xSemaphoreGiveRecursive(libraryMutex);
  return saved;
}

bool isPrivateOrReservedIPv4(const IPAddress &ip) {
  const uint8_t a = ip[0];
  const uint8_t b = ip[1];
  if (a == 0 || a == 10 || a == 127 || a >= 224)
    return true;
  if (a == 169 && b == 254)
    return true;
  if (a == 172 && b >= 16 && b <= 31)
    return true;
  if (a == 192 && b == 168)
    return true;
  if (a == 100 && b >= 64 && b <= 127)
    return true;
  if (a == 198 && (b == 18 || b == 19))
    return true;
  return false;
}

bool isSafeRemoteCoverUrl(const String &url) {
  if (!url.startsWith("https://"))
    return false;
  int hostStart = 8;
  int hostEnd = url.indexOf('/', hostStart);
  if (hostEnd < 0)
    hostEnd = url.length();
  String host = url.substring(hostStart, hostEnd);
  int portSeparator = host.indexOf(':');
  if (portSeparator >= 0)
    host = host.substring(0, portSeparator);
  host.toLowerCase();
  if (host.isEmpty() || host == "localhost" || host.endsWith(".local") ||
      host.startsWith("127.") || host.startsWith("10.") ||
      host.startsWith("192.168.") || host.startsWith("169.254.") ||
      host.startsWith("0.") || host.indexOf(':') >= 0)
    return false;
  if (host.startsWith("172.")) {
    int secondDot = host.indexOf('.', 4);
    int secondOctet = host.substring(4, secondDot).toInt();
    if (secondOctet >= 16 && secondOctet <= 31)
      return false;
  }

  IPAddress resolved;
  if (!WiFi.hostByName(host.c_str(), resolved))
    return false;
  return !isPrivateOrReservedIPv4(resolved);
}

String getWebFooter() {
  String f = "<nav class='site-nav' aria-label='Web interface navigation'>";
  f += "<h3>Navigation</h3><div class='nav-grid'>";
  f += "<a class='nav-link' href='/'>&#127968; Home</a>";
  f += "<a class='nav-link' href='/scan'>&#128247; Add</a>";
  f += "<a class='nav-link' href='/browse'>&#128241; Browse</a>";
  f += "<a class='nav-link' href='/link'>&#128444; Covers</a>";
  f += "<a class='nav-link' href='/backup'>&#128190; Backup</a>";
  f += "<a class='nav-link' href='/manual'>&#128214; Manual</a>";
  f += "<a class='nav-link' href='/errors'>&#128681; Health</a>";
  f += "</div></nav>";
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
  server.send(200, "text/html; charset=utf-8", html);
}

// ========================================
// WEB HANDLERS
// ========================================

void setupWebHandlers() {
  const char *authHeaders[] = {"X-Auth-Pin", "Cookie"};
  server.collectHeaders(authHeaders, 2);
  // 1. Dashboard
  server.on("/", HTTP_GET, []() {
    String html = String(INDEX_HTML_TEMPLATE);
    html.replace("%CSS%", getCommonCSS());
    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/api/auth", HTTP_POST, []() {
    if (!webRequestAuthorized(true)) {
      server.sendHeader("Cache-Control", "no-store");
      if (webAuthRequestThrottled) {
        server.sendHeader("Retry-After", String(webAuthRetryAfterSeconds()));
        server.send(429, "text/plain", "Too many attempts");
      } else {
        server.send(401, "text/plain", "Unauthorized");
      }
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(204, "text/plain", "");
  });

  // 2. Status API
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<256> doc;
    if (libraryMutex)
      xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
    doc["cdCount"] = cdLibrary.size();
    doc["bookCount"] = bookLibrary.size();
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);
    doc["currentMode"] = (int)currentMode;
    doc["heap"] = ESP.getFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["resetReason"] = boot_reset_reason;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // 2.5. Error Log API
  server.on("/api/errors", HTTP_GET, []() {
    if (!requireWebAuth())
      return;
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
    if (!requireWebAuth())
      return;
    ErrorHandler::clearRecentErrors();
    ErrorHandler::logInfo(ERR_CAT_SYSTEM, "Error log cleared via web API",
                          "/api/errors/clear");
    server.send(200, "application/json", "{\"status\":\"cleared\"}");
  });

  // 2.7. Destructive storage tests (explicit development builds only)
  server.on("/api/tests/run", HTTP_POST, []() {
    if (!requireWebAuth())
      return;
#if defined(ENABLE_DESTRUCTIVE_STORAGE_TESTS) && ENABLE_DESTRUCTIVE_STORAGE_TESTS
    String results = StorageTests::runTests();
    server.send(200, "text/plain", results);
#else
    server.send(403, "text/plain",
                "Storage tests are disabled in normal firmware builds");
#endif
  });

  // 2.8. detailed heap analysis
  server.on("/api/debug/heap", HTTP_GET, []() {
    if (!requireWebAuth())
      return;
    printDetailedMemoryStats();
    server.send(200, "text/plain",
                "Detailed heap info printed to Serial Console.");
  });

  // Lightweight authenticated job diagnostics. This lets the physical UI's
  // background actions be exercised and verified without generating the very
  // large remote-library page on a weak WiFi connection.
  server.on("/api/job", HTTP_GET, []() {
    if (!requireWebAuth())
      return;
    DynamicJsonDocument doc(1536);
    doc["busy"] = BackgroundWorker::isBusy();
    doc["queue"] = BackgroundWorker::getQueueSize();
    doc["currentJob"] = (int)BackgroundWorker::getCurrentJobType();
    doc["lastJob"] = (int)BackgroundWorker::getLastCompletedJobType();
    doc["lastSuccess"] = BackgroundWorker::wasLastJobSuccessful();
    doc["progress"] = BackgroundWorker::getProgress();
    doc["status"] = BackgroundWorker::getStatusMessage();
    uint32_t coverSequence = 0;
    bool coverSuccess = false;
    String coverMessage;
    String coverItemId;
    BackgroundWorker::getLastCoverCompletion(coverSequence, coverSuccess,
                                             coverMessage, &coverItemId);
    doc["coverSequence"] = coverSequence;
    doc["coverSuccess"] = coverSuccess;
    doc["coverStatus"] = coverMessage;
    doc["coverItemId"] = coverItemId;
    uint32_t webAddSequence = 0;
    bool webAddSuccess = false;
    String webAddMessage;
    String webAddCode;
    BackgroundWorker::getLastWebAddCompletion(webAddSequence, webAddSuccess,
                                               webAddMessage, &webAddCode);
    doc["webAddSequence"] = webAddSequence;
    doc["webAddSuccess"] = webAddSuccess;
    doc["webAddStatus"] = webAddMessage;
    doc["webAddCode"] = webAddCode;
    const DiagnosticBreadcrumb currentTrace =
        RuntimeDiagnostics::getCurrentOperation();
    doc["traceActive"] = currentTrace.active != 0;
    doc["tracePhase"] = currentTrace.phase;
    doc["traceItem"] = currentTrace.item;
    doc["traceIndex"] = currentTrace.itemIndex;
    doc["traceCount"] = currentTrace.itemCount;
    doc["traceLargestInternal"] = currentTrace.largestInternal;
    doc["traceWorkerStackWords"] = currentTrace.workerStackWords;
    doc["previousInterrupted"] =
        RuntimeDiagnostics::hasPreviousInterruptedOperation();
    if (RuntimeDiagnostics::hasPreviousInterruptedOperation()) {
      const DiagnosticBreadcrumb previousTrace =
          RuntimeDiagnostics::getPreviousInterruptedOperation();
      doc["previousPhase"] = previousTrace.phase;
      doc["previousItem"] = previousTrace.item;
      doc["previousIndex"] = previousTrace.itemIndex;
      doc["previousCount"] = previousTrace.itemCount;
      doc["previousLargestInternal"] = previousTrace.largestInternal;
      doc["previousWorkerStackWords"] = previousTrace.workerStackWords;
      doc["previousResetReason"] =
          RuntimeDiagnostics::getPreviousResetReason();
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  // Non-destructive reproduction of the lyrics TLS/PSRAM path. It performs a
  // bounded number of read-only LRCLIB requests and can be cancelled from the
  // normal progress dialog. No library, lyrics, or SD records are changed.
  server.on("/api/debug/stress/lyrics", HTTP_POST, []() {
    if (!requireWebAuth())
      return;
    if (WiFi.status() != WL_CONNECTED) {
      server.send(503, "text/plain", "No WiFi connection");
      return;
    }
    const int cycles = constrain(
        server.hasArg("cycles") ? server.arg("cycles").toInt() : 5, 1, 20);
    const bool queued = BackgroundWorker::addJob(
        {JOB_DIAGNOSTIC_LYRICS_STRESS, "LRCLIB", cycles, "", nullptr, true});
    if (!queued) {
      server.send(503, "text/plain", "Background queue is busy");
      return;
    }
    server.send(202, "application/json", "{\"status\":\"queued\"}");
  });

  // 3. Remote Control API
  server.on("/api/control", HTTP_POST, []() {
    String action = server.arg("action");
    if (!requireWebAuth())
      return;

    if (action == "random") {
      lvgl_port_lock(-1);
      selectRandomWithEffect();
      lvgl_port_unlock();
      server.send(200, "text/plain", "Random selected");

    } else if (action == "coversearch") {
      const int id = server.hasArg("id") ? server.arg("id").toInt()
                                          : getCurrentItemIndex();
      if (id < 0 || id >= getItemCount()) {
        server.send(400, "text/plain", "Invalid ID");
        return;
      }
      if (WiFi.status() != WL_CONNECTED) {
        server.send(503, "text/plain", "No WiFi connection");
        return;
      }

      const ItemView item = getItemAtRAM(id);
      const bool queued = BackgroundWorker::addJob(
          {JOB_COVER_DOWNLOAD, item.uniqueID, id, "", nullptr, true});
      if (!queued) {
        server.send(503, "text/plain", "Background queue is full");
        return;
      }

      StaticJsonDocument<256> doc;
      doc["status"] = "queued";
      doc["id"] = id;
      doc["title"] = item.title;
      doc["hasSavedCoverUrl"] = item.coverUrl.length() > 0;
      doc["hasCoverFile"] = item.coverFile.length() > 0;
      String out;
      serializeJson(doc, out);
      server.send(202, "application/json", out);

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
        ItemView original = getItemAt(id);
        ItemView item = original;
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
          if (!parseLedIndices(server.arg("ledIndex"), item.ledIndices)) {
            server.send(400, "text/plain", "LED positions must be unique numbers within the configured strip");
            return;
          }
        }

        if (server.hasArg("barcode"))
          item.codecOrIsbn = server.arg("barcode");
        if (server.hasArg("notes"))
          item.notes = server.arg("notes");

        String validationError;
        if (!validateWebItem(item, id, validationError)) {
          server.send(400, "text/plain", validationError);
          return;
        }

        setItem(id, item);
        if (!persistItemAt(id, oldUniqueID)) {
          setItem(id, original);
          server.send(500, "text/plain",
                      "Storage write failed; the in-memory edit was rolled back");
          return;
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
        ensureItemDetailsLoaded(id);
        ItemView original = getItemAt(id);
        ItemView item = original;
        if (!parseLedIndices(server.arg("leds"), item.ledIndices)) {
          server.send(400, "text/plain",
                      "LED positions must be unique numbers within the configured strip");
          return;
        }

        setItem(id, item);
        if (!persistItemAt(id, original.uniqueID)) {
          setItem(id, original);
          server.send(500, "text/plain",
                      "Storage write failed; LED positions were rolled back");
          return;
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
      if (ledMutex)
        xSemaphoreTake(ledMutex, portMAX_DELAY);
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
      if (ledMutex)
        xSemaphoreGive(ledMutex);
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
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/led-select?cd=" + server.arg("cd"), "LED selector");
      return;
    }
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
    html += "<meta charset='UTF-8'>";
    html += "<title>LED Selector - " + escapeHTML(cd.title) + "</title>";
    html += "<style>";
    html += getCommonCSS();
    html += "*{margin:0;padding:0;box-sizing:border-box}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe "
            "UI',Roboto,Arial,sans-serif;padding-bottom:100px}";
    html += ".header{position:sticky;top:10px;background:linear-gradient(145deg,"
            "rgba(23,52,79,.98),rgba(10,27,44,.98));padding:18px;border:1px "
            "solid var(--line-soft);border-radius:18px;box-shadow:var(--shadow);"
            "z-index:100}";
    html += ".cd-info{margin-bottom:12px}.cd-title{font-size:20px;font-weight:"
            "800;color:var(--text);margin-bottom:4px}.cd-artist{font-size:14px;"
            "color:var(--sub)}";
    html += ".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}";
    html += ".btn{padding:10px "
            "16px;border:none;border-radius:8px;font-weight:600;font-size:13px;"
            "cursor:pointer;transition:all "
            "0.2s;flex:1;min-width:80px;text-align:center}";
    html += ".btn{width:auto}.btn-primary{background:var(--accent);color:#18200b}"
            ".btn-secondary{background:var(--card-strong);color:var(--text);"
            "border:1px solid var(--line)}";
    html += ".search-box{width:100%;margin-top:10px}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax("
            "52px,1fr));gap:8px;padding:18px 2px;max-width:100%}";
    html += ".led{aspect-ratio:1;border-radius:11px;border:1px solid "
            "var(--line);background:rgba(16,36,58,.82);display:flex;align-items:center;justify-"
            "content:center;font-size:11px;font-weight:700;cursor:pointer;"
            "transition:all "
            "0.15s;user-select:none;-webkit-tap-highlight-color:transparent}";
    html += ".led.selected{background:var(--accent);color:#18200b;border-color:"
            "var(--accent);box-shadow:0 0 16px rgba(246,207,74,.4)}";
    html += ".led:active{transform:scale(0.92)}";
    html += ".led:hover{border-color:var(--cyan);transform:scale(1.04)}";
    html += ".footer{position:fixed;bottom:12px;left:50%;transform:translateX(-"
            "50%);width:min(calc(100% - 24px),756px);background:linear-gradient("
            "145deg,rgba(23,52,79,.98),rgba(10,27,44,.98));padding:12px;border:"
            "1px solid var(--line);border-radius:16px;box-shadow:var(--shadow);"
            "display:flex;gap:10px;z-index:110}";
    html += ".count{text-align:center;padding:8px;background:rgba(4,13,24,.55);"
            "border-radius:11px;font-size:14px;color:var(--accent);font-weight:"
            "700;flex:1}";
    html += "@media(max-width:600px){.grid{grid-template-columns:repeat(auto-"
            "fill,minmax(45px,1fr));gap:5px}.led{font-size:10px}}";
    html += "</style></head><body>";

    html += "<div class='header'>";
    html += "<div class='cd-info'><div class='cd-title'>" + escapeHTML(cd.title) +
            "</div><div class='cd-artist'>" + escapeHTML(cd.artistOrAuthor) +
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
    html += "led.onclick=()=>toggle(i,led);";
    html += "return led;";
    html += "}";

    html += "function render(){";
    html += "grid.innerHTML='';";
    html += "for(let i=0;i<totalLEDs;i++)grid.appendChild(createLED(i));";
    html += "updateCount();";
    html += "}";

    html += "function toggle(i,led){";
    html += "if(selected.has(i))selected.delete(i);else selected.add(i);";
    html += "led.classList.toggle('selected',selected.has(i));";
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
    html += ".then(async r=>{const msg=await r.text();if(!r.ok)throw new "
            "Error(msg||'Preview failed');console.log('Preview:',msg);})";
    html += ".catch(e=>console.error('Preview error:',e));";
    html += "},100);";
    html += "}";

    html += "async function save(){";
    html += "const leds=Array.from(selected).sort((a,b)=>a-b).join(',');";
    html += "const btn=document.querySelector('.footer button');btn.disabled=true;"
            "btn.textContent='SAVING...';try{";
    html += "const r=await fetch('/api/"
            "control',{method:'POST',headers:{'Content-Type':'application/"
            "x-www-form-urlencoded'},body:'action=ledupdate&id='+cdId+'&leds='+"
            "encodeURIComponent(leds)});const response=await r.text();if(!r.ok)"
            "throw new Error(response||'Save failed');";
    html += "console.log('Sending broadcast:', leds);";
    html += "const msg={type:'led-update',leds:leds};";
    html += "if(window.opener){window.opener.postMessage(msg,'*');}";
    html += "try{const bc=new "
            "BroadcastChannel('led_channel');bc.postMessage(msg);bc.close();}"
            "catch(e){console.log('BC Err',e);}";
    html += "alert('LEDs saved!');window.close();";
    html += "}catch(e){alert('Error: '+e.message);btn.disabled=false;"
            "btn.textContent='SAVE & CLOSE';}";
    html += "}";

    html += "render();";
    html += "</script></body></html>";

    server.send(200, "text/html; charset=utf-8", html);
  });

  // 4. Scanner Tool (Full Featured)
  server.on("/scan", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      String destination = "/scan";
      if (server.hasArg("code"))
        destination += "?code=" + urlEncode(server.arg("code"));
      sendWebLoginPage(destination, "scanner");
      return;
    }
    String codeArg = server.arg("code");
    if (codeArg.length() == 0)
      codeArg = server.arg("barcode");

    String html = "";
    html += "<!DOCTYPE html>";
    html += "<html>";
    html += "<head>";
    html += "    <meta name=\"viewport\" content=\"width=device-width, "
            "initial-scale=1, maximum-scale=1\">";
    html += "    <meta charset=\"UTF-8\">";
    html += "    <title>Scanner</title>";
    html += "    <style>";
    html += getCommonCSS();
    html += ".container{width:100%;max-width:620px;margin:0 auto}.scan-card{"
            "position:relative}.input-group{margin-bottom:14px}textarea{min-"
            "height:160px;resize:vertical;font-family:ui-monospace,SFMono-"
            "Regular,Consolas,monospace;line-height:1.5}.helper{margin-top:"
            "12px;color:var(--sub);font-size:12px}.result{display:none;margin-"
            "bottom:16px;padding:8px 16px;border:1px solid var(--line-soft);"
            "border-radius:16px;background:rgba(7,17,29,.58)}.result>div:last-"
            "child{border-bottom:0!important;margin-bottom:0!important}.step-"
            "badge{display:inline-flex;align-items:center;gap:7px;color:var(--"
            "cyan);font-size:12px;font-weight:800;letter-spacing:.08em;text-"
            "transform:uppercase;margin-bottom:8px}";
    html += "    </style>";
    html += "</head>";
    html += "<body>";
    html += "    <div class=\"container\">";
    html += "<header class='hero'><div class='eyebrow'>Fast entry</div><h1>Add " +
            getModeName() +
            "</h1><p class='subtitle'>Scan a code or paste several codes to "
            "build your collection.</p></header>";
    String codeLabel = getCodeLabel();
    if (codeLabel.endsWith(":"))
      codeLabel = codeLabel.substring(0, codeLabel.length() - 1); // remove ':'
    html += "        <div id=\"result\" class=\"result\" aria-live=\"polite\"></div>";

    html += "        <form id=\"scanForm\" class=\"card scan-card\">";
    html += "<div class='step-badge'>01 &nbsp; Enter codes</div>";
    html += "            <div class=\"input-group\">";
    String placeholder = "Scan or paste " + codeLabel + "s (one per line)";
    html += "                <textarea id=\"barcode\" "
            "placeholder=\"" +
            placeholder +
            "\" required "
            "autocomplete=\"off\" rows=\"5\">" +
            escapeHTML(codeArg) + "</textarea>";
    html += "            </div>";
    html += "            <button type=\"submit\" id=\"lookupButton\">Look up and add</button>";
    html += "<p class='helper'>One code per line. You can paste a batch and "
            "leave this page open while each item is processed.</p>";
    html += "        </form>";

    html += "    </div>"; // This closing div is for the container, it should
                          // remain.
    html += getWebFooter();

    // Client-side Logic (Main App Only)
    html += "<script>";
    html += "const pause=ms=>new Promise(r=>setTimeout(r,ms));";
    html += "function showLine(el,text,ok){el.textContent=text;el.style.color=ok?'var(--ok)':'#ffaaa6';}";
    html += "async function waitForWebAdd(after,code){";
    html += " for(let n=0;n<240;n++){await pause(500);const r=await fetch('/api/job');";
    html += "  if(!r.ok)throw new Error(await r.text());const j=await r.json();";
    html += "  if(j.webAddSequence>after&&j.webAddCode===code)return j;}throw new Error('Lookup timed out');}";
    html += "async function queueLookup(code,force){";
    html += " const body='barcode='+encodeURIComponent(code)+(force?'&force=true':'');";
    html += " const r=await fetch('/api/lookup',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});";
    html += " if(r.status===409){const d=await r.json();return {duplicate:d};}";
    html += " if(r.status!==202)throw new Error(await r.text());const q=await r.json();return {job:await waitForWebAdd(q.waitAfter,code)};}";
    html += "async function processQueue(lines,idx,res,btn){";
    html += " if(idx>=lines.length){btn.innerText='Look up and add';btn.disabled=false;document.getElementById('barcode').value='';return;}";
    html += " const code=lines[idx];btn.innerText='Processing '+(idx+1)+'/'+lines.length+'...';";
    html += " const itemDiv=document.createElement('div');itemDiv.style.cssText='border-bottom:1px solid #333;margin-bottom:10px;padding-bottom:10px';";
    html += " itemDiv.textContent='Scanning: '+code+'...';res.prepend(itemDiv);";
    html += " try{let result=await queueLookup(code,false);";
    html += "  if(result.duplicate){const d=result.duplicate;const force=confirm('Duplicate found for '+code+':\\n'+d.title+'\\nby '+d.artist+'\\n\\nAdd copy anyway?');";
    html += "   if(!force){showLine(itemDiv,'Code: '+code+' - Skipped duplicate',true);return processQueue(lines,idx+1,res,btn);}result=await queueLookup(code,true);}";
    html += "  const j=result.job;showLine(itemDiv,'Code: '+code+' - '+j.webAddStatus,j.webAddSuccess);";
    html += " }catch(e){showLine(itemDiv,'Code: '+code+' - '+e.message,false);}";
    html += " await pause(500);processQueue(lines,idx+1,res,btn);}";
    html += "    document.getElementById('scanForm').onsubmit = function(e) {";
    html += "        e.preventDefault();";
    html +=
        "        var txt = document.getElementById('barcode').value.trim();";
    html += "        if(!txt) return;";
    html += "        var lines = "
            "txt.split(/[\\n,]+/).map(s=>s.trim()).filter(s=>s.length>0);";
    html += "        if(lines.length===0) return;";
    html += "        var res = document.getElementById('result');";
    html += "        var btn = document.getElementById('lookupButton');";
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
    if (codeArg.length() > 0) {
      html += "    setTimeout(function(){ var e = new Event('submit'); "
              "document.getElementById('scanForm').dispatchEvent(e); }, 300);";
    }

    html += "</script>";

    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  // Manual Cover Link Page
  server.on("/link", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/link", "cover manager");
      return;
    }

    String html = "";
    html += "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, "
            "initial-scale=1, maximum-scale=1'>";
    html += "<meta charset='UTF-8'><title>" + getModeName() + " Art</title>";
    html += "<style>";
    html += getCommonCSS();
    html += ".container{width:100%;max-width:620px;margin:0 auto}.cover-form ."
            "field:last-of-type{margin-bottom:20px}.hint{margin-top:12px;color:"
            "var(--sub);font-size:12px}.result{margin:0 0 16px;padding:14px "
            "16px;border-radius:14px;background:rgba(7,17,29,.65);border:1px "
            "solid var(--line-soft)}.result.success{border-color:rgba(85,214,"
            "158,.55);color:var(--ok)}.result.error{border-color:rgba(255,101,"
            "95,.6);color:#ffaaa6}";
    html += "</style></head><body>";

    html += "<div class='container'>";
    html += "<header class='hero'><div class='eyebrow'>Artwork manager</div><h1>"
            "Update " + getModeName() +
            " cover</h1><p class='subtitle'>Attach a public HTTPS image to a "
            "record. The device downloads and stores it safely.</p></header>";
    html += "<div id='result' class='result' style='display:none' role='status' "
            "aria-live='polite'></div>";
    html += "<form id='linkForm' class='card cover-form'>";
    html += "<div class='field'><label for='target_id'>Record unique ID</label>"
            "<input type='text' id='target_id' placeholder='Example: "
            "093624639329' required autocomplete='off'></div>";
    html += "<div class='field'><label for='url'>Cover image URL</label><input "
            "type='url' inputmode='url' id='url' placeholder='https://example."
            "com/cover.jpg' required autocomplete='off'></div>";
    html += "<button type='submit' id='coverButton'>Update " + getModeName() +
            " cover</button>";
    html += "<p class='hint'>Tip: copy the unique ID from the Browse page's "
            "Edit panel. Only public HTTPS image addresses are accepted.</p>";
    html += "</form>";

    html += "</div>";

    html += "<script>";
    html += "const coverPause=ms=>new Promise(r=>setTimeout(r,ms));";
    html += "async function waitForCover(after,itemId){for(let n=0;n<240;n++){await coverPause(500);const r=await fetch('/api/job');if(!r.ok)throw new Error(await r.text());const j=await r.json();if(j.coverSequence>after&&j.coverItemId===itemId)return j;}throw new Error('Cover download timed out');}";
    html += "document.getElementById('linkForm').onsubmit = async function(e) {";
    html += "  e.preventDefault();";
    html += "  var url = document.getElementById('url').value.trim();";
    html += "  var tid = document.getElementById('target_id').value.trim();";
    html += "  var res = document.getElementById('result');";
    html += "  var btn = document.getElementById('coverButton');";
    html += "  if(url.length < 5 || tid.length < 1) return;";
    html += "  btn.innerHTML = '<span class=\"spinner\"></span>Downloading "
            "cover'; btn.disabled = true; "
            "res.style.display = 'none';";
    html += "  try{";
    html += "    const response=await fetch('/api/setcover',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'url='+encodeURIComponent(url)+'&id='+encodeURIComponent(tid)});";
    html += "    if(response.status===401){localStorage.removeItem('web_pin');location.reload();return;}";
    html += "    if(response.status!==202)throw new Error(await response.text());";
    html += "    const queued=await response.json();const job=await waitForCover(queued.waitAfter,tid);";
    html += "    res.style.display='block';res.className=job.coverSuccess?'result success':'result error';";
    html += "    res.textContent=(job.coverSuccess?'Cover updated: ':'Cover failed: ')+queued.title+' - '+job.coverStatus;";
    html += "    if(job.coverSuccess)document.getElementById('url').value='';";
    html += "  }catch(err){res.style.display='block';res.className='result error';res.textContent=err.message;}finally{btn.innerText='Update Cover';btn.disabled=false;}";
    html += "};";
    html += "</script>";

    html += getWebFooter();
    html += "</body></html>";

    server.send(200, "text/html; charset=utf-8", html);
  });

  // API to update cover from URL
  server.on("/api/setcover", HTTP_POST, []() {
    if (!requireWebAuth())
      return;
    String url = server.arg("url");
    if (!isSafeRemoteCoverUrl(url)) {
      server.send(400, "text/plain",
                  "Cover URL must be a public HTTPS address");
      return;
    }
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

    uint32_t waitAfter = 0;
    bool ignoredSuccess = false;
    String ignoredMessage;
    BackgroundWorker::getLastCoverCompletion(waitAfter, ignoredSuccess,
                                              ignoredMessage);
    if (!BackgroundWorker::addJob({JOB_COVER_DOWNLOAD, item.uniqueID,
                                   targetIndex, url, nullptr, true})) {
      server.send(503, "text/plain", "Background queue is full; try again");
      return;
    }

    StaticJsonDocument<384> doc;
    doc["status"] = "queued";
    doc["waitAfter"] = waitAfter;
    doc["title"] = item.title;
    doc["artist"] = item.artistOrAuthor;
    doc["year"] = item.year;
    doc["genre"] = item.genre;
    String json;
    serializeJson(doc, json);
    server.send(202, "application/json", json);
  });

  // 5. Metadata Lookup API (Updated with Duplicate Check)
  server.on("/api/lookup", HTTP_POST, []() {
    if (!requireWebAuth())
      return;
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

    if (code.length() < 4) {
      server.send(400, "text/plain", "Code is too short");
      return;
    }

    uint32_t waitAfter = 0;
    bool ignoredSuccess = false;
    String ignoredMessage;
    BackgroundWorker::getLastWebAddCompletion(waitAfter, ignoredSuccess,
                                               ignoredMessage);
    if (!BackgroundWorker::addJob({JOB_WEB_METADATA_ADD, code, -1,
                                   force ? "force" : "", nullptr, true})) {
      server.send(503, "text/plain", "Background queue is full; try again");
      return;
    }
    StaticJsonDocument<192> doc;
    doc["status"] = "queued";
    doc["waitAfter"] = waitAfter;
    String json;
    serializeJson(doc, json);
    server.send(202, "application/json", json);
  });

  // 6. Remote Browser (Full Featured)
  server.on("/browse", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/browse", "remote library");
      return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", ""); // Start chunked transfer

    // 1. Send HEAD and STYLES
    String chunk = "<!DOCTYPE html><html><head>";
    chunk +=
        "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    chunk += "<meta charset='UTF-8'>";
    chunk += "<title>Remote " + getModeName() + " Control</title>";
    chunk += "<style>";
    chunk += getCommonCSS();
    chunk += "* { box-sizing: border-box; }";
    chunk += "body{padding-bottom:60px;max-width:860px}.browser-head{padding-"
             "bottom:16px}.filter-card{position:sticky;top:8px;z-index:50;"
             "padding:14px;margin-bottom:14px;backdrop-filter:blur(12px)}"
             ".filter-row{display:flex;gap:9px;flex-wrap:wrap}.filter-row "
             "select{flex:1;min-width:135px;margin:0}.filter-row label{margin:"
             "0}.list-summary{display:flex;justify-content:space-between;align-"
             "items:center;margin:12px 2px;color:var(--sub);font-size:12px}";
    chunk +=
        ".cd { background:linear-gradient(145deg,rgba(23,52,79,.9),rgba(12,"
        "31,50,.92));border:1px solid var(--line-soft);padding:14px 15px;"
        "margin-bottom:9px;border-radius:14px;display:flex;align-items:"
        "center; justify-content: space-between; gap: 10px; }";
    chunk += ".cd-info { flex: 1; min-width: 0; padding-right: 10px; overflow: "
             "hidden; }";
    chunk += ".cd:active{border-color:var(--cyan)}";
    chunk +=
        ".cd h3 { margin: 0; font-size: 16px; color:var(--text); white-space: "
        "nowrap; overflow: hidden; text-overflow: ellipsis; }";
    chunk +=
        ".cd p { margin: 4px 0 0; color:var(--sub); font-size: 13px; white-space: "
        "nowrap; overflow: hidden; text-overflow: ellipsis; }";

    // Buttons
    chunk += ".btn-group { display: flex; align-items: center; gap: 8px; "
             "flex-shrink: 0; }";
    chunk += ".btn-go,.btn-edit{width:auto;min-height:40px;padding:9px 14px}"
             ".btn-go {background:var(--accent);color:#18200b;"
             "border:none;border-radius:10px;font-weight:bold;cursor:"
             "pointer; text-transform: uppercase; font-size: 14px; }";
    chunk +=
        ".btn-edit{background:var(--card-strong);color:var(--text);border:1px "
        "solid var(--line);border-radius:10px;font-weight:bold;cursor:"
        "pointer; text-transform: uppercase; font-size: 14px; }";
    chunk += ".btn-edit:hover{border-color:var(--cyan)}";

    chunk += "#search{margin-bottom:10px}.filter-favorite{display:flex;align-"
             "items:center;gap:8px;min-height:48px;padding:10px 13px;border:1px "
             "solid var(--line);border-radius:13px;background:rgba(4,13,24,.74);"
             "color:var(--text);white-space:nowrap}.filter-favorite input{width:"
             "auto;min-height:0;margin:0;accent-color:var(--accent)}";

    // Modal CSS
    chunk +=
        ".modal{display:none;position:fixed;inset:0;width:100%;height:100%;"
        "background:rgba(1,7,13,.88);z-index:1000;padding:20px;"
        "box-sizing:border-box; overflow-y:auto; }";
    chunk += ".modal-content{background:linear-gradient(145deg,var(--card-"
             "strong),var(--card));padding:20px;border:1px solid var(--line);"
             "border-radius:18px;max-width:540px;margin:20px auto;box-shadow:"
             "var(--shadow)}.modal-content h2{margin-bottom:18px}.modal-content>"
             "input:not([type=hidden]){margin-bottom:14px}.modal-actions{display:"
             "grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:20px}.led-"
             "field{display:flex;gap:8px;align-items:stretch;margin-bottom:14px}"
             ".led-field input{flex:1;min-width:0}.led-field button{width:auto;"
             "white-space:nowrap}.favorite-field{display:flex;align-items:center;"
             "gap:10px;color:var(--text);margin-top:4px}.favorite-field input{"
             "width:auto;min-height:0;margin:0;accent-color:var(--accent)}.empty-state{"
             "padding:42px 20px;text-align:center;color:var(--sub);border:1px "
             "dashed var(--line);border-radius:16px}@media(max-width:560px){"
             ".filter-card{top:0}.btn-group{gap:5px}.btn-go,.btn-edit{padding:"
             "8px 10px;font-size:12px}.cd{padding:12px}.list-summary{align-items:"
             "flex-start;flex-direction:column}.modal{padding:8px}.modal-content{"
             "margin:8px auto;padding:17px}.modal-actions{grid-template-columns:"
             "1fr}.led-field{flex-direction:column}.led-field button{width:100%}}";

    chunk += "</style></head><body>";
    server.sendContent(chunk);

    // 2. Send BODY Content (Headers & Filters)
    chunk = "<header class='hero browser-head'><div class='eyebrow'>Remote "
            "control</div><h1>Your library</h1><p class='subtitle'>Find a "
            "record, light its shelf position, or update its details.</p></"
            "header><section class='card filter-card'>";
    chunk += "<input type='text' id='search' placeholder='Search...' "
             "oninput='filter()' aria-label='Search the library'>";

    // Filter Controls Container
    chunk += "<div class='filter-row'>";

    // Genre Filter
    chunk += "<select id='filterGenre' onchange='filter();syncDeviceFilter()' "
             "aria-label='Filter by genre'>";
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
    chunk = "<select id='filterDecade' onchange='filter();syncDeviceFilter()' "
            "aria-label='Filter by decade'>";
    chunk += "<option value=''>All Decades</option>";
    chunk += "<option value='60'>60s</option><option "
             "value='70'>70s</option><option value='80'>80s</option>";
    chunk += "<option value='90'>90s</option><option "
             "value='00'>2000s</option><option "
             "value='10'>2010s</option><option value='20'>2020s</option>";
    chunk += "</select>";

    chunk += "<label class='filter-favorite'>";
    chunk += "<input type='checkbox' id='filterFav' "
             "onchange='filter();syncDeviceFilter()'>";
    chunk += "<span>&#9733; Favorites Only</span></label>";
    chunk += "</div></section>";

    chunk += "<div class='list-summary'><span id='result-count'></span><span>"
             "Tap GO to locate an item</span></div><div id='list'></div>";
    chunk += "<div id='browser-feedback' class='feedback' role='status' "
             "aria-live='polite'></div>";

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
    chunk += "<div class='led-field'>";
    chunk += "<input id='edit-ledIndex' type='text'>";
    chunk += "<button onclick='openLEDSelector()' type='button' class='btn-"
             "secondary'>&#9678; SELECT LEDs</button>";
    chunk += "</div>";
    String barcodeLabel = getCodeLabel();
    if (barcodeLabel.endsWith(":"))
      barcodeLabel =
          barcodeLabel.substring(0, barcodeLabel.length() - 1); // remove ':'
    chunk += "<label>" + barcodeLabel + "</label><input id='edit-barcode'>";
    chunk += "<label>Notes</label><input id='edit-notes'>";
    chunk += "<label class='favorite-field'><input "
             "type='checkbox' id='edit-fav'> "
             "Favorite</label>";
    chunk += "<div class='modal-actions'>";
    chunk += "<button id='save-edit' onclick='saveEdit()'>SAVE CHANGES</button>";
    chunk += "<button "
             "onclick=\"document.getElementById('edit-modal').style.display='"
             "none'\" class='btn-secondary'>CANCEL</button>";
    chunk += "</div>";
    chunk += "</div></div>";

    server.sendContent(chunk);

    // 3. Send SCRIPT - Start
    chunk = "<script>";
    chunk += "const browserFeedback=document.getElementById('browser-feedback');"
             "function showBrowserFeedback(message,type=''){browserFeedback."
             "textContent=message;browserFeedback.className='feedback show '+"
             "type;setTimeout(()=>{browserFeedback.className='feedback';},"
             "4500);}window.onerror=function(msg){console.error(msg);"
             "showBrowserFeedback('The page hit an unexpected error. Refresh "
             "and try again.','error');return false;};";
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
    chunk += "const esc=s=>String(s??'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));";
    chunk += "function render(items) {";
    chunk += "  list.innerHTML = '';";
    chunk += "  document.getElementById('result-count').textContent=items.length+"
             "' of '+library.length+' records';";
    chunk += "  if(items.length===0){list.innerHTML=\"<div class='empty-state'>"
             "No matching records. Try a broader search or clear a filter.</div>\";"
             "return;}";
    chunk += "  items.forEach(cd => {";
    chunk += "    const div = document.createElement('div');";
    chunk += "    div.className = 'cd';";
    chunk += "    div.innerHTML = `<div "
             "class='cd-info'><h3>${esc(cd.title)}</h3><p>${esc(cd.artist)}</p></div>";
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
    chunk += "doAction('ledpreview','&leds='+encodeURIComponent(leds))";
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

    chunk += "async function saveEdit() {";
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

    chunk += "  const btn=document.getElementById('save-edit');btn.disabled=true;"
             "btn.textContent='SAVING...';try{";
    chunk +=
        "  await doAction('edit', "
        "'&id='+encodeURIComponent(id)+'&title='+encodeURIComponent(t)+"
        "'&artist='+encodeURIComponent(a)+'&genre='+encodeURIComponent(g)+"
        "'&year='+encodeURIComponent(y)+'&fav='+f+'&uniqueID='+"
        "encodeURIComponent(uid)+'&ledIndex='+encodeURIComponent(li)+"
        "'&barcode='+encodeURIComponent(bc)+'&notes='+encodeURIComponent(n)); ";

    chunk += "  const cd=library.find(c=>c.id==id);if(cd){cd.title=t;"
             "cd.artist=a;cd.genre=g;cd.year=Number(y)||0;cd.favorite=f;"
             "cd.uniqueID=uid;cd.ledIndices=li.split(',').map(v=>parseInt(v."
             "trim())).filter(Number.isFinite);cd.ledIndex=cd.ledIndices[0]??"
             "-1;cd.barcode=bc;cd.notes=n;filter();}";
    chunk += "  document.getElementById('edit-modal').style.display='none';"
             "showBrowserFeedback('Changes saved.','success');}catch(e){"
             "showBrowserFeedback('Save failed: '+e.message,'error');}finally{btn.disabled="
             "false;btn.textContent='SAVE CHANGES';}}";

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
    chunk += "}";

    // Only filter controls affect the physical LEDs. Text search remains a
    // browser-local operation and must not issue a device command per key.
    chunk += "function syncDeviceFilter(){";
    chunk += "  const genre=document.getElementById('filterGenre').value;";
    chunk += "  const decade=document.getElementById('filterDecade').value;";
    chunk += "  const favorite=document.getElementById('filterFav').checked;";
    chunk += "  if(genre||decade||favorite){";
    chunk += "    const genre = document.getElementById('filterGenre').value;";
    chunk +=
        "    doAction('applyfilter', '&genre=' + encodeURIComponent(genre) + "
        "'&decade=' + encodeURIComponent(decade) + '&favorites=' + favorite)"
        ".catch(e=>showBrowserFeedback('Filter failed: '+e.message,'error'));";
    chunk += "  } else {";
    chunk += "    doAction('clearfilter').catch(e=>showBrowserFeedback('Filter "
             "failed: '+e.message,'error'));";
    chunk += "  }";
    chunk += "}";

    chunk += "function select(id){doAction('select','&id='+encodeURIComponent("
             "id)).then(()=>showBrowserFeedback('Item selected on the device.',"
             "'success')).catch(e=>showBrowserFeedback('Selection failed: '+"
             "e.message,'error'));}";

    // SECURE doAction
    chunk += "async function doAction(act,params=''){";
    chunk += "  const r=await fetch('/api/control',{method:'POST',headers:{'Content-Type':"
             "'application/x-www-form-urlencoded'},body:'action='+encodeURIComponent(act)+"
             "params});const message=await r.text();if(r.status===401){location."
             "reload();throw new Error('Session expired');}if(!r.ok)throw new "
             "Error(message||('Request failed: '+r.status));return message;";
    chunk += "}";

    chunk += "</script>";

    chunk += getWebFooter();
    chunk += "</body></html>";
    server.sendContent(chunk);
    server.sendContent(""); // End chunked transfer
  });

  // 4. Backup & Restore
  server.on("/backup", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/backup", "backup manager");
      return;
    }

    String html =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Backup & "
        "Restore</title><meta name='viewport' content='width=device-width, "
        "initial-scale=1'><style>";
    html += getCommonCSS();
    html += ".backup-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}"
            ".backup-card{display:flex;flex-direction:column}.backup-card p{"
            "color:var(--sub);margin:8px 0 18px}.backup-card .action{margin-top:"
            "auto}.backup-card a{text-decoration:none}.file-picker{padding:8px;"
            "margin:8px 0 14px}.file-picker::file-selector-button{background:"
            "var(--card-strong);color:var(--text);border:1px solid var(--line);"
            "border-radius:9px;padding:9px 12px;margin-right:10px;cursor:pointer}"
            ".notice{padding:12px 13px;margin:4px 0 14px;border-radius:12px;"
            "border:1px solid rgba(255,173,66,.42);background:rgba(255,173,66,"
            ".08);color:#ffd28b;font-size:13px}code{color:var(--accent);font-"
            "family:ui-monospace,Consolas,monospace}@media(max-width:650px){"
            ".backup-grid{grid-template-columns:1fr}}</style></head><body>";
    html += "<header class='hero'><div class='eyebrow'>Data safety</div><h1>"
            "Backup &amp; restore</h1><p class='subtitle'>Keep a portable copy "
            "of your collection or restore one you saved earlier.</p></header>";
    String libFile = getLibraryFileName();
    html += "<div class='notice'>Active database: <code>" + libFile +
            "</code>. Keep the device powered while a backup is processed.</div>";

    // Export (JSONL)
    html += "<div class='backup-grid'><section class='card backup-card'>";
    html += "<div class='section-kicker'>Download</div><h2>Export library</h2>"
            "<p>Save your records as one line-delimited JSON file that you can "
            "store on another device.</p>";
    html += "<a class='action' href='/api/export_backup' download='library_"
            "backup.jsonl'><button type='button'>Download full backup</button>"
            "</a></section>";

    // Import (JSONL)
    html += "<section class='card backup-card'><div class='section-kicker'>"
            "Upload</div><h2>Restore backup</h2><p>Select a <code>.jsonl</code> "
            "backup. Matching records may be overwritten and new records will "
            "be added.</p>";
    html += "<form id='restoreForm' method='POST' action='/api/import_backup' "
            "enctype='multipart/form-data'><label for='restoreFile'>Backup "
            "file</label><input class='file-picker' id='restoreFile' type='file' "
            "name='data' accept='.jsonl,application/x-ndjson'>";
    html += "<button class='btn-danger' type='submit' id='btnR' disabled>"
            "Restore from backup</button></form></section></div>";

    html += "<div id='restore-feedback' class='feedback' role='status' aria-"
            "live='polite'></div><script>const file=document.getElementById("
            "'restoreFile'),form=document.getElementById('restoreForm'),btn="
            "document.getElementById('btnR'),feedback=document.getElementById("
            "'restore-feedback');file.addEventListener('change',()=>{btn.disabled="
            "!file.files.length;feedback.className='feedback';});form.addEventListener("
            "'submit',()=>{btn.disabled=true;btn.innerHTML='<span class=\"spinner\">"
            "</span>Uploading backup';feedback.textContent='Uploading and "
            "validating your backup. Keep this page open.';feedback.className="
            "'feedback show';});</script>";

    html += getWebFooter();
    html += "</body></html>";
    server.send(200, "text/html; charset=utf-8", html);
  });

  // 5. Export Backup (JSONL)
  server.on("/api/export_backup", HTTP_GET, []() {
    if (!requireWebAuth())
      return;

    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"library_backup.jsonl\"");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/ndjson", ""); // Newline Delimited JSON

    // Snapshot the lightweight RAM records so a worker update cannot invalidate
    // an iterator while the HTTP response is being streamed.
    CDVector cdSnapshot;
    BookVector bookSnapshot;
    if (libraryMutex)
      xSemaphoreTakeRecursive(libraryMutex, portMAX_DELAY);
    cdSnapshot = cdLibrary;
    bookSnapshot = bookLibrary;
    if (libraryMutex)
      xSemaphoreGiveRecursive(libraryMutex);

    // Export CDs
    for (const auto &item : cdSnapshot) {
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
        data["coverUrl"] = fullCD.coverUrl.c_str();
        data["coverFile"] = fullCD.coverFile.c_str();
        data["favorite"] = fullCD.favorite;
        data["notes"] = fullCD.notes.c_str();
        data["barcode"] = fullCD.barcode.c_str();
        data["trackCount"] = fullCD.trackCount;
        data["totalDurationMs"] = fullCD.totalDurationMs;
        data["releaseMbid"] = fullCD.releaseMbid.c_str();
        JsonArray cdLeds = data.createNestedArray("ledIndices");
        for (int led : fullCD.ledIndices)
          cdLeds.add(led);

        // Tracks are stored separately and not included in basic backup

        // Export CD
        String line;
        serializeJson(doc, line);
        server.sendContent(line + "\n");

        // NEW: Export Tracklist if available
        if (fullCD.releaseMbid.length() > 0) {
          TrackList *tl = Storage.loadTracklist(fullCD.releaseMbid.c_str());
          if (tl) {
            BasicJsonDocument<SpiRamAllocator> tlDoc(65536);
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
            if (!tlDoc.overflowed()) {
              serializeJson(tlDoc, tlLine);
              server.sendContent(tlLine + "\n");
            }
            delete tl;
          }
        }
      }
      delay(1); // Keep the HTTP/idle tasks serviced during large exports.
    }

    // Export Books
    for (const auto &item : bookSnapshot) {
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
        data["coverUrl"] = fullBook.coverUrl.c_str();
        data["coverFile"] = fullBook.coverFile.c_str();
        data["favorite"] = fullBook.favorite;
        data["notes"] = fullBook.notes.c_str();
        data["isbn"] = fullBook.isbn.c_str();
        data["pageCount"] = fullBook.pageCount;
        data["currentPage"] = fullBook.currentPage;
        data["publisher"] = fullBook.publisher.c_str();
        JsonArray bookLeds = data.createNestedArray("ledIndices");
        for (int led : fullBook.ledIndices)
          bookLeds.add(led);

        String line;
        serializeJson(doc, line);
        server.sendContent(line + "\n");
      }
      delay(1);
    }

    server.sendContent("");
  });

  // 6. Import Backup (JSONL)
  server.on(
      "/api/import_backup", HTTP_POST,
      []() {
        if (!backupUploadAuthorized || backupUploadFailed ||
            !webRequestAuthorized()) {
          if (i2cMutex && xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) ==
                              pdPASS) {
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, LOW);
            if (SD.exists("/restore.jsonl"))
              SD.remove("/restore.jsonl");
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, HIGH);
            xSemaphoreGiveRecursive(i2cMutex);
          }
          return server.send(backupUploadAuthorized ? 400 : 401, "text/plain",
                             backupUploadFailed ? "Upload rejected or too large"
                                                : "Unauthorized");
        }

        if (!BackgroundWorker::addJob({JOB_BACKUP_IMPORT, "/restore.jsonl", -1,
                                       "", nullptr, true})) {
          return server.send(503, "text/plain",
                             "Background queue is full; try again");
        }
        String importPage = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                            "<meta name='viewport' content='width=device-width,"
                            "initial-scale=1'><meta http-equiv='refresh' content='"
                            "12;url=/backup'><title>Import started</title><style>";
        importPage += getCommonCSS();
        importPage += "body{display:grid;place-items:center;text-align:center}."
                      "import-card{width:100%;max-width:460px}.import-card .spinner"
                      "{width:28px;height:28px;border-width:3px;margin:0 0 16px}"
                      "</style></head><body><main class='card import-card'><span "
                      "class='spinner'></span><div class='eyebrow'>Backup restore"
                      "</div><h1>Import started</h1><p class='subtitle'>The device"
                      " is validating the file in the background. It will restart"
                      " automatically after a successful import.</p></main></body>"
                      "</html>";
        server.send(202, "text/html; charset=utf-8", importPage);
      },
      []() {
        // 2. Upload Handler
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          backupUploadAuthorized = webRequestAuthorized();
          backupUploadFailed = false;
          backupUploadBytes = 0;
          if (!backupUploadAuthorized)
            return;
          if (!i2cMutex || xSemaphoreTakeRecursive(i2cMutex,
                                                   pdMS_TO_TICKS(1000)) != pdPASS) {
            backupUploadFailed = true;
            return;
          }
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, LOW);
          if (SD.exists("/restore.jsonl"))
            SD.remove("/restore.jsonl");
          File restore = SD.open("/restore.jsonl", FILE_WRITE);
          if (!restore)
            backupUploadFailed = true;
          else
            restore.close();
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
          xSemaphoreGiveRecursive(i2cMutex);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (!backupUploadAuthorized || backupUploadFailed)
            return;
          backupUploadBytes += upload.currentSize;
          if (backupUploadBytes > MAX_BACKUP_UPLOAD_BYTES) {
            backupUploadFailed = true;
            return;
          }
          if (!i2cMutex || xSemaphoreTakeRecursive(i2cMutex,
                                                   pdMS_TO_TICKS(1000)) != pdPASS) {
            backupUploadFailed = true;
            return;
          }
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, LOW);
          File restore = SD.open("/restore.jsonl", FILE_APPEND);
          if (!restore || restore.write(upload.buf, upload.currentSize) !=
                              upload.currentSize)
            backupUploadFailed = true;
          if (restore) {
            restore.flush();
            restore.close();
          }
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
          xSemaphoreGiveRecursive(i2cMutex);
        } else if (upload.status == UPLOAD_FILE_END) {
          // Each upload chunk is already flushed under a short SD lock.
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          backupUploadFailed = true;
          if (i2cMutex && xSemaphoreTakeRecursive(i2cMutex,
                                                  pdMS_TO_TICKS(1000)) == pdPASS) {
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, LOW);
            if (SD.exists("/restore.jsonl"))
              SD.remove("/restore.jsonl");
            if (sdExpander)
              sdExpander->digitalWrite(SD_CS, HIGH);
            xSemaphoreGiveRecursive(i2cMutex);
          }
        }
      });

  // 3. User Manual
  server.on("/manual", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/manual", "manual");
      return;
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    String chunk =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Digital "
        "Librarian Manual</title><meta name='viewport' "
        "content='width=device-width, initial-scale=1'><style>";
    chunk += getCommonCSS();
    chunk +=
        ".manual-head{display:flex;align-items:flex-end;justify-content:space-"
        "between;gap:18px}.manual-head button{width:auto;min-width:120px}.manual-"
        "grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:"
        "16px}.manual-section{margin:0}.manual-section h2{margin-bottom:14px}."
        "manual-section h3{color:var(--accent);margin:16px 0 5px;font-size:14px}"
        ".manual-section p{color:var(--sub);margin:7px 0}.manual-section ul{"
        "padding-left:20px;color:var(--sub)}.manual-section li{margin:8px 0}."
        "manual-section strong{color:var(--text)}code{padding:2px 6px;border-"
        "radius:5px;background:rgba(4,13,24,.65);color:var(--accent);font-family:"
        "ui-monospace,Consolas,monospace}@media(max-width:650px){.manual-grid{"
        "grid-template-columns:1fr}.manual-head{align-items:flex-start;flex-"
        "direction:column}.manual-head button{width:100%}}@media print{body{"
        "max-width:none;background:#fff;color:#000;padding:0}.hero{padding:0 0 "
        "16px}.card{box-shadow:none;border:1px solid #bbb;background:#fff;break-"
        "inside:avoid}.manual-section p,.manual-section ul,.subtitle{color:#222}"
        ".manual-section strong,h1,h2{color:#000}.no-print{display:none}}"
        "</style></head><body>";
    server.sendContent(chunk);
    chunk = "<header class='hero manual-head'><div><div class='eyebrow'>Help "
            "center</div><h1>Digital Librarian manual</h1><p class='subtitle'>"
            "A quick guide to the touchscreen and web tools.</p></div><button "
            "class='btn-secondary no-print' onclick='window.print()'>Print "
            "guide</button></header><div class='manual-grid'>";
    chunk += "<section class='card manual-section'><div class='section-kicker'>"
             "01</div><h2>Getting started</h2><p><strong>Power:</strong> Connect "
             "the device to a stable USB-C power source.</p><p><strong>WiFi:</"
             "strong> Add or select a saved network in Device Settings. The "
             "WiFi icon shows the current connection state.</p></section>";
    server.sendContent(chunk);
    chunk = "<section class='card manual-section'><div class='section-kicker'>"
            "02</div><h2>Touchscreen controls</h2><ul><li><strong>Search:</"
            "strong> Find records with the on-screen keyboard.</li><li><strong>"
            "Add (+):</strong> Create a record or fetch its metadata.</li><li>"
            "<strong>Track list:</strong> Open songs and lyrics.</li><li><strong>"
            "Shuffle:</strong> Pick a random record.</li><li><strong>Filter:</"
            "strong> Narrow by genre, decade, or favorites.</li><li><strong>"
            "Sync:</strong> Complete missing online data.</li><li><strong>Eye:"
            "</strong> Toggle the shelf locator LED.</li></ul></section>";
    server.sendContent(chunk);
    chunk = "<section class='card manual-section'><div class='section-kicker'>"
            "03</div><h2>Web tools</h2><h3>Remote library</h3><p>Search, filter, "
            "select, and edit records from a phone or computer.</p><h3>Code "
            "scanner</h3><p>Scan one or several barcodes to add records.</p><h3>"
            "Cover manager</h3><p>Attach an image URL to a record when automatic "
            "artwork is unavailable.</p></section>";
    server.sendContent(chunk);
    chunk = "<section class='card manual-section'><div class='section-kicker'>"
            "04</div><h2>Data and maintenance</h2><ul><li><strong>Lyrics:</"
            "strong> Synced and cached from LRCLib when WiFi is available.</li>"
            "<li><strong>Favorites:</strong> Highlight important records and "
            "tracks.</li><li><strong>Backups:</strong> Export before major edits "
            "and restore from a trusted <code>.jsonl</code> file.</li><li><strong>"
            "Diagnostics:</strong> Review memory and error history on the Health "
            "page.</li><li><strong>Storage:</strong> Library data is stored in "
            "<code>/db/</code>.</li></ul></section></div>";

    chunk += "<div class='no-print'>";
    chunk += getWebFooter();
    chunk += "</div></body></html>";
    server.sendContent(chunk);
    server.sendContent("");
  });

  // Error Dashboard Page
  server.on("/errors", HTTP_GET, []() {
    if (!webRequestAuthorized()) {
      sendWebLoginPage("/errors", "diagnostics dashboard");
      return;
    }
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html +=
        "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>Error Dashboard</title>";
    html += "<style>";
    html += getCommonCSS();
    html += "body{max-width:1040px}.health-stats{display:grid;grid-template-"
            "columns:repeat(4,minmax(0,1fr));gap:10px;margin-bottom:16px}.health-"
            "stat{min-width:0;padding:16px;border:1px solid var(--line-soft);"
            "border-radius:14px;background:rgba(4,13,24,.48)}.health-stat h3{"
            "margin-bottom:8px;color:var(--sub);font-size:11px;letter-spacing:"
            ".08em;text-transform:uppercase}.health-stat .value{overflow:hidden;"
            "text-overflow:ellipsis;color:var(--accent);font-size:clamp(20px,4vw,"
            "28px);font-weight:850}.controls{display:flex;align-items:center;gap:"
            "10px;flex-wrap:wrap;margin-bottom:14px}.controls button{width:auto}."
            "auto-refresh{display:flex;align-items:center;gap:8px;margin:0 0 0 "
            "auto;color:var(--text)}.auto-refresh input{width:auto;min-height:0;"
            "margin:0;accent-color:var(--accent)}.error-table{overflow:auto;"
            "border:1px solid var(--line-soft);border-radius:14px;background:"
            "rgba(4,13,24,.42)}table{width:100%;min-width:680px;border-collapse:"
            "collapse}th{padding:12px;text-align:left;background:rgba(23,52,79,"
            ".72);color:var(--sub);font-size:11px;letter-spacing:.07em;text-"
            "transform:uppercase}td{padding:12px;border-top:1px solid var(--line-"
            "soft);vertical-align:top;font-size:13px}";
    html += ".badge{display:inline-block;padding:4px "
            "8px;border-radius:999px;font-size:10px;font-weight:850;}";
    html += ".badge.info{background:rgba(83,199,255,.2);color:var(--cyan)}";
    html += ".badge.warn{background:rgba(255,173,66,.2);color:#ffc36f}";
    html += ".badge.error,.badge.fatal{background:rgba(255,101,95,.2);color:"
            "#ffaaa6}.empty{text-align:center;padding:40px;color:var(--sub)}.last-"
            "updated{text-align:center;margin-top:14px;color:var(--sub);font-size:"
            "12px}@media(max-width:720px){.health-stats{grid-template-columns:1fr "
            "1fr}.auto-refresh{margin-left:0;width:100%}}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'><header class='hero'><div class='eyebrow'>"
            "System health</div><h1>Diagnostics</h1><p class='subtitle'>Review "
            "device memory, uptime, and recent error history.</p></header>";

    html += "<section class='card'><div class='health-stats'>";
    html += "<div class='health-stat'><h3>Free heap</h3><div class='value' "
             "id='freeHeap'>-</div></div>";
    html += "<div class='health-stat'><h3>Lowest heap</h3><div class='value' "
             "id='minHeap'>-</div></div>";
    html += "<div class='health-stat'><h3>Uptime</h3><div class='value' "
             "id='uptime'>-</div></div>";
    html += "<div class='health-stat'><h3>Logged events</h3><div class='value' "
             "id='errorCount'>-</div></div>";
    html += "</div>";

    html += "<div class='controls'>";
    html += "<button onclick='refresh()'>Refresh now</button>";
    html += "<button onclick='clearErrors()' class='btn-danger'>Clear "
            "history</button>";
    html += "<label class='auto-refresh'><input type='checkbox' id='autoRefresh'> Auto-refresh "
             "(5s)</label>";
    html += "</div>";
    html += "<div id='health-feedback' class='feedback' role='status' aria-"
            "live='polite'></div>";

    html += "<div class='error-table'>";
    html += "<table><thead><tr>";
    html += "<th>Time</th><th>Level</th><th>Category</th><th>Message</"
            "th><th>Context</th>";
    html += "</tr></thead><tbody id='errorTable'></tbody></table>";
    html += "</div>";

    html += "<p class='last-updated'>Last updated: <span id='lastUpdate'>-</"
            "span></p></section></div>";

    html += "<script>";
    html += "const levels=['INFO','WARN','ERROR','FATAL'];";
    html += "const "
            "categories=['NETWORK','STORAGE','API','PARSING','MEMORY','"
            "HARDWARE','SYSTEM'];";
    html += "let autoRefreshInterval=null;";
    html += "const healthFeedback=document.getElementById('health-feedback');"
            "function showHealthFeedback(message,type=''){healthFeedback."
            "textContent=message;healthFeedback.className='feedback show '+type;}";
    html += "const esc=s=>String(s??'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));";

    html += "async function loadErrors(){try{";
    html += "  const res=await fetch('/api/errors');if(res.status===401){"
            "location.reload();return;}if(!res.ok)throw new Error(await res."
            "text()||('Request failed: '+res.status));";
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
        "${badge}'>${level}</span></td><td>${cat}</td><td>${esc(e.message)}</td><td "
        "style='color:#888;font-size:11px'>${esc(e.context||'-')}</td></tr>`;";
    html += "    }).reverse().join('');";
    html += "  }";
    html += "  document.getElementById('lastUpdate').textContent=new "
            "Date().toLocaleTimeString();";
    html += "}catch(e){document.getElementById('lastUpdate').textContent="
            "'Refresh failed';showHealthFeedback('Could not load diagnostics: '+"
            "e.message,'error');}}";

    html += "async function clearErrors(){";
    html += "  if(!confirm('Clear all error logs?'))return;";
    html += "  try{const res=await fetch('/api/errors/clear',{method:'POST'});"
            "if(res.status===401){location.reload();return;}if(!res.ok)throw "
            "new Error(await res.text()||'Clear failed');await loadErrors();}"
            "catch(e){showHealthFeedback('Clear failed: '+e.message,'error');}";
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

    html += getWebFooter();

    html += "</body></html>";

    server.send(200, "text/html; charset=utf-8", html);
  });

  server.on("/restart", HTTP_POST, []() {
    if (!requireWebAuth())
      return;
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
  Serial.println("[UPLOAD SMOKE TEST] Digital Librarian build 2026-08-31");
  boot_reset_reason = (int)esp_reset_reason();
  Serial.printf("[BOOT] ESP reset reason: %d\n", boot_reset_reason);
  RuntimeDiagnostics::begin(boot_reset_reason);
  logMemoryUsage("BOOT START");

  logMemoryUsage("BOOT START");

  libraryMutex = xSemaphoreCreateRecursiveMutex();
  i2cMutex = xSemaphoreCreateRecursiveMutex();
  ledMutex = xSemaphoreCreateMutex();

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
  sd_card_ready = mountLibrarySdCard();
  if (sd_card_ready) {
    Serial.println("✅ SD Card Mounted");
    Storage.begin();

    Serial.println("Creating loading screen...");
    // Show loading screen before syncing library
    lvgl_port_lock(-1);
    Serial.println("LVGL lock acquired");

    // Create a startup surface that matches the application shell.
    lv_obj_t *loading_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(loading_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(loading_bg, 0, 0);
    lv_obj_set_style_bg_color(loading_bg, lv_color_hex(0x090D12), 0);
    lv_obj_set_style_border_width(loading_bg, 0, 0);
    lv_obj_set_style_radius(loading_bg, 0, 0);
    lv_obj_set_style_pad_all(loading_bg, 0, 0);
    lv_obj_clear_flag(loading_bg, LV_OBJ_FLAG_SCROLLABLE);
    Serial.println("Background created");

    lv_obj_t *loading_card = lv_obj_create(loading_bg);
    lv_obj_set_size(loading_card, 520, 220);
    lv_obj_center(loading_card);
    lv_obj_set_style_bg_color(loading_card, lv_color_hex(0x111820), 0);
    lv_obj_set_style_border_color(loading_card, lv_color_hex(0x263544), 0);
    lv_obj_set_style_border_width(loading_card, 1, 0);
    lv_obj_set_style_radius(loading_card, 16, 0);
    lv_obj_set_style_shadow_color(loading_card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(loading_card, 28, 0);
    lv_obj_set_style_shadow_opa(loading_card, LV_OPA_40, 0);
    lv_obj_clear_flag(loading_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *accent = lv_obj_create(loading_card);
    lv_obj_set_size(accent, 520, 5);
    lv_obj_set_pos(accent, -1, -1);
    lv_obj_set_style_bg_color(accent, lv_color_hex(getCurrentThemeColor()), 0);
    lv_obj_set_style_border_width(accent, 0, 0);
    lv_obj_set_style_radius(accent, 0, 0);
    lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(loading_card);
    lv_label_set_text(brand, setting_brand_title.c_str());
    lv_obj_set_pos(brand, 32, 34);
    lv_obj_set_style_text_color(brand, lv_color_hex(0xF4F7FA), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_16, 0);

    lv_obj_t *loading_label = lv_label_create(loading_card);
    lv_label_set_text(loading_label, "LOADING YOUR LIBRARY");
    lv_obj_set_pos(loading_label, 92, 104);
    lv_obj_set_style_text_color(loading_label,
                                lv_color_hex(getCurrentThemeColor()), 0);
    lv_obj_set_style_text_font(loading_label, &lv_font_montserrat_16, 0);

    lv_obj_t *loading_hint = lv_label_create(loading_card);
    lv_label_set_text(loading_hint, "Reading the collection and preparing navigation...");
    lv_obj_set_pos(loading_hint, 92, 142);
    lv_obj_set_width(loading_hint, 390);
    lv_obj_set_style_text_color(loading_hint, lv_color_hex(0x98A6B5), 0);
    lv_label_set_long_mode(loading_hint, LV_LABEL_LONG_WRAP);

    lv_obj_t *spinner = lv_spinner_create(loading_card, 900, 76);
    lv_obj_set_size(spinner, 44, 44);
    lv_obj_set_pos(spinner, 32, 104);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x263544), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(getCurrentThemeColor()),
                               LV_PART_INDICATOR);
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
  if (!leds) {
    static CRGB fallbackLed;
    ErrorHandler::logFatal(ERR_CAT_MEMORY,
                           "LED buffer allocation failed; using one safe pixel",
                           "setup");
    leds = &fallbackLed;
    led_count = 1;
  }
  FastLED.addLeds<WS2812B, LED_PIN, COLOR_ORDER>(leds, led_count);
  FastLED.setBrightness(led_brightness);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, led_max_milliamps);
  FastLED.clear(true);

  // 5. Network
  Serial.println("Network Init...");
  AppNetworkManager::init();
  AppNetworkManager::startConnection();

  Serial.println("Web Handlers...");
  setupWebHandlers();

  Serial.println("Server Begin...");
  server.begin();

  // The worker is started only after storage, network, and the web server are
  // ready. Starting it just before the UI allows the first paint to queue an
  // optional detail read without ever touching SD from the LVGL task.
  Serial.println("Starting Background Worker...");
  BackgroundWorker::begin();

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
}

// ========================================
// ARDUINO LOOP
// ========================================
void loop() {
  AppNetworkManager::serviceConnection();
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
    if (ledMutex)
      xSemaphoreTake(ledMutex, portMAX_DELAY);
    FastLED.clear();
    FastLED.show();
    if (ledMutex)
      xSemaphoreGive(ledMutex);
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
