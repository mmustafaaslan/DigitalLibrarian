#ifndef TLS_TRUST_H
#define TLS_TRUST_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <time.h>

// Arduino-ESP32 ships the ESP-IDF Mozilla certificate bundle in libmbedtls.
// Referencing the linker symbols keeps trust data tied to the installed board
// core, so no additional library or independently maintained certificate file
// is required by this sketch.
extern const uint8_t arduino_crt_bundle_start[]
    asm("_binary_x509_crt_bundle_start");
extern const uint8_t arduino_crt_bundle_end[]
    asm("_binary_x509_crt_bundle_end");

inline bool ensureTlsClock(uint32_t timeoutMs = 5000) {
  time_t now = time(nullptr);
  if (now >= 1700000000)
    return true;

  static bool syncRequested = false;
  if (!syncRequested) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    syncRequested = true;
  }

  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    now = time(nullptr);
    if (now >= 1700000000)
      return true;
    delay(100);
  }
  return false;
}

inline bool configureTrustedTlsClient(WiFiClientSecure &client) {
  if (!ensureTlsClock())
    return false;
  client.setCACertBundle(
      arduino_crt_bundle_start,
      static_cast<size_t>(arduino_crt_bundle_end - arduino_crt_bundle_start));
  return true;
}

#endif // TLS_TRUST_H
