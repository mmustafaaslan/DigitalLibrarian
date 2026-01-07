#include "AppGlobals.h"
#include "Core_Data.h"
#include <SD.h>
#include <WebServer.h>

#if __has_include("secrets.h")
#include "secrets.h"
const char *DEFAULT_SSID = WIFI_SSID;
const char *DEFAULT_PASSWORD = WIFI_PASSWORD;
const char *DISCOGS_TOKEN = DISCOGS_API_TOKEN;
#else
const char *DEFAULT_SSID = "YOUR_WIFI_SSID";
const char *DEFAULT_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *DISCOGS_TOKEN = "YOUR_DISCOGS_TOKEN";
#endif

Preferences preferences;
ESP_IOExpander_CH422G *sdExpander = NULL;
CRGB *leds = NULL;

std::vector<WiFiNetwork> savedWiFiNetworks;

String web_pin = "cd1234";
String mdns_name = "mylibrary";
int setting_screensaver_min = 0;
bool backlight_on = true;
bool setting_enable_cds = true;
bool setting_enable_books = false;
int setting_books_led_start = 300;
int setting_cds_led_start = 0;

int led_count = 800;
String led_type_str = "WS2812B";
bool led_master_on = true;
int led_brightness = 50;
bool led_use_wled = false;
String wled_ip = "192.168.1.100";
int wled_timeout_ms = 500;

CRGB COLOR_FAVORITE = CRGB::Magenta;
CRGB COLOR_SELECTED = CRGB::Green;
CRGB COLOR_FILTERED = CRGB::Cyan;
CRGB COLOR_TEMPORARY = CRGB(255, 255, 0);

uint32_t setting_theme_cd = 0x00ff88;
uint32_t setting_theme_book = 0xffaa00;
int setting_cache_size =
    5; // Items per side (5 = 11 total, 10 = 21 total, 15 = 31 total)
bool is_screen_off = false;
bool filter_active = false;
bool settings_reboot_needed = false;
String filter_genre = "";
int filter_decade = 0;
bool filter_favorites_only = false;
unsigned long previewModeUntil = 0;

std::vector<int> search_matches;
int search_display_offset = 0;
const int SEARCH_PAGE_SIZE = 20;

CD currentEditCD;
Book currentEditBook;
bool trigger_cover_sync = false;
bool is_sync_stopping = false;
MediaMode currentMode = MODE_CD;

// Core library storage
CDVector cdLibrary;
int currentCDIndex = 0;
BookVector bookLibrary;
int currentBookIndex = 0;

// Navigation cache for fast browsing
NavigationCache navCache = {.cdCacheStartIndex = -1, .bookCacheStartIndex = -1};

// --- Registry Definition (Single Instance) ---
ModeDefinition registry[] = {
    {MODE_CD,
     "CD",
     "CDs",
     "CDs",
     "Artist",
     "Barcode",
     "cd_index.jsonl",
     "cd_",
     "album",
     "Barcode Scanner",
     "Cover Art Tool",
     "Scan barcodes to auto-add CDs. Fetches Genres and Tags from "
     "MusicBrainz.",
     "BC",
     "min",
     true,
     LV_SYMBOL_AUDIO,
     LV_SYMBOL_FILE,
     &currentCDIndex,
     &setting_cds_led_start,
     &setting_theme_cd},
    {MODE_BOOK,
     "Book",
     "Books",
     "BKS",
     "Author",
     "ISBN",
     "book_index.jsonl",
     "book_",
     "item",
     "ISBN Scanner",
     "Book Art Tool",
     "Scan ISBNs to auto-add Books. Fetches metadata and covers from Google "
     "Books.",
     "ISBN",
     "Pages",
     true,
     LV_SYMBOL_FILE,
     LV_SYMBOL_AUDIO,
     &currentBookIndex,
     &setting_books_led_start,
     &setting_theme_book}};

// --- Settings Persistence ---
void loadSettings() {
  preferences.begin("settings", true); // Read-only
  web_pin = preferences.getString("web_pin", "cd1234");
  mdns_name = preferences.getString("mdns_name", "mylibrary");
  led_brightness = preferences.getInt("led_bright", 50);

  // Colors stored as uint32_t (R << 16 | G << 8 | B)
  uint32_t fav = preferences.getUInt("col_fav", 0xFF00FF); // Magenta default
  COLOR_FAVORITE = CRGB(fav);

  uint32_t sel = preferences.getUInt("col_sel", 0x008000); // Green default
  COLOR_SELECTED = CRGB(sel);

  uint32_t filt = preferences.getUInt("col_filt", 0x00FFFF); // Cyan default
  COLOR_FILTERED = CRGB(filt);

  setting_screensaver_min = preferences.getInt("saver_min", 0);

  // Load LED Config
  led_count = preferences.getInt("led_count", 800);
  led_type_str = preferences.getString("led_type", "WS2812B");
  led_use_wled = preferences.getBool("use_wled", false);
  wled_ip = preferences.getString("wled_ip", "192.168.1.100");

  // Load Features Settings
  setting_enable_cds = preferences.getBool("enable_cds", true);
  setting_enable_books = preferences.getBool("enable_books", false);
  setting_books_led_start = preferences.getInt("books_led_start", 300);
  setting_cds_led_start = preferences.getInt("cds_led_start", 0);

  // Load Theme Colors
  setting_theme_cd = preferences.getUInt("theme_cd", 0x00ff88);
  setting_theme_book = preferences.getUInt("theme_book", 0xffaa00);

  // Load Cache Size
  setting_cache_size =
      preferences.getInt("cache_size", 5); // Default 5 items per side
  // Validate: must be 5, 10, or 15
  if (setting_cache_size != 5 && setting_cache_size != 10 &&
      setting_cache_size != 15) {
    setting_cache_size = 5; // Reset to default if invalid
  }

  // Load Saved Mode
  currentMode = (MediaMode)preferences.getInt("mode", (int)MODE_CD);

  preferences.end();

  // Apply immediate effects
  FastLED.setBrightness(led_brightness);
}

void saveSettings() {
  preferences.begin("settings", false); // Read-write
  preferences.putString("web_pin", web_pin);
  preferences.putString("mdns_name", mdns_name);
  preferences.putInt("led_bright", led_brightness);

  uint32_t fav = ((uint32_t)COLOR_FAVORITE.r << 16) |
                 ((uint32_t)COLOR_FAVORITE.g << 8) | COLOR_FAVORITE.b;
  preferences.putUInt("col_fav", fav);

  uint32_t sel = ((uint32_t)COLOR_SELECTED.r << 16) |
                 ((uint32_t)COLOR_SELECTED.g << 8) | COLOR_SELECTED.b;
  preferences.putUInt("col_sel", sel);

  uint32_t filt = ((uint32_t)COLOR_FILTERED.r << 16) |
                  ((uint32_t)COLOR_FILTERED.g << 8) | COLOR_FILTERED.b;
  preferences.putUInt("col_filt", filt);

  preferences.putInt("saver_min", setting_screensaver_min);
  preferences.putInt("led_count", led_count);
  preferences.putString("led_type", led_type_str);
  preferences.putBool("use_wled", led_use_wled);
  preferences.putString("wled_ip", wled_ip);

  // Save Features Settings
  preferences.putBool("enable_cds", setting_enable_cds);
  preferences.putBool("enable_books", setting_enable_books);
  preferences.putInt("books_led_start", setting_books_led_start);
  preferences.putInt("cds_led_start", setting_cds_led_start);

  // Save Theme Colors
  preferences.putUInt("theme_cd", setting_theme_cd);
  preferences.putUInt("theme_book", setting_theme_book);

  // Save Cache Size
  preferences.putInt("cache_size", setting_cache_size);

  // Save Current Mode
  preferences.putInt("mode", (int)currentMode);

  preferences.end();
}
