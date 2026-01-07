// NetworkManager.cpp

#include "NetworkManager.h"
#include "AppGlobals.h"
#include "ErrorHandler.h"
#include <esp_heap_caps.h>

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

  Serial.printf("🔍 Trying %d saved WiFi networks...\n",
                savedWiFiNetworks.size());

  for (int i = 0; i < savedWiFiNetworks.size(); i++) {
    WiFiNetwork &net = savedWiFiNetworks[i];

    Serial.printf("   %d/%d Trying: %s ... ", i + 1, savedWiFiNetworks.size(),
                  net.ssid.c_str());

    WiFi.begin(net.ssid.c_str(), net.password.c_str());

    // Wait for connection (max 10 seconds per network)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n✅ Connected to: %s\n", net.ssid.c_str());
      Serial.printf("   IP: %s\n", WiFi.localIP().toString().c_str());
      return true;
    } else {
      ErrorHandler::logWarn(ERR_CAT_NETWORK,
                            String("Failed to connect to: ") + net.ssid,
                            "tryConnectToSavedNetworks");
      Serial.println(" ❌ Failed");
      WiFi.disconnect();
    }
  }

  ErrorHandler::logError(ERR_CAT_NETWORK,
                         String("Could not connect to any of ") +
                             String(savedWiFiNetworks.size()) +
                             " saved networks",
                         "tryConnectToSavedNetworks");
  Serial.println("❌ Could not connect to any saved network");
  return false;
}

String AppNetworkManager::fetchURL(String url, int timeout) {
  if (WiFi.status() != WL_CONNECTED)
    return "";

  HTTPClient http;
  WiFiClientSecure clientSecure;
  WiFiClient clientInsecure;

  if (url.startsWith("https://")) {
    clientSecure.setInsecure();
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
  if (WiFi.status() != WL_CONNECTED)
    return false;
  if (url.isEmpty())
    return false;

  HTTPClient http;
  WiFiClientSecure clientSecure;
  WiFiClient clientInsecure;

  if (url.startsWith("https://")) {
    clientSecure.setInsecure();
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
  if (len <= 0) {
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

  if (totalRead < len) {
    heap_caps_free(downloadBuffer);
    return false;
  }

  // 2. Write to SD with exclusive lock (Rapid block write)
  bool success = false;
  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(5000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    File file = SD.open(savePath.c_str(), FILE_WRITE);
    if (file) {
      size_t written = file.write(downloadBuffer, totalRead);
      file.close();
      success = (written == (size_t)totalRead);
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
  json += "]}}";

  int httpCode = http.POST(json);
  if (httpCode <= 0) {
    Serial.printf("WLED Error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}
