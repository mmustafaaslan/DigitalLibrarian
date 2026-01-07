#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <vector>

class AppNetworkManager {
public:
  static void init();
  static bool isConnected();
  static String getLocalIP();

  // WiFi Persistence
  static void loadWiFiNetworks();
  static void saveWiFiNetworks();
  static void addWiFiNetwork(String ssid, String password);
  static void removeWiFiNetwork(int index);
  static bool tryConnectToSavedNetworks();

  // Shared HTTP helper
  static String fetchURL(String url, int timeout = 5000);
  static bool downloadCoverImage(const String &url, const String &savePath);
  static void forceUpdateWLED();
};

#endif
