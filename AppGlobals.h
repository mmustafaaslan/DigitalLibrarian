#ifndef APP_GLOBALS_H
#define APP_GLOBALS_H

#include "mode_abstraction.h"
#include <Arduino.h>
#include <ESP_IOExpander_Library.h>
#include <FastLED.h>
#include <Preferences.h>
#include <vector>

// --- Global Constants ---
#define MAX_WIFI_NETWORKS 3
#define LED_PIN 6
#define COLOR_ORDER GRB

extern const char *DEFAULT_SSID;
extern const char *DEFAULT_PASSWORD;
extern const char *DISCOGS_TOKEN;

// --- Global Objects ---
extern Preferences preferences;
extern ESP_IOExpander_CH422G *sdExpander;
extern CRGB *leds;

extern SemaphoreHandle_t libraryMutex;
extern SemaphoreHandle_t i2cMutex;

extern MediaMode currentMode;
extern int currentCDIndex;
extern int currentBookIndex;
extern CDVector cdLibrary;
extern BookVector bookLibrary;

// --- WiFi Network Structure ---
struct WiFiNetwork {
  String ssid;
  String password;
};
extern std::vector<WiFiNetwork> savedWiFiNetworks;

// --- Settings & Calibration ---
extern String web_pin;
extern String mdns_name;
extern int setting_screensaver_min;
extern bool backlight_on;
extern bool setting_enable_cds;
extern bool setting_enable_books;
extern int setting_books_led_start;
extern int setting_cds_led_start;

// --- LED State ---
extern int led_count;
extern String led_type_str;
extern bool led_master_on;
extern int led_brightness;
extern bool led_use_wled;
extern String wled_ip;
extern int wled_timeout_ms;

extern CRGB COLOR_FAVORITE;
extern CRGB COLOR_SELECTED;
extern CRGB COLOR_FILTERED;
extern CRGB COLOR_TEMPORARY;

// --- UI State ---
extern uint32_t setting_theme_cd;
extern uint32_t setting_theme_book;
extern bool is_screen_off;
extern bool filter_active;
extern bool settings_reboot_needed;
extern String filter_genre;
extern int filter_decade;
extern bool filter_favorites_only;
extern unsigned long previewModeUntil;

// --- Search State ---
extern std::vector<int> search_matches;
extern int search_display_offset;
extern const int SEARCH_PAGE_SIZE;

// --- Media State ---
extern CD currentEditCD;
extern Book currentEditBook;
extern bool trigger_cover_sync;
extern bool is_sync_stopping;

// --- Forward Declarations for UI Updates ---
void update_item_display();
void logMemoryUsage(const char *label);
void forceUpdateWLED();
void update_filtered_leds();
uint32_t getCurrentThemeColor();
bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h,
                 uint16_t *bitmap);
void loadSettings();
void saveSettings();

#endif
