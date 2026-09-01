// NetworkManager.cpp

#include "NetworkManager.h"
#include "TlsTrust.h"
#include "AppGlobals.h"
#include "ErrorHandler.h"
#include <ESPmDNS.h>
#include <esp_heap_caps.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {
int connectionNetworkIndex = -1;
unsigned long connectionAttemptStarted = 0;
bool connectionServiceActive = false;
bool networkReadyAnnounced = false;
constexpr uint8_t WIFI_CREDENTIAL_SEED_VERSION = 3;
constexpr unsigned long SAVED_NETWORK_ATTEMPT_TIMEOUT_MS = 30000;

void beginNetworkAttempt(int index) {
  connectionNetworkIndex = index;
  connectionAttemptStarted = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedWiFiNetworks[index].ssid.c_str(),
             savedWiFiNetworks[index].password.c_str());
  Serial.printf("Trying WiFi: %s\n", savedWiFiNetworks[index].ssid.c_str());
}

void startSetupAccessPoint() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  char password[16];
  snprintf(password, sizeof(password), "DL%08lX",
           (unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFF));
  WiFi.softAP("DigitalLibrarian_Setup", password);
  Serial.printf("Setup AP ready at %s, password: %s\n",
                WiFi.softAPIP().toString().c_str(), password);
}
} // namespace

void AppNetworkManager::init() {
  // Initialize in station mode
  WiFi.mode(WIFI_STA);
  loadWiFiNetworks();
  Serial.println("AppNetworkManager Initialized");
}

bool AppNetworkManager::isConnected() { return WiFi.status() == WL_CONNECTED; }

String AppNetworkManager::getLocalIP() { return WiFi.localIP().toString(); }

void AppNetworkManager::loadWiFiNetworks() {
  preferences.begin("wifi", true); // Open in read-only mode

  savedWiFiNetworks.clear();

  // Load network count
  int count = preferences.getInt("count", 0);
  const uint8_t credentialSeedVersion =
      preferences.getUChar("seed_ver", 0);

  if (count > 0) {
    Serial.printf("✅ Loading %d saved WiFi networks from flash\n", count);

    for (int i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
      WiFiNetwork net;
      String ssidKey = "ssid" + String(i);
      String passKey = "pass" + String(i);

      net.ssid = preferences.getString(ssidKey.c_str(), "");
      net.password = preferences.getString(passKey.c_str(), "");

      if (net.ssid.length() > 0) {
        savedWiFiNetworks.push_back(net);
        Serial.printf("   %d. %s\n", i + 1, net.ssid.c_str());
      }
    }
  } else {
    Serial.println("ℹ️  No saved WiFi networks found");
    // Add default network to the list
    WiFiNetwork defaultNet;
    defaultNet.ssid = String(DEFAULT_SSID);
    defaultNet.password = String(DEFAULT_PASSWORD);
    savedWiFiNetworks.push_back(defaultNet);
    Serial.printf("   Added default network: %s\n", DEFAULT_SSID);
  }

  preferences.end();

#if (defined(WIFI_SSID_2) && defined(WIFI_PASSWORD_2)) ||                  \
    (defined(WIFI_SSID_3) && defined(WIFI_PASSWORD_3))
  // Provision optional additional credentials once. Keeping the marker in the
  // same NVS namespace lets users remove a network later without it being
  // silently restored on every boot.
  if (credentialSeedVersion < WIFI_CREDENTIAL_SEED_VERSION) {
    auto seedNetwork = [](const char *ssid, const char *password) {
      if (!ssid || String(ssid).isEmpty())
        return;

      for (WiFiNetwork &network : savedWiFiNetworks) {
        if (network.ssid == ssid) {
          network.password = password;
          return;
        }
      }

      if (savedWiFiNetworks.size() >= MAX_WIFI_NETWORKS)
        savedWiFiNetworks.erase(savedWiFiNetworks.begin());
      savedWiFiNetworks.push_back({String(ssid), String(password)});
    };

#if defined(WIFI_SSID_2) && defined(WIFI_PASSWORD_2)
    seedNetwork(WIFI_SSID_2, WIFI_PASSWORD_2);
#endif
#if defined(WIFI_SSID_3) && defined(WIFI_PASSWORD_3)
    seedNetwork(WIFI_SSID_3, WIFI_PASSWORD_3);
#endif
    saveWiFiNetworks();
  }
#endif
}

void AppNetworkManager::saveWiFiNetworks() {
  preferences.begin("wifi", false); // Open in read-write mode

  // Clear old data
  preferences.clear();

  // Save network count
  int count = savedWiFiNetworks.size();
  if (count > MAX_WIFI_NETWORKS)
    count = MAX_WIFI_NETWORKS;

  preferences.putInt("count", count);
  preferences.putUChar("seed_ver", WIFI_CREDENTIAL_SEED_VERSION);

  Serial.printf("✅ Saving %d WiFi networks to flash\n", count);

  // Save each network
  for (int i = 0; i < count; i++) {
    String ssidKey = "ssid" + String(i);
    String passKey = "pass" + String(i);

    preferences.putString(ssidKey.c_str(), savedWiFiNetworks[i].ssid);
    preferences.putString(passKey.c_str(), savedWiFiNetworks[i].password);

    Serial.printf("   %d. %s\n", i + 1, savedWiFiNetworks[i].ssid.c_str());
  }

  preferences.end();
}

void AppNetworkManager::addWiFiNetwork(String ssid, String password) {
  // Check if network already exists (update password if so)
  for (int i = 0; i < savedWiFiNetworks.size(); i++) {
    if (savedWiFiNetworks[i].ssid == ssid) {
      Serial.printf("📝 Updating password for existing network: %s\n",
                    ssid.c_str());
      savedWiFiNetworks[i].password = password;
      saveWiFiNetworks();
      return;
    }
  }

  // Add new network
  if (savedWiFiNetworks.size() >= MAX_WIFI_NETWORKS) {
    Serial.println("⚠️ Maximum networks reached, removing oldest");
    savedWiFiNetworks.erase(savedWiFiNetworks.begin());
  }

  WiFiNetwork newNet;
  newNet.ssid = ssid;
  newNet.password = password;
  savedWiFiNetworks.push_back(newNet);

  Serial.printf("➕ Added new network: %s\n", ssid.c_str());
  saveWiFiNetworks();
}

void AppNetworkManager::removeWiFiNetwork(int index) {
  if (index >= 0 && index < savedWiFiNetworks.size()) {
    Serial.printf("➖ Removing network: %s\n",
                  savedWiFiNetworks[index].ssid.c_str());
    savedWiFiNetworks.erase(savedWiFiNetworks.begin() + index);
    saveWiFiNetworks();
  }
}

bool AppNetworkManager::tryConnectToSavedNetworks() {
  if (savedWiFiNetworks.size() == 0) {
    Serial.println("⚠️ No saved networks to try");
    return false;
  }

  // Compatibility wrapper for older callers. Connection attempts are serviced
  // incrementally from loop(), never by waiting here on the UI/application
  // thread.
  startConnection();
  return WiFi.status() == WL_CONNECTED;
}

void AppNetworkManager::startConnection() {
  networkReadyAnnounced = false;
  if (savedWiFiNetworks.empty()) {
    startSetupAccessPoint();
    connectionServiceActive = false;
    return;
  }
  WiFi.setAutoReconnect(false);
  connectionServiceActive = true;
  beginNetworkAttempt(0);
}

bool AppNetworkManager::isConnectionInProgress() {
  return connectionServiceActive;
}

int AppNetworkManager::getConnectionNetworkIndex() {
  return connectionNetworkIndex;
}

void AppNetworkManager::cancelConnectionAttempts() {
  connectionServiceActive = false;
  connectionNetworkIndex = -1;
}

void AppNetworkManager::completeManualConnection() {
  cancelConnectionAttempts();
  WiFi.setAutoReconnect(true);
  if (!networkReadyAnnounced) {
    networkReadyAnnounced = true;
    if (mdns_name.length() == 0)
      mdns_name = "digitallibrarian";
    MDNS.begin(mdns_name.c_str());
  }
}

void AppNetworkManager::serviceConnection() {
  if (!connectionServiceActive)
    return;
  if (WiFi.status() == WL_CONNECTED) {
    connectionServiceActive = false;
    WiFi.setAutoReconnect(true);
    if (!networkReadyAnnounced) {
      networkReadyAnnounced = true;
      Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
      if (mdns_name.length() == 0)
        mdns_name = "digitallibrarian";
      MDNS.begin(mdns_name.c_str());
    }
    return;
  }
  if (millis() - connectionAttemptStarted <
      SAVED_NETWORK_ATTEMPT_TIMEOUT_MS)
    return;

  Serial.printf("WiFi attempt timed out: %s\n",
                savedWiFiNetworks[connectionNetworkIndex].ssid.c_str());
  WiFi.disconnect(false, false);
  const int next = connectionNetworkIndex + 1;
  if (next < (int)savedWiFiNetworks.size()) {
    beginNetworkAttempt(next);
  } else {
    ErrorHandler::logWarn(ERR_CAT_NETWORK,
                          "Saved networks unavailable; starting setup AP",
                          "serviceConnection");
    startSetupAccessPoint();
    connectionServiceActive = false;
  }
}

String AppNetworkManager::fetchURL(String url, int timeout) {
  if (WiFi.status() != WL_CONNECTED)
    return "";

  HTTPClient http;
  WiFiClientSecure clientSecure;
  WiFiClient clientInsecure;

  if (url.startsWith("https://")) {
    configureTrustedTlsClient(clientSecure);
    http.begin(clientSecure, url);
  } else {
    http.begin(clientInsecure, url);
  }

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(timeout);

  int httpCode = http.GET();
  String payload = "";

  if (httpCode == HTTP_CODE_OK) {
    payload = http.getString();
  } else {
    Serial.printf("API Error: %d for %s\n", httpCode, url.c_str());
  }
  http.end();
  return payload;
}

bool AppNetworkManager::downloadCoverImage(const String &url,
                                           const String &savePath) {
  static constexpr int MAX_COVER_BYTES = 2 * 1024 * 1024;
  if (WiFi.status() != WL_CONNECTED)
    return false;
  if (url.isEmpty())
    return false;

  HTTPClient http;
  WiFiClientSecure clientSecure;
  WiFiClient clientInsecure;

  if (url.startsWith("https://")) {
    configureTrustedTlsClient(clientSecure);
    http.begin(clientSecure, url);
  } else {
    http.begin(clientInsecure, url);
  }

  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  int len = http.getSize();
  String contentType = http.header("Content-Type");
  contentType.toLowerCase();
  if (len <= 0 || len > MAX_COVER_BYTES ||
      (!contentType.isEmpty() && contentType.indexOf("image/jpeg") < 0 &&
       contentType.indexOf("image/jpg") < 0 &&
       contentType.indexOf("application/octet-stream") < 0)) {
    http.end();
    return false;
  }

  // 1. Download to PSRAM first (No I2C buffering needed)
  uint8_t *downloadBuffer =
      (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!downloadBuffer) {
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int totalRead = 0;
  unsigned long startT = millis();
  while (http.connected() && totalRead < len && (millis() - startT < 20000)) {
    if (stream->available()) {
      int readSize = stream->read(downloadBuffer + totalRead, len - totalRead);
      if (readSize > 0)
        totalRead += readSize;
    }
    delay(1);
  }
  http.end();

  if (totalRead < len || totalRead < 4 || downloadBuffer[0] != 0xFF ||
      downloadBuffer[1] != 0xD8) {
    heap_caps_free(downloadBuffer);
    return false;
  }

  // 2. Write to SD with exclusive lock (Rapid block write)
  bool success = false;
  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    const String tmpPath = savePath + ".tmp";
    const String backupPath = savePath + ".bak";
    if (SD.exists(tmpPath))
      SD.remove(tmpPath);
    File file = SD.open(tmpPath.c_str(), FILE_WRITE);
    if (file) {
      size_t written = file.write(downloadBuffer, totalRead);
      file.flush();
      const bool writeOk = file.getWriteError() == 0;
      file.close();
      if (writeOk && written == (size_t)totalRead) {
        if (SD.exists(backupPath))
          SD.remove(backupPath);
        const bool hadOriginal = SD.exists(savePath);
        const bool backedUp = !hadOriginal || SD.rename(savePath, backupPath);
        success = backedUp && SD.rename(tmpPath, savePath);
        if (!success && hadOriginal)
          SD.rename(backupPath, savePath);
        if (success && hadOriginal)
          SD.remove(backupPath);
      }
      if (!success)
        SD.remove(tmpPath);
    }

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);
  }

  heap_caps_free(downloadBuffer);
  return success;
}

void AppNetworkManager::forceUpdateWLED() {
  if (!led_use_wled)
    return;
  if (WiFi.status() != WL_CONNECTED)
    return;

  // Sync WLED with local LED state (WYSIWYG)
  WiFiClient client;
  HTTPClient http;

  String url = "http://" + wled_ip + "/json/state";
  http.begin(client, url);
  http.setTimeout(wled_timeout_ms);
  http.addHeader("Content-Type", "application/json");

  // Build JSON: Clear all, then set active pixels
  // format: {"seg":{"i":[start, stop, "000000", idx1, "RRGGBB", idx2, "RRGGBB",
  // ...]}}
  String json = "{\"seg\":{\"i\":[0," + String(led_count) + ",\"000000\"";

  int activeCount = 0;
  if (ledMutex)
    xSemaphoreTake(ledMutex, portMAX_DELAY);
  for (int i = 0; i < led_count; i++) {
    if (leds[i].r > 0 || leds[i].g > 0 || leds[i].b > 0) {
      char hex[8];
      sprintf(hex, "%02X%02X%02X", leds[i].r, leds[i].g, leds[i].b);
      json += "," + String(i) + ",\"" + String(hex) + "\"";
      activeCount++;
      if (activeCount > 150)
        break;
    }
  }
  if (ledMutex)
    xSemaphoreGive(ledMutex);
  json += "]}}";

  int httpCode = http.POST(json);
  if (httpCode <= 0) {
    Serial.printf("WLED Error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}
