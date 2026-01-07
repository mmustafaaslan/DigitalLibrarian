#include "UIManager.h"
#include "AppGlobals.h"
#include "BackgroundWorker.h"
#include "MediaManager.h"
#include "NavigationCache.h"
#include "NetworkManager.h"
#include "Storage.h"
#include "UI_Styles.h"
#include "Utils.h"
#include "Waveshare_ST7262_LVGL.h"
#include "mode_abstraction.h"
#include <Arduino.h>
#include <FastLED.h>
#include <SD.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>

// --- Image Loading Globals ---
static uint16_t *img_buffer = NULL;
static lv_img_dsc_t raw_img_dsc;

// --- Progress Modal Globals ---
static lv_obj_t *progress_modal = NULL;
static lv_obj_t *progress_bar = NULL;
static lv_obj_t *progress_label = NULL;

// TJpg output callback
bool tjpg_output(int16_t x, int16_t y, uint16_t w, uint16_t h,
                 uint16_t *bitmap) {
  if (y >= 240 || img_buffer == NULL)
    return 0;
  int16_t out_w = w;
  if (x + w > 240)
    out_w = 240 - x;
  int16_t out_h = h;
  if (y + h > 240)
    out_h = 240 - y;

  for (int16_t j = 0; j < out_h; j++) {
    memcpy(&img_buffer[(y + j) * 240 + x], &bitmap[j * w], out_w * 2);
  }
  return 1;
}

// --- UI Objects Implementation ---
lv_obj_t *label_title;
lv_obj_t *label_artist;
lv_obj_t *label_genre;
lv_obj_t *label_year;
lv_obj_t *label_led;
lv_obj_t *label_notes;
lv_obj_t *label_favorites;
lv_obj_t *label_counter;
lv_obj_t *img_cover;
lv_obj_t *img_cover_container;
lv_obj_t *label_cover_url;
lv_obj_t *btn_search;
lv_obj_t *btn_delete_cover;
lv_obj_t *label_favorite;
lv_obj_t *label_extra_info;
lv_obj_t *label_filter_status;
lv_obj_t *btn_tracklist;
lv_obj_t *btn_prev;
lv_obj_t *btn_next;
lv_obj_t *btn_edit;

// --- Panel Objects Implementation ---
lv_obj_t *tracklist_panel = NULL;
lv_obj_t *lyrics_panel = NULL;
lv_obj_t *search_panel = NULL;
lv_obj_t *add_item_panel = NULL;
lv_obj_t *wifi_config_panel = NULL;
lv_obj_t *filter_panel = NULL;
lv_obj_t *dd_genre_filter = NULL;
lv_obj_t *dd_decade_filter = NULL;
lv_obj_t *cb_fav_filter = NULL;

lv_obj_t *btn_lib_search = NULL;
lv_obj_t *btn_add_item = NULL;
lv_obj_t *btn_random = NULL; // Promoted to global
lv_obj_t *btn_filter = NULL; // Promoted to global
lv_obj_t *btn_wifi = NULL;   // Promoted to global
lv_obj_t *btn_mode = NULL;   // Promoted to global

// Promoted Labels to Globals
lv_obj_t *search_label = NULL;
lv_obj_t *add_label = NULL;
lv_obj_t *random_label = NULL;
lv_obj_t *filter_label = NULL;
lv_obj_t *label_wifi = NULL;
lv_obj_t *label_mode = NULL;
lv_obj_t *btn_settings = NULL;
lv_obj_t *label_settings = NULL;
lv_obj_t *btn_led_toggle = NULL;
lv_obj_t *label_led_btn = NULL;
lv_obj_t *btn_sync_ui = NULL;
lv_obj_t *label_sync = NULL;
lv_obj_t *btn_qr = NULL;
lv_obj_t *label_qr = NULL;
lv_obj_t *btn_restart_h = NULL;
lv_obj_t *lbl_restart_h = NULL;

// --- Modal Specific Objects ---
// Search UI
lv_obj_t *ta_search = NULL;
lv_obj_t *kb_search = NULL;
lv_obj_t *dd_filter = NULL;
lv_obj_t *list_results = NULL;
static lv_timer_t *search_timer = NULL;
static lv_timer_t *nav_idle_timer = NULL;

// Add/Edit UI
lv_obj_t *ta_barcode = NULL;
lv_obj_t *ta_title = NULL;
lv_obj_t *ta_artist = NULL;
lv_obj_t *ta_genre = NULL;
lv_obj_t *ta_year = NULL;
lv_obj_t *ta_led_index = NULL;
lv_obj_t *ta_uniqueID = NULL;
lv_obj_t *ta_notes = NULL;
lv_obj_t *ta_publisher = NULL;
lv_obj_t *ta_page_count = NULL;
lv_obj_t *ta_current_page = NULL;

// WiFi UI
lv_obj_t *ta_ssid = NULL;
lv_obj_t *ta_password = NULL;
lv_obj_t *kb_wifi = NULL;

int edit_item_index = -1;
bool sort_by_artist = true;

// Forward declarations

void btn_search_clicked(lv_event_t *e);
void btn_delete_cover_clicked(lv_event_t *e);
void btn_favorite_clicked(lv_event_t *e);
void btn_prev_clicked(lv_event_t *e);
void btn_next_clicked(lv_event_t *e);
void filter_library(const char *query);

// UI Panel Functions
void show_search_ui();
void close_search_ui();
void show_add_item_ui();
void show_edit_item_ui(int index);
void close_add_item_ui();
void show_settings_ui();
void close_settings_ui();
void show_wifi_config_ui();
void close_wifi_config_ui();
void show_filter_ui();
void close_filter_ui();
void apply_filters();
void clear_filters();
void show_tracklist_ui(int index);
void close_tracklist_ui();
void show_chapter_list_ui(int index);
void show_lyrics_popup(String trackTitle, String lyricsText);
void close_lyrics_popup();
void show_qr_ui();
void close_qr_ui();
void show_led_selector_ui(lv_obj_t *target_ta);
void show_confirmation_popup(const char *title, const char *message,
                             lv_event_cb_t yes_cb, lv_event_cb_t no_cb,
                             void *user_data);
void show_info_popup(const char *title, const char *message,
                     lv_event_cb_t ok_cb, void *user_data);

// Helper functions
// Helper functions
void load_and_show_cover(String filename);
void update_filtered_leds();
bool is_item_match(int index);
void selectRandomWithEffect();
void forceUpdateWLED();

// Forward declarations for lyrics/chapter fetching
LyricsResult fetchLyricsIfNeeded(const char *releaseMbid, int trackIndex);
void fetchAllLyrics(const char *releaseMbid);

// Implementation of missing functions
void selectRandomWithEffect() {
  int total = getItemCount();
  if (total == 0)
    return;

  // Visual "scanning" effect
  lv_obj_t *popup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(popup, 200, 100);
  lv_obj_center(popup);
  lv_obj_set_style_bg_color(popup, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(popup, lv_color_hex(0xFF00FF), 0);
  lv_obj_set_style_border_width(popup, 2, 0);

  lv_obj_t *lbl = lv_label_create(popup);
  lv_label_set_text(lbl, "Picking Random...");
  lv_obj_center(lbl);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF00FF), 0);

  lv_timer_t *timer = lv_timer_create(
      [](lv_timer_t *t) {
        static int steps = 0;
        lv_obj_t *p = (lv_obj_t *)t->user_data;

        if (steps++ < 10) {
          // Flash random LEDs
          FastLED.clear();
          for (int i = 0; i < 5; i++) {
            int r = random(led_count);
            leds[r] = CHSV(random8(), 255, 255);
          }
          FastLED.show();
        } else {
          // Done
          int total = getItemCount();
          int r = random(total);
          setCurrentItemIndex(r);
          update_item_display();
          lv_obj_del(p);
          lv_timer_del(t);
        }
      },
      100, popup);
}

void forceUpdateWLED() { AppNetworkManager::forceUpdateWLED(); }

// Helper functions (formerly in DigitalLibrarian.ino, now drop-in compatible)
inline int getCDCount() { return getItemCount(); }
inline uint32_t get_current_theme_color() { return getCurrentThemeColor(); }

// Helper for tracklist button
void createTracklistButton() {
  lvgl_port_lock(-1);
  btn_tracklist = lv_btn_create(lv_scr_act());
  lv_obj_set_size(btn_tracklist, 130, 40);
  lv_obj_set_pos(btn_tracklist, LV_HOR_RES - 140, 10);
  lv_obj_set_style_bg_color(btn_tracklist, lv_color_hex(0x0088ff), 0);
  lv_obj_set_style_border_color(btn_tracklist, lv_color_hex(0x00aaff), 0);
  lv_obj_set_style_border_width(btn_tracklist, 2, 0);

  lv_obj_t *lblTracklist = lv_label_create(btn_tracklist);
  lv_label_set_text(lblTracklist, (currentMode == MODE_CD)
                                      ? LV_SYMBOL_LIST " Tracks"
                                      : LV_SYMBOL_LIST " Chapters");
  lv_obj_center(lblTracklist);

  lv_obj_add_event_cb(
      btn_tracklist,
      [](lv_event_t *e) { show_tracklist_ui(getCurrentItemIndex()); },
      LV_EVENT_CLICKED, NULL);
  lvgl_port_unlock();
}

void close_tracklist_ui() {
  if (!tracklist_panel)
    return;
  lvgl_port_lock(-1);
  lv_obj_del(tracklist_panel);
  tracklist_panel = NULL;
  lvgl_port_unlock();
}

void close_wifi_conf_ui();
void show_web_qr_ui();

void close_lyrics_popup() {
  if (!lyrics_panel)
    return;
  lvgl_port_lock(-1);
  lv_obj_del(lyrics_panel);
  lyrics_panel = NULL;
  lvgl_port_unlock();
}

void show_lyrics_popup(String trackTitle, String lyricsText) {
  if (lyrics_panel)
    close_lyrics_popup();
  lvgl_port_lock(-1);

  // Use top layer to ensure visibility above other modals
  lyrics_panel = lv_obj_create(lv_layer_top());
  lv_obj_clear_flag(lyrics_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(lyrics_panel, lv_pct(90), lv_pct(90));
  lv_obj_center(lyrics_panel);
  lv_obj_add_style(lyrics_panel, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(lyrics_panel, lv_color_hex(0x000000), 0);

  lv_obj_t *lblTitle = lv_label_create(lyrics_panel);
  lv_label_set_text(lblTitle, sanitizeText(trackTitle).c_str());
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_add_style(lblTitle, &style_text_header, 0);

  lv_obj_t *btnClose = lv_btn_create(lyrics_panel);
  lv_obj_set_size(btnClose, 60, 40);
  lv_obj_align(btnClose, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btnClose, &style_btn_close, 0);

  lv_obj_t *lblClose = lv_label_create(btnClose);
  lv_label_set_text(lblClose, LV_SYMBOL_CLOSE);
  lv_obj_center(lblClose);
  lv_obj_set_style_text_color(lblClose, lv_color_hex(0xff4444), 0);
  lv_obj_add_event_cb(
      btnClose, [](lv_event_t *e) { close_lyrics_popup(); }, LV_EVENT_CLICKED,
      NULL);

  lv_obj_t *cont = lv_obj_create(lyrics_panel);
  lv_obj_set_size(cont, lv_pct(100), lv_pct(80));
  lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(cont, 0, 0);
  lv_obj_set_style_border_width(cont, 0, 0);

  lv_obj_t *lblLyrics = lv_label_create(cont);
  lv_label_set_text(lblLyrics, lyricsText.c_str());
  lv_obj_set_width(lblLyrics, lv_pct(95));
  lv_label_set_long_mode(lblLyrics, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(lblLyrics, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lblLyrics, lv_color_hex(0xcccccc), 0);
  lv_obj_set_style_text_font(lblLyrics, &lv_font_montserrat_16, 0);

  lvgl_port_unlock();
}

static void trackClickHandler(lv_event_t *e) {
  Serial.println(">>> trackClickHandler CLICKED <<<");
  Track *track = (Track *)lv_event_get_user_data(e);
  int idx = getCurrentItemIndex();

  if (idx < 0 || idx >= (int)cdLibrary.size()) {
    Serial.printf("Error: Invalid Index %d\n", idx);
    return;
  }

  CD &cd = cdLibrary[idx];

  if (!track || cd.releaseMbid.length() == 0) {
    return;
  }
  int trackIndex = track->trackNo - 1;
  Serial.printf("Status: '%s'\n", track->lyrics.status.c_str());

  if (track->lyrics.status == "cached") {
    Serial.println("Status is CACHED. Loading...");
    Serial.printf("Loading from path: %s\n", track->lyrics.path.c_str());
    String lyrics = Storage.loadLyrics(track->lyrics.path.c_str());
    Serial.printf("Loaded Lyrics: %d bytes\n", lyrics.length());

    if (lyrics.length() > 0) {
      Serial.println("Calling popup...");
      show_lyrics_popup(String(track->title.c_str()), lyrics);
      return; // Done
    } else {
      Serial.println("Error: Cached lyrics empty? Treating as missing.");
      track->lyrics.status = "missing";
      // Fall through to missing logic below
    }
  }

  if (track->lyrics.status == "missing") {
    // Retry logic
    Serial.printf("Retrying lyrics for track %d\n", track->trackNo);

    LyricsResult result =
        fetchLyricsIfNeeded(cd.releaseMbid.c_str(), trackIndex, true);

    if (result == LYRICS_FETCHED_NOW || result == LYRICS_ALREADY_CACHED) {
      TrackList *tl = Storage.loadTracklist(cd.releaseMbid.c_str());
      if (tl && trackIndex < (int)tl->tracks.size()) {
        String lyrics =
            Storage.loadLyrics(tl->tracks[trackIndex].lyrics.path.c_str());

        if (lyrics.length() > 0) {
          show_lyrics_popup(String(track->title.c_str()), lyrics);
          // close_tracklist_ui(); // Keep open behind for context
          // show_tracklist_ui(getCurrentItemIndex());
        }
      }
      if (tl)
        delete tl;
    } else {
      show_info_popup("Not Found", "Lyrics still not available", NULL, NULL);
    }
  } else {
    // Unchecked - fetch now
    Serial.printf("Fetching lyrics for track %d\n", track->trackNo);

    LyricsResult result =
        fetchLyricsIfNeeded(cd.releaseMbid.c_str(), trackIndex, true);

    if (result == LYRICS_FETCHED_NOW || result == LYRICS_ALREADY_CACHED) {
      TrackList *tl = Storage.loadTracklist(cd.releaseMbid.c_str());
      if (tl && trackIndex < (int)tl->tracks.size()) {
        String lyrics =
            Storage.loadLyrics(tl->tracks[trackIndex].lyrics.path.c_str());

        if (lyrics.length() > 0) {
          show_lyrics_popup(String(track->title.c_str()), lyrics);
        }
      }
      if (tl)
        delete tl;
    } else {
      show_info_popup("Not Found", "No lyrics available for this track", NULL,
                      NULL);
    }
  }
}

void show_tracklist_ui(int idx) {
  switch (currentMode) {
  case MODE_BOOK:
    return; // No tracklist/chapter support

  case MODE_CD:
  default:
    break;
  }
  if (idx < 0 || idx >= (int)cdLibrary.size())
    return;
  ensureItemDetailsLoaded(idx);
  CD &cd = cdLibrary[idx];

  if (cd.releaseMbid.length() == 0) {
    show_info_popup("No Tracklist", "This CD has no MusicBrainz data.", NULL,
                    NULL);
    return;
  }
  TrackList *trackList = Storage.loadTracklist(cd.releaseMbid.c_str());
  if (!trackList || trackList->tracks.size() == 0) {
    if (trackList)
      delete trackList;
    show_info_popup("No Tracks", "Track file not found.", NULL, NULL);
    return;
  }

  if (tracklist_panel)
    close_tracklist_ui();
  lvgl_port_lock(-1);

  tracklist_panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(tracklist_panel, LV_HOR_RES * 0.6, LV_VER_RES * 0.7);
  lv_obj_center(tracklist_panel);
  lv_obj_add_style(tracklist_panel, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(tracklist_panel, lv_color_hex(0x000000), 0);

  lv_obj_t *lblTitle = lv_label_create(tracklist_panel);
  lv_label_set_text_fmt(lblTitle, "%s - %s",
                        sanitizeText(String(cd.title.c_str())).c_str(),
                        sanitizeText(String(cd.artist.c_str())).c_str());
  lv_obj_align(lblTitle, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_add_style(lblTitle, &style_text_header, 0);

  lv_obj_t *btnClose = lv_btn_create(tracklist_panel);
  lv_obj_set_size(btnClose, 50, 35);
  lv_obj_align(btnClose, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btnClose, &style_btn_close, 0);
  lv_obj_t *lblClose = lv_label_create(btnClose);
  lv_label_set_text(lblClose, LV_SYMBOL_CLOSE);
  lv_obj_center(lblClose);
  lv_obj_set_style_text_color(lblClose, lv_color_hex(0xff4444), 0);

  lv_obj_add_event_cb(
      btnClose,
      [](lv_event_t *e) {
        TrackList *tl = (TrackList *)lv_event_get_user_data(e);
        close_tracklist_ui();
        delete tl;
      },
      LV_EVENT_CLICKED, trackList);

  // Fetch All Lyrics button (Only for CDs)
  switch (currentMode) {
  case MODE_CD: {
    lv_obj_t *btnFetchAll = lv_btn_create(tracklist_panel);
    lv_obj_set_size(btnFetchAll, 50, 35);
    lv_obj_align(btnFetchAll, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(btnFetchAll, lv_color_hex(0x0088ff), 0);
    lv_obj_t *lblFetchAll = lv_label_create(btnFetchAll);
    lv_label_set_text(lblFetchAll, LV_SYMBOL_DOWNLOAD);
    lv_obj_center(lblFetchAll);

    char *mbidCopy = strdup(cd.releaseMbid.c_str());
    lv_obj_add_event_cb(
        btnFetchAll,
        [](lv_event_t *e) {
          char *mbid = (char *)lv_event_get_user_data(e);
          fetchAllLyrics(mbid);
          free(mbid);
        },
        LV_EVENT_CLICKED, mbidCopy);
  } break;
  default:
    break;
  }

  lv_obj_t *container = lv_obj_create(tracklist_panel);
  int containerW = (int)(LV_HOR_RES * 0.55);
  int containerH = (int)(LV_VER_RES * 0.45);
  lv_obj_set_size(container, containerW, containerH);
  lv_obj_align(container, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(container, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_color(container, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_set_style_border_width(container, 1, 0);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(container, LV_DIR_VER);

  for (int i = 0; i < (int)trackList->tracks.size(); i++) {
    Track &track = trackList->tracks[i];
    if (track.title.length() == 0 || track.title == " ")
      continue;

    const char *icon = getLyricsStatusIcon(track.lyrics.status.c_str());

    lv_obj_t *btn = lv_btn_create(container);
    lv_obj_set_width(btn, lv_pct(95));
    lv_obj_set_height(btn, 40);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(getCurrentThemeColor()), 0);
    lv_obj_set_style_border_width(btn, 1, 0);

    lv_obj_t *btn_fav = lv_btn_create(btn);
    lv_obj_set_size(btn_fav, 30, 30);
    lv_obj_align(btn_fav, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_bg_color(btn_fav,
                              track.isFavoriteTrack ? lv_color_hex(0xFFD700)
                                                    : lv_color_hex(0x555555),
                              0);
    lv_obj_t *lbl_bell = lv_label_create(btn_fav);
    lv_label_set_text(lbl_bell, LV_SYMBOL_BELL);
    lv_obj_center(lbl_bell);
    lv_obj_set_style_text_color(lbl_bell,
                                track.isFavoriteTrack ? lv_color_hex(0x000000)
                                                      : lv_color_hex(0xCCCCCC),
                                0);

    struct TrackFavData {
      int idx;
      TrackList *tl;
      String mbid;
    };
    TrackFavData *fd = new TrackFavData{i, trackList, cd.releaseMbid.c_str()};
    lv_obj_add_event_cb(
        btn_fav,
        [](lv_event_t *e) {
          TrackFavData *data = (TrackFavData *)lv_event_get_user_data(e);
          Track &t = data->tl->tracks[data->idx];
          t.isFavoriteTrack = !t.isFavoriteTrack;

          lv_obj_t *b = lv_event_get_target(e);
          lv_obj_t *l = lv_obj_get_child(b, 0);

          lv_obj_set_style_bg_color(b,
                                    t.isFavoriteTrack ? lv_color_hex(0xFFD700)
                                                      : lv_color_hex(0x555555),
                                    0);
          lv_obj_set_style_text_color(l,
                                      t.isFavoriteTrack
                                          ? lv_color_hex(0x000000)
                                          : lv_color_hex(0xCCCCCC),
                                      0);
          Storage.saveTracklist(data->mbid.c_str(), data->tl);
        },
        LV_EVENT_CLICKED, fd);
    lv_obj_add_event_cb(
        btn_fav,
        [](lv_event_t *e) { delete (TrackFavData *)lv_event_get_user_data(e); },
        LV_EVENT_DELETE, fd);

    lv_obj_t *lblLeft = lv_label_create(btn);
    lv_label_set_text_fmt(lblLeft, "%d. %s", track.trackNo,
                          sanitizeText(String(track.title.c_str())).c_str());
    lv_obj_align(lblLeft, LV_ALIGN_LEFT_MID, 50, 0);
    lv_obj_set_width(lblLeft, containerW - 160);
    lv_label_set_long_mode(lblLeft, LV_LABEL_LONG_DOT);

    lv_obj_t *lblRight = lv_label_create(btn);
    lv_label_set_text_fmt(lblRight, "%s %s",
                          formatDuration(track.durationMs).c_str(), icon);
    lv_obj_align(lblRight, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_text_color(lblRight, lv_color_hex(getCurrentThemeColor()),
                                0);

    Track *trackCopy = new Track(track);
    lv_obj_add_event_cb(btn, trackClickHandler, LV_EVENT_CLICKED, trackCopy);
    lv_obj_add_event_cb(
        btn, [](lv_event_t *e) { delete (Track *)lv_event_get_user_data(e); },
        LV_EVENT_DELETE, trackCopy);
  }
  lvgl_port_unlock();
}

// show_chapter_list_ui removed

// Remaining chapter logic removed

void setupMainUI() {
  // Serial.println(">> setupMainUI Start");
  ui_styles_init(); // Initialize global styles
  // Serial.println(">> Styles Init Done");

  lv_obj_t *scr = lv_scr_act();
  // Serial.println(">> Screen Act Done");
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a1a), 0);

  // Filter Status Indicator
  // Serial.println(">> Creating Filter Status...");
  label_filter_status = lv_label_create(scr);
  lv_label_set_text(label_filter_status, "");
  lv_obj_align(label_filter_status, LV_ALIGN_TOP_MID, 0, 75);
  lv_obj_set_style_text_color(label_filter_status, lv_color_hex(0x00aaff), 0);
  lv_obj_add_flag(label_filter_status, LV_OBJ_FLAG_HIDDEN);

  // Search Button
  // Serial.println(">> Creating Search Button...");
  btn_lib_search = lv_btn_create(scr);
  lv_obj_set_size(btn_lib_search, 50, 40);
  lv_obj_align(btn_lib_search, LV_ALIGN_TOP_LEFT, 10, 15);
  lv_obj_set_style_bg_color(btn_lib_search, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(btn_lib_search, lv_color_hex(0x00ffff), 0);
  lv_obj_set_style_border_width(btn_lib_search, 2, 0);
  lv_obj_set_style_radius(btn_lib_search, 5, 0);

  search_label = lv_label_create(btn_lib_search);
  lv_label_set_text(search_label, LV_SYMBOL_LIST);
  lv_obj_center(search_label);
  lv_obj_set_style_text_color(search_label, lv_color_hex(0x00ffff), 0);
  lv_obj_add_event_cb(
      btn_lib_search, [](lv_event_t *e) { show_search_ui(); }, LV_EVENT_CLICKED,
      NULL);

  // Serial.println(">> Search Button Done");

  // Add Button
  // Serial.println(">> Creating Add Button...");
  btn_add_item = lv_btn_create(scr);
  // Serial.println(">> Add Button Created");

  lv_obj_set_size(btn_add_item, 50, 40);
  lv_obj_align(btn_add_item, LV_ALIGN_TOP_LEFT, 70, 15);
  lv_obj_set_style_bg_color(btn_add_item, lv_color_hex(0x000000), 0);

  // Serial.println(">> Setting Add Button Border Color...");
  uint32_t color = getCurrentThemeColor();
  // Serial.printf(">> Theme Color: %06X\n", color);

  lv_obj_set_style_border_color(btn_add_item, lv_color_hex(color), 0);
  // Serial.println(">> Add Button Border Color Set");

  lv_obj_set_style_border_width(btn_add_item, 2, 0);
  lv_obj_set_style_radius(btn_add_item, 5, 0);

  // Serial.println(">> Creating Add Label...");
  add_label = lv_label_create(btn_add_item);
  lv_label_set_text(add_label, LV_SYMBOL_PLUS);
  lv_obj_center(add_label);
  lv_obj_set_style_text_color(add_label, lv_color_hex(color), 0);

  // Serial.println(">> Adding Add Event...");
  lv_obj_add_event_cb(
      btn_add_item,
      [](lv_event_t *e) {
        edit_item_index = -1;
        show_add_item_ui();
      },
      LV_EVENT_CLICKED, NULL);
  // Serial.println(">> Add Button Done");

  // Random Button
  // Serial.println(">> Creating Random Button...");
  if (scr == NULL)
    Serial.println("!! CRITICAL: Screen is NULL !!");
  delay(10);
  btn_random = lv_btn_create(scr);
  // Serial.printf(">> Random Button Created: %p\n", btn_random);

  lv_obj_set_size(btn_random, 50, 40);
  lv_obj_align(btn_random, LV_ALIGN_TOP_LEFT, 130, 15);
  lv_obj_set_style_bg_color(btn_random, lv_color_hex(0x000000), 0);

  // Serial.println(">> Setting Random Border...");
  lv_obj_set_style_border_color(btn_random, lv_color_hex(0xff00ff), 0);
  lv_obj_set_style_border_width(btn_random, 2, 0);
  lv_obj_set_style_radius(btn_random, 5, 0);

  // Serial.println(">> Creating Random Label...");
  random_label = lv_label_create(btn_random);
  lv_label_set_text(random_label, LV_SYMBOL_SHUFFLE);
  lv_obj_center(random_label);
  lv_obj_set_style_text_color(random_label, lv_color_hex(0xff00ff), 0);

  // Serial.println(">> Adding Random Event...");
  lv_obj_add_event_cb(
      btn_random, [](lv_event_t *e) { selectRandomWithEffect(); },
      LV_EVENT_CLICKED, NULL);

  // Serial.println(">> Random Button Done");

  // Filter Button
  // Serial.println(">> Creating Filter Button...");
  btn_filter = lv_btn_create(scr);
  // Serial.println(">> Filter Button Created");

  lv_obj_set_size(btn_filter, 50, 40);
  lv_obj_align(btn_filter, LV_ALIGN_TOP_LEFT, 190, 15);
  lv_obj_set_style_bg_color(btn_filter, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_color(btn_filter, lv_color_hex(0x00aaff), 0);
  lv_obj_set_style_border_width(btn_filter, 2, 0);
  lv_obj_set_style_radius(btn_filter, 5, 0);

  filter_label = lv_label_create(btn_filter);
  lv_label_set_text(filter_label, LV_SYMBOL_DIRECTORY);
  lv_obj_center(filter_label);
  lv_obj_set_style_text_color(filter_label, lv_color_hex(0x00aaff), 0);
  lv_obj_add_event_cb(
      btn_filter, [](lv_event_t *e) { show_filter_ui(); }, LV_EVENT_CLICKED,
      NULL);
  // Serial.println(">> Filter Button Done");

  // WiFi Button
  // Serial.println(">> Creating WiFi Button...");
  btn_wifi = lv_btn_create(scr);
  lv_obj_set_size(btn_wifi, 50, 40);
  lv_obj_align(btn_wifi, LV_ALIGN_TOP_RIGHT, -5, 15);
  lv_obj_add_style(btn_wifi, &style_btn_header_green, 0);

  label_wifi = lv_label_create(btn_wifi);
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(label_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(label_wifi,
                                lv_color_hex(getCurrentThemeColor()), 0);
  } else {
    lv_label_set_text(label_wifi, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(label_wifi, lv_color_hex(0xff8800), 0);
  }
  lv_obj_center(label_wifi);
  lv_obj_add_event_cb(
      btn_wifi, [](lv_event_t *e) { show_wifi_config_ui(); }, LV_EVENT_CLICKED,
      NULL);
  // Serial.println(">> WiFi Button Done");

  // Mode Switch Button
  // Serial.println(">> Creating Mode Button...");
  btn_mode = lv_btn_create(scr);
  // Serial.println(">> Mode Button Created");

  lv_obj_set_size(btn_mode, 50, 40);
  lv_obj_align(btn_mode, LV_ALIGN_TOP_RIGHT, -365, 15);
  lv_obj_add_style(btn_mode, &style_btn_header_green, 0);

  label_mode = lv_label_create(btn_mode);
  // Serial.println(">> Mode Label...");

  if (label_mode == NULL) {
    Serial.println("!! CRITICAL: label_mode is NULL !!");
  } else {
    // Registry Fix Verified - Restoring Dynamic Name
    // Serial.println(">> Getting Mode Name String...");
    String mName = getModeShortName();
    // Serial.printf(">> Mode Name Got: '%s'\n", mName.c_str());

    lv_label_set_text(label_mode, mName.c_str());
    // Serial.println(">> Mode Label Text Set");
  }

  lv_obj_center(label_mode);

  // Serial.println(">> Setting Mode Color...");
  // Registry Fix Verified - Restoring Dynamic Color
  lv_obj_set_style_text_color(label_mode, lv_color_hex(getCurrentThemeColor()),
                              0);
  // Serial.println(">> Mode Color Set");

  // Serial.println(">> Adding Mode Event...");
  lv_obj_add_event_cb(
      btn_mode,
      [](lv_event_t *e) {
        saveLibrary();
        MediaMode newMode = getOtherMode();
        preferences.begin("settings", false);
        preferences.putInt("mode", (int)newMode);
        preferences.end();
        lv_obj_t *panel = lv_obj_create(lv_scr_act());
        lv_obj_set_size(panel, 320, 180);
        lv_obj_center(panel);
        lv_obj_add_style(panel, &style_modal_panel, 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);

        lv_obj_t *title = lv_label_create(panel);
        lv_label_set_text(title, "Switching Mode");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_add_style(title, &style_text_header, 0);

        lv_obj_t *msg = lv_label_create(panel);
        lv_label_set_text(msg, "Restarting device...");
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);

        lv_timer_create([](lv_timer_t *t) { ESP.restart(); }, 1000, NULL);
      },
      LV_EVENT_CLICKED, NULL);

  // Settings Button
  // Settings Button
  // Serial.println(">> Creating Settings Button...");
  btn_settings = lv_btn_create(scr);
  lv_obj_set_size(btn_settings, 50, 40);
  lv_obj_align(btn_settings, LV_ALIGN_TOP_RIGHT, -305, 15);
  lv_obj_add_style(btn_settings, &style_btn_header_green, 0);

  label_settings = lv_label_create(btn_settings);
  lv_label_set_text(label_settings, LV_SYMBOL_SETTINGS);
  lv_obj_center(label_settings);
  lv_obj_set_style_text_color(label_settings,
                              lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_event_cb(
      btn_settings, [](lv_event_t *e) { show_settings_ui(); }, LV_EVENT_CLICKED,
      NULL);
  // Serial.println(">> Settings Button Done");

  // LED Toggle Button
  // Serial.println(">> Creating LED Toggle Button...");
  btn_led_toggle = lv_btn_create(scr);
  lv_obj_set_size(btn_led_toggle, 50, 40);
  lv_obj_align(btn_led_toggle, LV_ALIGN_TOP_RIGHT, -245, 15);
  lv_obj_add_style(btn_led_toggle, &style_btn_header_green, 0);

  label_led_btn = lv_label_create(btn_led_toggle);
  lv_label_set_text(label_led_btn,
                    led_master_on ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
  lv_obj_center(label_led_btn);
  lv_obj_set_style_text_color(label_led_btn,
                              led_master_on
                                  ? lv_color_hex(getCurrentThemeColor())
                                  : lv_color_hex(0x888888),
                              0);

  lv_obj_add_event_cb(
      btn_led_toggle,
      [](lv_event_t *e) {
        led_master_on = !led_master_on;
        lv_obj_t *btn = lv_event_get_target(e);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (led_master_on) {
          lv_label_set_text(lbl, LV_SYMBOL_EYE_OPEN);
          lv_obj_set_style_text_color(lbl, lv_color_hex(getCurrentThemeColor()),
                                      0);
          update_item_display();
        } else {
          lv_label_set_text(lbl, LV_SYMBOL_EYE_CLOSE);
          lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
          FastLED.clear();
          FastLED.show();
        }
      },
      LV_EVENT_CLICKED, NULL);
  // Serial.println(">> LED Toggle Button Done");

  // Sync Button
  // Serial.println(">> Creating Sync Button...");
  btn_sync_ui = lv_btn_create(scr);
  lv_obj_set_size(btn_sync_ui, 50, 40);
  lv_obj_align(btn_sync_ui, LV_ALIGN_TOP_RIGHT, -185, 15);
  lv_obj_add_style(btn_sync_ui, &style_btn_header_green, 0);

  label_sync = lv_label_create(btn_sync_ui);
  lv_label_set_text(label_sync, LV_SYMBOL_REFRESH);
  lv_obj_center(label_sync);
  lv_obj_set_style_text_color(label_sync, lv_color_hex(getCurrentThemeColor()),
                              0);

  lv_obj_add_event_cb(
      btn_sync_ui,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Sync Library", "Reload index AND download missing covers?",
            [](lv_event_t *e) { // YES
              Serial.println("UI: Sync YES clicked. Starting rebuild...");
              MediaManager::syncFromStorage();
              Serial.println(
                  "UI: Library synced from storage. Updating display...");
              update_item_display();
              Serial.println("UI: Display updated. Queueing bulk sync job...");
              BackgroundWorker::addJob({JOB_BULK_SYNC, "", -1, "", NULL});
              Serial.println("UI: Job queued. Closing popup...");
            },
            [](lv_event_t *e) { // NO
              Serial.println("UI: Sync NO clicked. Quick sync...");
              MediaManager::syncFromStorage();
              Serial.println("UI: Sync done. Updating display...");
              update_item_display();
              Serial.println("UI: Display updated. Showing popup...");
              show_info_popup("Library reloaded from index.", "Success", NULL,
                              NULL);
            },
            NULL);
      },
      LV_EVENT_CLICKED, NULL);
  // Serial.println(">> Sync Button Done");

  // QR Code Button
  // Serial.println(">> Creating QR Button...");
  btn_qr = lv_btn_create(scr);
  lv_obj_set_size(btn_qr, 50, 40);
  lv_obj_align(btn_qr, LV_ALIGN_TOP_RIGHT, -125, 15);
  lv_obj_add_style(btn_qr, &style_btn_header_green, 0);

  label_qr = lv_label_create(btn_qr);
  lv_label_set_text(label_qr, LV_SYMBOL_IMAGE);
  lv_obj_center(label_qr);
  lv_obj_set_style_text_color(label_qr, lv_color_hex(getCurrentThemeColor()),
                              0);

  lv_obj_add_event_cb(
      btn_qr, [](lv_event_t *e) { show_qr_ui(); }, LV_EVENT_CLICKED, NULL);
  // Serial.println(">> QR Button Done");

  // Restart Button (Between QR and WiFi)
  // Serial.println(">> Creating Restart Button...");
  btn_restart_h = lv_btn_create(scr);
  lv_obj_set_size(btn_restart_h, 50, 40);
  lv_obj_align(btn_restart_h, LV_ALIGN_TOP_RIGHT, -65, 15);
  lv_obj_add_style(btn_restart_h, &style_btn_header_green, 0);

  lbl_restart_h = lv_label_create(btn_restart_h);
  lv_label_set_text(lbl_restart_h, LV_SYMBOL_POWER);
  lv_obj_center(lbl_restart_h);
  lv_obj_set_style_text_color(lbl_restart_h, lv_color_hex(0xff4444),
                              0); // Red for reboot

  lv_obj_add_event_cb(
      btn_restart_h,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Restart Device", "Do you want to restart the device?",
            [](lv_event_t *e) {
              lv_obj_del(
                  lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e))));
              ESP.restart();
            },
            [](lv_event_t *e) {
              lv_obj_del(
                  lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e))));
            },
            NULL);
      },
      LV_EVENT_CLICKED, NULL);
  // Serial.println(">> Restart Button Done");

  // Album Cover Area
  img_cover_container = lv_obj_create(scr);
  lv_obj_set_size(img_cover_container, 250, 250);
  lv_obj_align(img_cover_container, LV_ALIGN_LEFT_MID, 30, 0);
  lv_obj_set_style_bg_color(img_cover_container, lv_color_hex(0x333333), 0);
  lv_obj_set_style_border_color(img_cover_container,
                                lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_border_width(img_cover_container, 2, 0);
  lv_obj_set_style_radius(img_cover_container, 5, 0);
  lv_obj_set_style_pad_all(img_cover_container, 0,
                           0); // Ensure no padding for centering
  lv_obj_clear_flag(img_cover_container, LV_OBJ_FLAG_SCROLLABLE);

  img_cover = lv_img_create(img_cover_container);
  lv_obj_set_size(img_cover, 240, 240);
  lv_obj_center(img_cover);
  lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);

  label_cover_url = lv_label_create(img_cover_container);
  lv_label_set_text(label_cover_url, "Click Search to find cover");
  lv_obj_align(label_cover_url, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_set_style_text_color(label_cover_url, lv_color_hex(0xaaaaaa), 0);
  lv_obj_set_style_text_align(label_cover_url, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(label_cover_url, 230);
  lv_label_set_long_mode(label_cover_url, LV_LABEL_LONG_WRAP);

  btn_search = lv_btn_create(img_cover_container);
  lv_obj_set_size(btn_search, 180, 50);
  lv_obj_add_style(btn_search, &style_btn_header_green, 0);
  lv_obj_align(btn_search, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_event_cb(btn_search, btn_search_clicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t *label_search_btn = lv_label_create(btn_search);
  lv_label_set_text(label_search_btn, LV_SYMBOL_EYE_OPEN " SEARCH");
  lv_obj_center(label_search_btn);
  lv_obj_set_style_text_color(label_search_btn,
                              lv_color_hex(getCurrentThemeColor()), 0);

  // Delete Cover Button
  btn_delete_cover = lv_btn_create(img_cover_container);
  lv_obj_set_size(btn_delete_cover, 40, 40);
  lv_obj_align(btn_delete_cover, LV_ALIGN_TOP_RIGHT, -5, 5);
  lv_obj_set_style_bg_color(btn_delete_cover, lv_color_hex(0xff4444), 0);
  lv_obj_set_style_radius(btn_delete_cover, 5, 0);
  lv_obj_add_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN); // Hidden by default

  lv_obj_t *label_del_btn = lv_label_create(btn_delete_cover);
  lv_label_set_text(label_del_btn, LV_SYMBOL_TRASH);
  lv_obj_center(label_del_btn);
  lv_obj_set_style_text_color(label_del_btn, lv_color_hex(0xffffff), 0);
  lv_obj_add_event_cb(btn_delete_cover, btn_delete_cover_clicked,
                      LV_EVENT_CLICKED, NULL);

  // CD Info Container
  lv_obj_t *info_container = lv_obj_create(scr);
  lv_obj_set_size(info_container, 450, 250);
  lv_obj_align(info_container, LV_ALIGN_RIGHT_MID, -30, 0);
  lv_obj_add_style(info_container, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(info_container, lv_color_hex(0x2a2a2a), 0);
  lv_obj_clear_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);

  label_title = lv_label_create(info_container);
  lv_obj_align(label_title, LV_ALIGN_TOP_LEFT, 5, 10);
  lv_obj_set_style_text_color(label_title, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(label_title, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(label_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(label_title, 330);

  label_artist = lv_label_create(info_container);
  lv_obj_align(label_artist, LV_ALIGN_TOP_LEFT, 5, 30);
  lv_obj_set_style_text_color(label_artist, lv_color_hex(0xcccccc), 0);
  lv_label_set_long_mode(label_artist, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label_artist, 410);

  label_favorite = lv_label_create(info_container);
  lv_obj_align(label_favorite, LV_ALIGN_TOP_RIGHT, -45, -5);
  lv_obj_set_style_text_color(label_favorite, lv_color_hex(0xffdd00), 0);
  lv_obj_set_style_text_font(label_favorite, &lv_font_montserrat_16, 0);
  lv_obj_add_flag(label_favorite, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(label_favorite, btn_favorite_clicked, LV_EVENT_CLICKED,
                      NULL);

  btn_tracklist = lv_btn_create(info_container);
  lv_obj_set_size(btn_tracklist, 40, 35);
  lv_obj_align(btn_tracklist, LV_ALIGN_TOP_RIGHT, 0, -10);
  lv_obj_add_style(btn_tracklist, &style_btn_header_green, 0);
  lv_obj_set_style_border_width(btn_tracklist, 0, 0);
  lv_obj_set_style_shadow_width(btn_tracklist, 0, 0);

  lv_obj_t *lblTracklist = lv_label_create(btn_tracklist);
  lv_label_set_text(lblTracklist, LV_SYMBOL_LIST);
  lv_obj_center(lblTracklist);
  lv_obj_set_style_text_color(lblTracklist, lv_color_hex(0xffffff), 0);
  lv_obj_add_event_cb(
      btn_tracklist,
      [](lv_event_t *e) { show_tracklist_ui(getCurrentItemIndex()); },
      LV_EVENT_CLICKED, NULL);

  label_genre = lv_label_create(info_container);
  lv_obj_align(label_genre, LV_ALIGN_TOP_LEFT, 5, 60);
  lv_obj_set_style_text_color(label_genre, lv_color_hex(getCurrentThemeColor()),
                              0);

  label_year = lv_label_create(info_container);
  lv_obj_align(label_year, LV_ALIGN_TOP_LEFT, 5, 85);
  lv_obj_set_style_text_color(label_year, lv_color_hex(getCurrentThemeColor()),
                              0);

  label_extra_info = lv_label_create(info_container);
  lv_obj_align(label_extra_info, LV_ALIGN_TOP_LEFT, 5, 110);
  lv_obj_set_style_text_color(label_extra_info,
                              lv_color_hex(getCurrentThemeColor()), 0);

  label_led = lv_label_create(info_container);
  lv_obj_align(label_led, LV_ALIGN_TOP_LEFT, 5, 135);
  lv_obj_set_style_text_color(label_led, lv_color_hex(0xff8800), 0);
  lv_obj_set_width(label_led, 300);
  lv_label_set_long_mode(label_led, LV_LABEL_LONG_DOT);

  label_notes = lv_label_create(info_container);
  lv_obj_align(label_notes, LV_ALIGN_TOP_LEFT, 5, 160);
  lv_obj_set_style_text_color(label_notes, lv_color_hex(0xcccccc), 0);
  lv_label_set_long_mode(label_notes, LV_LABEL_LONG_DOT);
  lv_obj_set_width(label_notes, 410);

  label_favorites = lv_label_create(info_container);
  lv_obj_align(label_favorites, LV_ALIGN_TOP_LEFT, 5, 180);
  lv_obj_set_style_text_color(label_favorites, lv_color_hex(0xcccccc), 0);
  lv_label_set_long_mode(label_favorites, LV_LABEL_LONG_DOT);
  lv_obj_set_width(label_favorites, 410);
  lv_obj_add_flag(label_favorites, LV_OBJ_FLAG_HIDDEN);

  label_counter = lv_label_create(info_container);
  lv_obj_align(label_counter, LV_ALIGN_TOP_RIGHT, -20, 135);
  lv_obj_set_style_text_color(label_counter, lv_color_hex(0x888888), 0);

  // Navigation
  btn_prev = lv_btn_create(scr);
  lv_obj_set_size(btn_prev, 120, 60);
  lv_obj_align(btn_prev, LV_ALIGN_BOTTOM_LEFT, 50, -30);
  lv_obj_add_event_cb(btn_prev, btn_prev_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_bg_color(btn_prev, lv_color_hex(getCurrentThemeColor()), 0);

  lv_obj_t *label_prev = lv_label_create(btn_prev);
  lv_label_set_text(label_prev, LV_SYMBOL_LEFT " PREV");
  lv_obj_center(label_prev);
  lv_obj_set_style_text_color(label_prev, lv_color_hex(0x000000), 0);

  btn_edit = lv_btn_create(scr);
  lv_obj_set_size(btn_edit, 120, 60);
  lv_obj_align(btn_edit, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_add_event_cb(
      btn_edit, [](lv_event_t *e) { show_edit_item_ui(getCurrentItemIndex()); },
      LV_EVENT_CLICKED, NULL);

  {
    lv_color_t theme = lv_color_hex(getCurrentThemeColor());
    lv_color_t red = lv_color_hex(0xFF0000);
    lv_color_t mixed = lv_color_mix(red, theme, 175);
    lv_obj_set_style_bg_color(btn_edit, mixed, 0);
  }

  lv_obj_t *label_edit = lv_label_create(btn_edit);
  lv_label_set_text(label_edit, LV_SYMBOL_EDIT " EDIT");
  lv_obj_center(label_edit);
  lv_obj_set_style_text_color(label_edit, lv_color_hex(0x000000), 0);

  btn_next = lv_btn_create(scr);
  lv_obj_set_size(btn_next, 120, 60);
  lv_obj_align(btn_next, LV_ALIGN_BOTTOM_RIGHT, -50, -30);
  lv_obj_add_event_cb(btn_next, btn_next_clicked, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(getCurrentThemeColor()), 0);

  lv_obj_t *label_next = lv_label_create(btn_next);
  lv_label_set_text(label_next, "NEXT " LV_SYMBOL_RIGHT);
  lv_obj_center(label_next);
  lv_obj_set_style_text_color(label_next, lv_color_hex(0x000000), 0);

  // --- Progress Monitor Timer ---
  lv_timer_create(
      [](lv_timer_t *t) {
        bool isBusy = BackgroundWorker::isBusy();

        // Create/Show Modal
        if (isBusy && !progress_modal) {
          progress_modal = lv_obj_create(lv_scr_act());
          lv_obj_set_size(progress_modal, 480, 200);
          lv_obj_center(progress_modal);
          lv_obj_add_style(progress_modal, &style_modal_panel, 0);
          lv_obj_set_style_bg_color(progress_modal, lv_color_hex(0x222222), 0);
          lv_obj_set_style_border_color(
              progress_modal, lv_color_hex(getCurrentThemeColor()), 0);

          lv_obj_t *title = lv_label_create(progress_modal);
          lv_label_set_text(title, "Processing...");
          lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
          lv_obj_add_style(title, &style_text_header, 0);

          progress_bar = lv_bar_create(progress_modal);
          lv_obj_set_size(progress_bar, 350, 25);
          lv_obj_center(progress_bar);
          lv_bar_set_range(progress_bar, 0, 100);
          lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
          lv_obj_set_style_bg_color(progress_bar,
                                    lv_color_hex(getCurrentThemeColor()),
                                    LV_PART_INDICATOR);

          progress_label = lv_label_create(progress_modal);
          lv_label_set_text(progress_label, "Preparing...");
          lv_obj_align(progress_label, LV_ALIGN_BOTTOM_MID, 0, -30);
          lv_obj_set_style_text_color(progress_label, lv_color_hex(0xcccccc),
                                      0);
          lv_label_set_long_mode(progress_label, LV_LABEL_LONG_DOT);
          lv_obj_set_width(progress_label, 400);
          lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, 0);
        }

        // Update
        if (isBusy && progress_modal) {
          int pct = (int)(BackgroundWorker::getProgress() * 100);
          lv_bar_set_value(progress_bar, pct, LV_ANIM_ON);
          String status = BackgroundWorker::getStatusMessage();
          lv_label_set_text(progress_label, status.c_str());
        }

        // Close
        if (!isBusy && progress_modal) {
          lv_obj_del(progress_modal);
          progress_modal = NULL; // Reset
          progress_bar = NULL;
          progress_label = NULL;

          // Show completion info
          // Note: using show_info_popup might act as double popup if multiple
          // jobs run But for Bulk Sync it's useful.
          String msg = BackgroundWorker::getStatusMessage();
          if (msg.length() > 0 && msg != "Idle") {
            if (msg == "Sync Complete") {
              show_info_popup(
                  "Task Finished", "Sync Complete. Tap OK to restart.",
                  [](lv_event_t *e) { ESP.restart(); }, NULL);
            } else {
              show_info_popup("Task Finished", msg.c_str(), NULL, NULL);
            }
          }
        }
      },
      200, NULL);

  Serial.println(">> setupMainUI Done");
}

// Implement the rest of the functions...

// Implement the rest of the functions...
// Helper to check if an item matches current filters
bool is_item_match(int index) {
  if (!filter_active)
    return true;

  // Use RAM-only access for bulk filtering to avoid SD bottleneck
  ItemView item = getItemAtRAM(index);
  if (!item.isValid)
    return false;

  if (filter_genre.length() > 0 && !item.genre.equalsIgnoreCase(filter_genre))
    return false;

  if (filter_decade > 0) {
    int decade = (item.year / 10) * 10;
    if (decade != (filter_decade + 1900))
      return false;
  }

  if (filter_favorites_only && !item.favorite)
    return false;

  return true;
}

void update_item_display() {
  // --- CACHED LOAD ---
  // We no longer call ensureItemDetailsLoaded(idx) here because
  // getItemAt(idx) uses the sliding window cache which pre-loads details.
  // Fallen-back getItemAtSD(idx) will handle details if it's a cache miss.
  int idx = getCurrentItemIndex();

  String d_title, d_artist_line, d_genre, d_year_line, d_led_text, d_notes;
  String d_uniqueID, d_barcode, d_coverFile, d_coverUrl;
  std::vector<int> d_ledIndices;
  bool d_favorite = false;
  bool d_hasData = false;
  int d_currentIndex = 0;
  int d_totalCount = 0;
  String d_counter_text = "";
  String d_extra_info = "";
  String favoritesLine = "";
  bool hasFavorites = false;

  // 1. Fetch Data based on Mode
  int currentIdx = getCurrentItemIndex();
  ItemView item = getItemAt(currentIdx);
  if (!item.isValid) {
    if (getItemCount() > 0) {
      setCurrentItemIndex(0);
      currentIdx = 0;
      item = getItemAt(currentIdx);
    }
    if (!item.isValid)
      return;
  }

  d_title = sanitizeText(item.title);
  d_artist_line = "by " + sanitizeText(item.artistOrAuthor);
  d_genre = "Genre: " + sanitizeText(item.genre);
  d_year_line = "Year: " + String(item.year);
  d_ledIndices = item.ledIndices;
  d_uniqueID = item.uniqueID;
  d_notes = item.notes;
  d_coverFile = item.coverFile;
  d_coverUrl = item.coverUrl;
  d_favorite = item.favorite;
  d_barcode = item.codecOrIsbn;
  d_extra_info = item.extraInfo;

  /*
  // DEBUG: Print complete item data
  Serial.println("=== ITEM VIEW DEBUG ===");
  Serial.printf("Title: %s\n", item.title.c_str());
  Serial.printf("Artist/Author: %s\n", item.artistOrAuthor.c_str());
  Serial.printf("Genre: %s\n", item.genre.c_str());
  Serial.printf("Year: %d\n", item.year);
  Serial.printf("UniqueID: %s\n", item.uniqueID.c_str());
  Serial.printf("Code/ISBN: %s\n", item.codecOrIsbn.c_str());
  Serial.printf("ExtraInfo: '%s'\n", item.extraInfo.c_str());
  Serial.printf("CoverFile: %s\n", item.coverFile.c_str());
  Serial.printf("Favorite: %s\n", item.favorite ? "Yes" : "No");
  String ledsStr = "";
  for (int l : item.ledIndices)
    ledsStr += String(l) + " ";
  Serial.printf("LEDs: %s\n", ledsStr.c_str());
  Serial.println("=======================");
  */

  d_hasData = true;
  d_currentIndex = currentIdx;
  d_totalCount = getItemCount();
  d_counter_text = getModeName() + " " + String(d_currentIndex + 1) + " of " +
                   String(d_totalCount);

  if (!d_hasData)
    return;

  // 2. Update UI Labels
  lv_label_set_text(label_title, d_title.c_str());
  lv_label_set_text(label_artist, d_artist_line.c_str());
  lv_label_set_text(label_genre, d_genre.c_str());
  lv_label_set_text(label_year, d_year_line.c_str());

  // LED Text
  String ledsShown = "";
  for (size_t i = 0; i < d_ledIndices.size(); i++) {
    ledsShown += String(d_ledIndices[i]);
    if (i < d_ledIndices.size() - 1)
      ledsShown += ", ";
  }
  lv_label_set_text_fmt(label_led, "ID: %s | LED No: %s", d_uniqueID.c_str(),
                        ledsShown.c_str());

  // Extra Info
  lv_label_set_text(label_extra_info, d_extra_info.c_str());

  // Text Colors (Dynamic update based on theme)
  uint32_t themeColor = getCurrentThemeColor();
  lv_obj_set_style_text_color(label_genre, lv_color_hex(themeColor), 0);
  lv_obj_set_style_text_color(label_year, lv_color_hex(themeColor), 0);
  lv_obj_set_style_text_color(label_extra_info, lv_color_hex(themeColor), 0);

  // Update Main Navigation Button Colors
  if (btn_prev)
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(themeColor), 0);
  if (btn_next)
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(themeColor), 0);
  if (btn_edit) {
    lv_color_t theme = lv_color_hex(themeColor);
    lv_color_t red = lv_color_hex(0xFF0000);
    lv_color_t mixed = lv_color_mix(red, theme, 175); // 77 is 30% Red
    lv_obj_set_style_bg_color(btn_edit, mixed, 0);
  }

  if (!d_favorite) {
    // Only update favorite label color if not favorited (red)
    lv_obj_set_style_text_color(label_favorite, lv_color_hex(themeColor), 0);
  }

  // Tracklist & Favorites (Mode Specific)
  if (hasTracklist()) {
    int idx = getCurrentItemIndex();
    if (idx >= 0 && idx < (int)cdLibrary.size()) {
      CD &cd = cdLibrary[idx];
      if (cd.releaseMbid.length() > 0) {
        TrackList *trackList = Storage.loadTracklist(cd.releaseMbid.c_str());
        if (trackList && trackList->tracks.size() > 0) {
          favoritesLine = "Fav: ";
          int favCount = 0;
          for (int i = 0; i < (int)trackList->tracks.size(); i++) {
            Track &track = trackList->tracks[i];
            if (track.isFavoriteTrack) {
              if (favCount > 0)
                favoritesLine += " | ";
              favoritesLine += LV_SYMBOL_BELL " " + String(track.trackNo) +
                               ". " + String(track.title.c_str());
              favCount++;
            }
          }
          Storage.deleteTracklist(trackList);
          if (favCount > 0)
            hasFavorites = true;
        }
      }
    }
  }

  // Notes
  if (d_notes.length() > 0) {
    lv_label_set_text_fmt(label_notes, "Notes: %s", d_notes.c_str());
    if (d_notes.length() > 50) {
      lv_label_set_long_mode(label_notes, LV_LABEL_LONG_SCROLL_CIRCULAR);
      lv_obj_set_style_anim_speed(label_notes, 40, 0);
    } else {
      lv_label_set_long_mode(label_notes, LV_LABEL_LONG_DOT);
    }
    lv_obj_clear_flag(label_notes, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(label_notes, LV_OBJ_FLAG_HIDDEN);
  }

  // Toggle Tracklist Button (Hide in Book Mode)
  if (btn_tracklist) {
    switch (currentMode) {
    case MODE_BOOK:
      lv_obj_add_flag(btn_tracklist, LV_OBJ_FLAG_HIDDEN);
      break;
    case MODE_CD:
    default:
      if (hasTracklist()) {
        lv_obj_clear_flag(btn_tracklist, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(btn_tracklist, LV_OBJ_FLAG_HIDDEN);
      }
      break;
    }
  }

  // Update FAVORITES label (Tracks)
  if (hasFavorites) {
    lv_label_set_text(label_favorites, favoritesLine.c_str());
    if (favoritesLine.length() > 50) {
      lv_label_set_long_mode(label_favorites, LV_LABEL_LONG_SCROLL_CIRCULAR);
      lv_obj_set_style_anim_speed(label_favorites, 40, 0);
    } else {
      lv_label_set_long_mode(label_favorites, LV_LABEL_LONG_DOT);
    }
    lv_obj_clear_flag(label_favorites, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(label_favorites, LV_OBJ_FLAG_HIDDEN);
  }

  // Counter
  lv_label_set_text(label_counter, d_counter_text.c_str());

  // Favorite Status (Entity level)
  if (d_favorite) {
    lv_label_set_text(label_favorite, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(label_favorite, lv_color_hex(0xff4444), 0);
  } else {
    lv_label_set_text(label_favorite, LV_SYMBOL_PLUS);
    // Color set above
  }

  // Cover Image
  String diskPath = "/covers/" + d_coverFile;
  bool fileExists = false;
  if (d_coverFile.length() > 0) {
    if (i2cMutex &&
        xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(50)) == pdPASS) {
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, LOW);
      fileExists = SD.exists(diskPath);
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
    } else {
      // Fallback: If we can't get lock, assume it might exist but we can't
      // check OR better, assume it doesn't to avoid a hang.
      fileExists = false;
    }
  }

  if (fileExists) {
    lv_obj_clear_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(img_cover, LV_OPA_TRANSP, 0);
    load_and_show_cover(d_coverFile);
    lv_obj_add_flag(label_cover_url, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_search, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(label_cover_url, "Click Search to find cover");
    lv_obj_clear_flag(label_cover_url, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(btn_search, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(btn_delete_cover, LV_OBJ_FLAG_HIDDEN);
  }

  // LED Update
  if (filter_active) {
    update_filtered_leds();
    return;
  }
  if (millis() < previewModeUntil)
    return;

  FastLED.clear();
  if (!led_master_on) {
    FastLED.show();
    return;
  }

  if (d_ledIndices.empty()) {
    // No LEDs
  } else {
    for (int idx : d_ledIndices) {
      if (idx >= 0 && idx < led_count) {
        if (d_favorite)
          leds[idx] = COLOR_FAVORITE;
        else
          leds[idx] = COLOR_SELECTED;
      }
    }
  }
  FastLED.show();

  if (led_use_wled)
    forceUpdateWLED();
}

void btn_prev_clicked(lv_event_t *e) {
  if (getItemCount() == 0)
    return;

  lvgl_port_lock(-1);
  int idx = getCurrentItemIndex();
  int start = idx;
  int candidate = idx;

  do {
    candidate--;
    if (candidate < 0) {
      candidate = getItemCount() - 1;
    }
    if (is_item_match(candidate)) {
      setCurrentItemIndex(candidate);
      shiftCacheWindow(false); // Shift cache backward
      break;
    }
  } while (candidate != start);

  // Re-center cache if idle for 10 seconds
  if (nav_idle_timer)
    lv_timer_del(nav_idle_timer);
  nav_idle_timer = lv_timer_create(
      [](lv_timer_t *t) {
        rebuildNavigationCache(getCurrentItemIndex());
        nav_idle_timer = NULL;
      },
      10000, NULL);
  lv_timer_set_repeat_count(nav_idle_timer, 1);

  update_item_display();
  lvgl_port_unlock();
}

void btn_next_clicked(lv_event_t *e) {
  if (getItemCount() == 0)
    return;

  lvgl_port_lock(-1);
  int idx = getCurrentItemIndex();
  int start = idx;
  int candidate = idx;

  do {
    candidate++;
    if (candidate >= getItemCount()) {
      candidate = 0;
    }
    if (is_item_match(candidate)) {
      setCurrentItemIndex(candidate);
      shiftCacheWindow(true); // Shift cache forward
      break;
    }
  } while (candidate != start);

  // Re-center cache if idle for 10 seconds
  if (nav_idle_timer)
    lv_timer_del(nav_idle_timer);
  nav_idle_timer = lv_timer_create(
      [](lv_timer_t *t) {
        rebuildNavigationCache(getCurrentItemIndex());
        nav_idle_timer = NULL;
      },
      10000, NULL);
  lv_timer_set_repeat_count(nav_idle_timer, 1);

  update_item_display();
  lvgl_port_unlock();
}

void btn_favorite_clicked(lv_event_t *e) {
  if (getItemCount() == 0)
    return;

  lvgl_port_lock(-1);
  int idx = getCurrentItemIndex();
  toggleFavoriteAt(idx);

  ItemView item = getItemAt(idx);
  bool isFav = item.favorite;
  String title = item.title;

  if (isFav) {
    lv_label_set_text(label_favorite, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(label_favorite, lv_color_hex(0xff4444), 0);
    Serial.printf("â­ Marked '%s' as favorite\n", title.c_str());
  } else {
    lv_label_set_text(label_favorite, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(label_favorite,
                                lv_color_hex(getCurrentThemeColor()), 0);
    Serial.printf("Unmarked '%s' as favorite\n", title.c_str());
  }

  lvgl_port_unlock();

  if (saveLibrary()) {
    Serial.println("Favorites saved to SD card!");
  } else {
    Serial.println("WARNING: Failed to save favorites!");
  }
}

void btn_delete_cover_clicked(lv_event_t *e) {
  int idx = getCurrentItemIndex();
  ItemView item = getItemAt(idx);
  if (item.coverFile.length() < 3)
    return;

  show_confirmation_popup(
      "Delete Cover", "Are you sure you want to delete this cover file?",
      [](lv_event_t *e) {
        int idx = getCurrentItemIndex();
        ItemView item = getItemAt(idx);
        String path = "/covers/" + item.coverFile;

        // 1. Delete from SD
        if (i2cMutex &&
            xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, LOW);
          if (SD.exists(path)) {
            SD.remove(path);
          }
          if (sdExpander)
            sdExpander->digitalWrite(SD_CS, HIGH);
          xSemaphoreGiveRecursive(i2cMutex);
        }

        // 2. Update model
        ensureItemDetailsLoaded(idx);
        if (currentMode == MODE_CD) {
          cdLibrary[idx].coverFile = "";
          Storage.saveCD(cdLibrary[idx]);
        } else {
          bookLibrary[idx].coverFile = "";
          Storage.saveBook(bookLibrary[idx]);
        }
        saveLibrary();
        update_item_display();
      },
      NULL, NULL);
}

void btn_search_clicked(lv_event_t *e) {
  if (getItemCount() == 0)
    return;

  lvgl_port_lock(-1);
  int idx = getCurrentItemIndex();
  ItemView item = getItemAt(idx);
  if (!item.isValid) {
    lvgl_port_unlock();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lv_label_set_text(label_cover_url, "No WiFi\nConnection!");
    lvgl_port_unlock();
    return;
  }

  lv_label_set_text(label_cover_url, "Searching...\nPlease wait");
  lv_refr_now(NULL);
  lvgl_port_unlock();

  String newUrl = "";

  // Fetch cover URL based on current mode
  switch (currentMode) {
  case MODE_BOOK: {
    Book tempBook;
    if (MediaManager::fetchBookByISBN(item.codecOrIsbn.c_str(), tempBook)) {
      newUrl = tempBook.coverUrl.c_str();
    }
    break;
  }
  case MODE_CD:
  default:
    newUrl = MediaManager::fetchAlbumCoverUrl(item.artistOrAuthor.c_str(),
                                              item.title.c_str());
    break;
  }

  lvgl_port_lock(-1);

  if (newUrl.length() > 0) {
    String uid = item.uniqueID;
    if (uid.length() == 0) {
      uid = String(millis()) + "_" + String(random(9999));
    }

    String fileName = getUidPrefix() + sanitizeFilename(uid) + ".jpg";
    setItemCoverUrl(idx, newUrl);
    setItemCoverFile(idx, fileName);

    Serial.printf("Found: %s\n", newUrl.c_str());

    lv_label_set_text(label_cover_url, "Downloading...\nPlease wait");
    lv_refr_now(NULL);
    lvgl_port_unlock();

    if (AppNetworkManager::downloadCoverImage(newUrl, "/covers/" + fileName)) {
      lvgl_port_lock(-1);
      lv_label_set_text_fmt(label_cover_url, "Success!\nSaved as %s",
                            fileName.c_str());

      ensureItemDetailsLoaded(idx);
      switch (currentMode) {
      case MODE_CD:
        cdLibrary[idx].coverFile = fileName.c_str();
        Storage.saveCD(cdLibrary[idx]);
        break;
      case MODE_BOOK:
        bookLibrary[idx].coverFile = fileName.c_str();
        Storage.saveBook(bookLibrary[idx]);
        break;
      }
      saveLibrary();
      update_item_display();
    } else {
      lvgl_port_lock(-1);
      lv_label_set_text(label_cover_url, "Download Failed!\nCheck WiFi/SD");
    }
  } else {
    setItemCoverFile(idx, "cover_default.jpg");
    Serial.println("Cover not found. Setting to default.");

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);
    if (SD.exists("/covers/cover_default.jpg")) {
      lv_label_set_text(label_cover_url, "Not Found on Web\nUsing Default");
    } else {
      lv_label_set_text(label_cover_url,
                        "Not Found.\n(Upload cover_default.jpg)");
    }
    saveLibrary();
    update_item_display();
  }
  lvgl_port_unlock();
}

void close_search_ui() {
  if (search_timer) {
    lv_timer_del(search_timer);
    search_timer = NULL;
  }
  if (search_panel) {
    lv_obj_del(search_panel);
    search_panel = NULL;
    ta_search = NULL;
    kb_search = NULL;
    dd_filter = NULL;
    list_results = NULL;
    update_item_display();
  }
}

static void result_click_cb(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  lvgl_port_lock(-1);
  setCurrentItemIndex(idx);
  update_item_display();
  close_search_ui();
  lvgl_port_unlock();
}

static void search_input_cb(lv_event_t *e) {
  if (search_timer)
    lv_timer_del(search_timer);

  search_timer = lv_timer_create(
      [](lv_timer_t *t) {
        if (ta_search) {
          const char *txt = lv_textarea_get_text(ta_search);
          filter_library(txt);
        }
        search_timer = NULL;
      },
      400, NULL);
  lv_timer_set_repeat_count(search_timer, 1);
}

static void render_search_batch() {
  if (!list_results)
    return;

  int end = search_display_offset + SEARCH_PAGE_SIZE;
  if (end > (int)search_matches.size())
    end = search_matches.size();

  lv_obj_t *last_child = lv_obj_get_child(list_results, -1);
  if (last_child) {
    lv_obj_t *label = lv_obj_get_child(last_child, 0);
    if (label && strcmp(lv_label_get_text(label), "Load More...") == 0) {
      lv_obj_del(last_child);
    }
  }

  for (int i = search_display_offset; i < end; i++) {
    int libraryIdx = search_matches[i];
    ItemView item = getItemAt(libraryIdx);
    if (!item.isValid)
      continue;

    String labelStr = item.artistOrAuthor + " - " + item.title;
    lv_obj_t *btn =
        lv_list_add_btn(list_results, LV_SYMBOL_AUDIO, labelStr.c_str());
    lv_obj_add_event_cb(btn, result_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)libraryIdx);
  }

  search_display_offset = end;

  if (search_display_offset < (int)search_matches.size()) {
    lv_obj_t *btn_more =
        lv_list_add_btn(list_results, LV_SYMBOL_DOWN, "Load More...");
    lv_obj_add_event_cb(
        btn_more, [](lv_event_t *e) { render_search_batch(); },
        LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(btn_more, lv_color_hex(0x333333), 0);
  }

  if (search_matches.size() == 0) {
    const char *q = lv_textarea_get_text(ta_search);
    lv_list_add_text(list_results, strlen(q) == 0 ? "Enter search term..."
                                                  : "No matches found");
  }
}

void filter_library(const char *query) {
  if (!list_results)
    return;
  lv_obj_clean(list_results);
  int filter_mode = lv_dropdown_get_selected(dd_filter);
  MediaManager::filter(query, filter_mode, led_master_on);
  render_search_batch();
}

void show_search_ui() {
  if (search_panel)
    return;
  lvgl_port_lock(-1);

  search_panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(search_panel, 800, 480);
  lv_obj_center(search_panel);
  lv_obj_add_style(search_panel, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(search_panel, lv_color_hex(0x0d0d0d), 0);
  lv_obj_set_scroll_dir(search_panel, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(search_panel, LV_SCROLLBAR_MODE_ON);
  lv_obj_set_style_pad_right(search_panel, 12, 0);
  lv_obj_set_style_bg_color(search_panel, lv_color_hex(getCurrentThemeColor()),
                            LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(search_panel, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(search_panel, 8, LV_PART_SCROLLBAR);

  lv_obj_t *btn_led_search = lv_btn_create(search_panel);
  lv_obj_set_size(btn_led_search, 40, 40);
  lv_obj_align(btn_led_search, LV_ALIGN_TOP_RIGHT, -190, 10);
  lv_obj_add_style(btn_led_search, &style_btn_header_green, 0);

  lv_obj_t *label_led_search = lv_label_create(btn_led_search);
  lv_label_set_text(label_led_search,
                    led_master_on ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
  lv_obj_center(label_led_search);
  lv_obj_set_style_text_color(label_led_search,
                              led_master_on
                                  ? lv_color_hex(getCurrentThemeColor())
                                  : lv_color_hex(0x888888),
                              0);

  lv_obj_add_event_cb(
      btn_led_search,
      [](lv_event_t *e) {
        led_master_on = !led_master_on;
        lv_obj_t *btn = lv_event_get_target(e);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (led_master_on) {
          lv_label_set_text(lbl, LV_SYMBOL_EYE_OPEN);
          lv_obj_set_style_text_color(lbl, lv_color_hex(getCurrentThemeColor()),
                                      0);
        } else {
          lv_label_set_text(lbl, LV_SYMBOL_EYE_CLOSE);
          lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        }
        if (filter_active)
          update_filtered_leds();
        else
          update_item_display();
        if (led_use_wled)
          forceUpdateWLED();
      },
      LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_toggle_kb = lv_btn_create(search_panel);
  lv_obj_set_size(btn_toggle_kb, 100, 40);
  lv_obj_align(btn_toggle_kb, LV_ALIGN_TOP_RIGHT, -80, 10);
  lv_obj_set_style_bg_color(btn_toggle_kb, lv_color_hex(0x444444), 0);
  lv_obj_t *label_toggle_kb = lv_label_create(btn_toggle_kb);
  lv_label_set_text(label_toggle_kb, LV_SYMBOL_KEYBOARD " HIDE");
  lv_obj_center(label_toggle_kb);
  lv_obj_set_style_text_color(label_toggle_kb, lv_color_hex(0xffffff), 0);

  lv_obj_t *btn_close = lv_btn_create(search_panel);
  lv_obj_set_size(btn_close, 60, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btn_close, &style_btn_close, 0);
  lv_obj_add_event_cb(
      btn_close, [](lv_event_t *e) { close_search_ui(); }, LV_EVENT_CLICKED,
      NULL);
  lv_obj_t *label_close = lv_label_create(btn_close);
  lv_label_set_text(label_close, LV_SYMBOL_CLOSE);
  lv_obj_center(label_close);
  lv_obj_set_style_text_color(label_close, lv_color_hex(0xff4444), 0);

  lv_obj_t *title = lv_label_create(search_panel);
  String searchTitle = " SEARCH " + getModeNamePlural();
  lv_label_set_text(title, (LV_SYMBOL_LIST + searchTitle).c_str());
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 15);
  lv_obj_set_style_text_color(title, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

  lv_obj_t *btn_sort = lv_btn_create(search_panel);
  lv_obj_set_size(btn_sort, 100, 40);
  lv_obj_align(btn_sort, LV_ALIGN_TOP_LEFT, 20, 65);
  lv_obj_set_style_bg_color(btn_sort, lv_color_hex(0x444444), 0);
  lv_obj_t *label_sort = lv_label_create(btn_sort);
  lv_label_set_text(label_sort, LV_SYMBOL_LIST " ID");
  lv_obj_center(label_sort);
  lv_obj_set_style_text_color(label_sort, lv_color_hex(0xffffff), 0);

  lv_obj_add_event_cb(
      btn_sort,
      [](lv_event_t *e) {
        lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
        sort_by_artist = !sort_by_artist;
        if (sort_by_artist) {
          MediaManager::sortByArtistOrAuthor();
          String sortLabel = " " + getArtistOrAuthorLabelUpper();
          lv_label_set_text(label, (LV_SYMBOL_LIST + sortLabel).c_str());
        } else {
          MediaManager::sortByLedIndex();
          lv_label_set_text(label, LV_SYMBOL_LIST " ID");
        }
        const char *current_query = lv_textarea_get_text(ta_search);
        filter_library(current_query);
      },
      LV_EVENT_CLICKED, label_sort);

  dd_filter = lv_dropdown_create(search_panel);
  String artistOrAuthor = getArtistOrAuthorLabel();
  String filterOptions = "All\nTitle\n" + artistOrAuthor + "\nGenre";
  lv_dropdown_set_options(dd_filter, filterOptions.c_str());
  lv_obj_set_width(dd_filter, 100);
  lv_obj_align(dd_filter, LV_ALIGN_TOP_LEFT, 130, 65);
  lv_obj_set_style_bg_color(dd_filter, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(dd_filter, lv_color_hex(getCurrentThemeColor()),
                              0);
  lv_obj_set_style_border_color(dd_filter, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_set_style_border_width(dd_filter, 1, 0);

  ta_search = lv_textarea_create(search_panel);
  lv_obj_set_size(ta_search, 340, 40);
  lv_obj_align(ta_search, LV_ALIGN_TOP_LEFT, 240, 65);
  lv_textarea_set_placeholder_text(ta_search, "Type to search...");
  lv_obj_set_style_bg_color(ta_search, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_search, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_search, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_search, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(ta_search, 1, 0);
  lv_obj_add_event_cb(ta_search, search_input_cb, LV_EVENT_VALUE_CHANGED, NULL);

  list_results = lv_list_create(search_panel);
  lv_obj_set_size(list_results, 760, 450);
  lv_obj_align(list_results, LV_ALIGN_TOP_MID, 0, 115);
  lv_obj_set_style_bg_color(list_results, lv_color_hex(0x0d0d0d), 0);
  lv_obj_set_style_border_color(list_results, lv_color_hex(0x333333), 0);
  lv_obj_set_style_border_width(list_results, 1, 0);
  lv_obj_set_style_radius(list_results, 5, 0);

  kb_search = lv_keyboard_create(search_panel);
  lv_obj_set_size(kb_search, 780, 200);
  lv_obj_align(kb_search, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_flag(kb_search, LV_OBJ_FLAG_HIDDEN);

  // Apply dark theme styling to keyboard
  lv_obj_set_style_bg_color(kb_search, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(kb_search, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_color(kb_search, lv_color_hex(0x2a2a2a), LV_PART_ITEMS);
  lv_obj_set_style_text_color(kb_search, lv_color_hex(0xffffff), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kb_search, lv_color_hex(getCurrentThemeColor()),
                            LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(kb_search, lv_color_hex(0x444444),
                            LV_PART_ITEMS | LV_STATE_CHECKED);

  lv_obj_add_event_cb(
      ta_search,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_FOCUSED, kb_search);

  static lv_obj_t *toggle_data[2];
  toggle_data[0] = kb_search;
  toggle_data[1] = label_toggle_kb;
  lv_obj_add_event_cb(
      btn_toggle_kb,
      [](lv_event_t *e) {
        lv_obj_t **data = (lv_obj_t **)lv_event_get_user_data(e);
        if (!data)
          return;
        lv_obj_t *kb = data[0];
        lv_obj_t *label = data[1];
        if (lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " HIDE");
        } else {
          lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " SHOW");
        }
      },
      LV_EVENT_CLICKED, toggle_data);

  lv_keyboard_set_textarea(kb_search, ta_search);
  lv_obj_clear_flag(kb_search, LV_OBJ_FLAG_HIDDEN);

  filter_library("");
  lvgl_port_unlock();
}
void load_and_show_cover(String filename) {
  if (img_buffer == NULL) {
    // Try PSRAM first
    img_buffer = (uint16_t *)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (img_buffer == NULL) {
      Serial.println("Warning: Allocating image buffer in Internal RAM");
      img_buffer = (uint16_t *)malloc(240 * 240 * 2);
    }
  }

  if (img_buffer == NULL) {
    Serial.println("CRITICAL: Failed to allocate image buffer!");
    return;
  }

  // Clear buffer with container background color (0x333333 -> 0x3186 in RGB565)
  for (int i = 0; i < 240 * 240; i++) {
    img_buffer[i] = 0x3186;
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tjpg_output);

  if (i2cMutex &&
      xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdPASS) {
    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, LOW);

    File f = SD.open("/covers/" + filename, FILE_READ);
    if (!f) {
      Serial.printf("Failed to open file: %s\n", filename.c_str());
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
      return;
    }

    size_t jpg_size = f.size();
    uint8_t *jpg_data = (uint8_t *)malloc(jpg_size);
    if (jpg_data == NULL) {
      Serial.println("Failed to malloc buffer for JPG file!");
      f.close();
      if (sdExpander)
        sdExpander->digitalWrite(SD_CS, HIGH);
      xSemaphoreGiveRecursive(i2cMutex);
      return;
    }

    f.read(jpg_data, jpg_size);
    f.close();

    if (sdExpander)
      sdExpander->digitalWrite(SD_CS, HIGH);
    xSemaphoreGiveRecursive(i2cMutex);

    // Get dimensions and center
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, jpg_data, jpg_size);
    int off_x = (240 - w) / 2;
    int off_y = (240 - h) / 2;
    if (off_x < 0)
      off_x = 0;
    if (off_y < 0)
      off_y = 0;

    TJpgDec.drawJpg(off_x, off_y, jpg_data, jpg_size);
    free(jpg_data);
  } else {
    Serial.println(
        "load_and_show_cover: Failed to get I2C lock, skipping image load");
  }

  raw_img_dsc.header.always_zero = 0;
  raw_img_dsc.header.w = 240;
  raw_img_dsc.header.h = 240;
  raw_img_dsc.data_size = 240 * 240 * 2;
  raw_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  raw_img_dsc.data = (const uint8_t *)img_buffer;

  lv_img_set_src(img_cover, &raw_img_dsc);
  lv_obj_clear_flag(img_cover, LV_OBJ_FLAG_HIDDEN);
}

void update_filtered_leds() {
  if (!led_master_on) {
    FastLED.clear();
    FastLED.show();
    if (led_use_wled)
      forceUpdateWLED();
    return;
  }

  FastLED.clear();

  int total = getItemCount();

  for (int i = 0; i < total; i++) {
    if (is_item_match(i)) {
      std::vector<int> indices = getItemLedIndices(i);
      for (int idx : indices) {
        if (idx >= 0 && idx < led_count) {
          leds[idx] = COLOR_FILTERED;
        }
      }
    }
  }

  if (getCurrentItemIndex() >= 0 && getCurrentItemIndex() < total) {
    std::vector<int> selIndices = getItemLedIndices(getCurrentItemIndex());
    for (int idx : selIndices) {
      if (idx >= 0 && idx < led_count) {
        leds[idx] = COLOR_SELECTED;
      }
    }
  }

  FastLED.show();
  if (led_use_wled)
    forceUpdateWLED();
}

// ==========================================
// WIFI CONFIGURATION UI
// ==========================================

void show_wifi_config_ui() {
  if (wifi_config_panel)
    return; // Already open

  lvgl_port_lock(-1);

  wifi_config_panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifi_config_panel, 630, 450);
  lv_obj_center(wifi_config_panel);
  lv_obj_add_style(wifi_config_panel, &style_modal_panel,
                   0); // Apply Modal Style
  lv_obj_set_style_bg_color(wifi_config_panel, lv_color_hex(0x0d0d0d),
                            0); // Slightly darker
  lv_obj_set_scroll_dir(wifi_config_panel,
                        LV_DIR_VER); // Enable vertical scrolling
  lv_obj_set_scrollbar_mode(wifi_config_panel,
                            LV_SCROLLBAR_MODE_ON); // Always show scrollbar
  lv_obj_set_style_pad_right(wifi_config_panel, 12,
                             0); // Make room for scrollbar
  // Make scrollbar more visible
  lv_obj_set_style_bg_color(wifi_config_panel,
                            lv_color_hex(getCurrentThemeColor()),
                            LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(wifi_config_panel, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(wifi_config_panel, 8,
                         LV_PART_SCROLLBAR); // Wider scrollbar

  // Title
  lv_obj_t *title = lv_label_create(wifi_config_panel);
  lv_label_set_text(title, LV_SYMBOL_WIFI " WIFI SETTINGS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
  lv_obj_add_style(title, &style_text_header, 0); // Apply Global Header Style

  // Hide/Show Keyboard Toggle (before close button)
  lv_obj_t *btn_toggle_kb_wifi = lv_btn_create(wifi_config_panel);
  lv_obj_set_size(btn_toggle_kb_wifi, 100, 40);
  lv_obj_align(btn_toggle_kb_wifi, LV_ALIGN_TOP_RIGHT, -70, 10);
  lv_obj_set_style_bg_color(btn_toggle_kb_wifi, lv_color_hex(0x444444), 0);
  lv_obj_t *label_toggle_kb_wifi = lv_label_create(btn_toggle_kb_wifi);
  lv_label_set_text(label_toggle_kb_wifi, LV_SYMBOL_KEYBOARD " SHOW");
  lv_obj_center(label_toggle_kb_wifi);
  lv_obj_set_style_text_color(label_toggle_kb_wifi, lv_color_hex(0xffffff), 0);

  // Close button
  lv_obj_t *btn_close = lv_btn_create(wifi_config_panel);
  lv_obj_set_size(btn_close, 50, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btn_close, &style_btn_close, 0); // Apply Global Style
  lv_obj_add_event_cb(
      btn_close, [](lv_event_t *e) { close_wifi_config_ui(); },
      LV_EVENT_CLICKED, NULL);
  lv_obj_t *label_close = lv_label_create(btn_close);
  lv_label_set_text(label_close, LV_SYMBOL_CLOSE);
  lv_obj_center(label_close);
  lv_obj_set_style_text_color(label_close, lv_color_hex(0xff4444), 0);

  // Connect button (top left)
  lv_obj_t *btn_connect = lv_btn_create(wifi_config_panel);
  lv_obj_set_size(btn_connect, 120, 40);
  lv_obj_align(btn_connect, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_set_style_bg_color(btn_connect, lv_color_hex(getCurrentThemeColor()),
                            0);
  lv_obj_t *label_connect = lv_label_create(btn_connect);
  lv_label_set_text(label_connect, LV_SYMBOL_WIFI " CONNECT");
  lv_obj_center(label_connect);
  lv_obj_set_style_text_color(label_connect, lv_color_hex(0x000000), 0);

  // Saved Networks List Section
  lv_obj_t *label_saved = lv_label_create(wifi_config_panel);
  lv_label_set_text_fmt(label_saved,
                        "Saved Networks (%d):", savedWiFiNetworks.size());
  lv_obj_align(label_saved, LV_ALIGN_TOP_LEFT, 30, 70);
  lv_obj_set_style_text_color(label_saved, lv_color_hex(getCurrentThemeColor()),
                              0);
  lv_obj_set_style_text_font(label_saved, &lv_font_montserrat_14, 0);

  // Create a container for the network list
  int list_y = 95;
  String currentSSID = WiFi.SSID();

  // Use a local array for delete indices to bypass capture issues
  static int delete_indices[MAX_WIFI_NETWORKS];

  for (int i = 0; i < (int)savedWiFiNetworks.size() && i < MAX_WIFI_NETWORKS;
       i++) {
    // Network item container
    lv_obj_t *net_container = lv_obj_create(wifi_config_panel);
    lv_obj_set_size(net_container, 530, 40);
    lv_obj_align(net_container, LV_ALIGN_TOP_LEFT, 30, list_y);
    lv_obj_set_style_bg_color(net_container, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(net_container, 1, 0);
    lv_obj_set_style_border_color(net_container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(net_container, 5, 0);
    lv_obj_set_style_pad_all(net_container, 5, 0);
    lv_obj_clear_flag(net_container, LV_OBJ_FLAG_SCROLLABLE);

    // Network name label
    lv_obj_t *net_label = lv_label_create(net_container);
    String displayText = savedWiFiNetworks[i].ssid;

    // Add indicator if this is the currently connected network
    if (WiFi.status() == WL_CONNECTED &&
        savedWiFiNetworks[i].ssid == currentSSID) {
      displayText = LV_SYMBOL_WIFI " " + displayText + " (Connected)";
      lv_obj_set_style_text_color(net_label,
                                  lv_color_hex(getCurrentThemeColor()), 0);
    } else {
      displayText = String(i + 1) + ". " + displayText;
      lv_obj_set_style_text_color(net_label, lv_color_hex(0xffffff), 0);
    }

    lv_label_set_text(net_label, displayText.c_str());
    lv_obj_align(net_label, LV_ALIGN_LEFT_MID, 5, 0);

    // Delete button
    lv_obj_t *btn_delete = lv_btn_create(net_container);
    lv_obj_set_size(btn_delete, 60, 30);
    lv_obj_align(btn_delete, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0xff4444), 0);

    lv_obj_t *label_delete = lv_label_create(btn_delete);
    lv_label_set_text(label_delete, LV_SYMBOL_TRASH);
    lv_obj_center(label_delete);

    delete_indices[i] = i;

    lv_obj_add_event_cb(
        btn_delete,
        [](lv_event_t *e) {
          int *idx = (int *)lv_event_get_user_data(e);
          if (idx && *idx >= 0 && *idx < (int)savedWiFiNetworks.size()) {
            AppNetworkManager::removeWiFiNetwork(*idx);
            // Refresh the WiFi config UI
            close_wifi_config_ui();
            show_wifi_config_ui();
          }
        },
        LV_EVENT_CLICKED, &delete_indices[i]);

    list_y += 45; // Move down for next item
  }

  int input_start_y = list_y + 20; // Start input fields after the list

  // Separator line
  lv_obj_t *separator = lv_obj_create(wifi_config_panel);
  lv_obj_set_size(separator, 530, 2);
  lv_obj_align(separator, LV_ALIGN_TOP_LEFT, 30, list_y + 5);
  lv_obj_set_style_bg_color(separator, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_border_width(separator, 0, 0);
  lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);

  // "Add New Network" label
  lv_obj_t *label_add_new = lv_label_create(wifi_config_panel);
  lv_label_set_text(label_add_new, LV_SYMBOL_PLUS " Add New Network");
  lv_obj_align(label_add_new, LV_ALIGN_TOP_LEFT, 30, input_start_y);
  lv_obj_set_style_text_color(label_add_new,
                              lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_text_font(label_add_new, &lv_font_montserrat_14, 0);

  input_start_y += 30;

  // SSID field
  lv_obj_t *label_ssid = lv_label_create(wifi_config_panel);
  lv_label_set_text(label_ssid, "WiFi Name (SSID):");
  lv_obj_align_to(label_ssid, label_add_new, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  lv_obj_set_style_text_color(label_ssid, lv_color_hex(0xaaaaaa), 0);

  ta_ssid = lv_textarea_create(wifi_config_panel);
  lv_obj_set_size(ta_ssid, 500, 40);
  lv_obj_align_to(ta_ssid, label_ssid, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  lv_textarea_set_one_line(ta_ssid, true);
  lv_textarea_set_placeholder_text(ta_ssid, "Enter WiFi name...");
  // Pre-fill with first saved network (if any)
  if (savedWiFiNetworks.size() > 0) {
    lv_textarea_set_text(ta_ssid, savedWiFiNetworks[0].ssid.c_str());
  }
  lv_obj_set_style_bg_color(ta_ssid, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_ssid, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_ssid, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_ssid, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  // Password field
  lv_obj_t *label_password = lv_label_create(wifi_config_panel);
  lv_label_set_text(label_password, "Password:");
  lv_obj_align_to(label_password, ta_ssid, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  lv_obj_set_style_text_color(label_password, lv_color_hex(0xaaaaaa), 0);

  ta_password = lv_textarea_create(wifi_config_panel);
  lv_obj_set_size(ta_password, 500, 40);
  lv_obj_align_to(ta_password, label_password, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
  lv_textarea_set_one_line(ta_password, true);
  lv_textarea_set_placeholder_text(ta_password, "Enter password...");
  lv_textarea_set_password_mode(ta_password, true);
  lv_obj_set_style_bg_color(ta_password, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_password, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_password,
                                lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_style(ta_password, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  // Current WiFi Status (positioned after password field)
  lv_obj_t *status_label = lv_label_create(wifi_config_panel);
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(status_label, "Connected to: %s",
                          WiFi.SSID().c_str());
    lv_obj_set_style_text_color(status_label,
                                lv_color_hex(getCurrentThemeColor()), 0);
  } else {
    lv_label_set_text(status_label, "Not Connected");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xff8800), 0);
  }
  lv_obj_align_to(status_label, ta_password, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 20);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);

  // Keyboard (positioned after status label in content flow)
  kb_wifi = lv_keyboard_create(wifi_config_panel);
  lv_obj_set_size(kb_wifi, 580, 120);
  lv_obj_align_to(kb_wifi, status_label, LV_ALIGN_OUT_BOTTOM_LEFT, -20, 20);
  lv_obj_set_style_bg_color(kb_wifi, lv_color_hex(0x1a1a1a), 0);
  lv_obj_add_flag(kb_wifi, LV_OBJ_FLAG_HIDDEN); // Hidden by default

  // Event callback to show keyboard when text area is focused
  auto ta_focus_cb = [](lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);

    // Scroll the text area into view to prevent keyboard overlap
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
  };

  lv_obj_add_event_cb(ta_ssid, ta_focus_cb, LV_EVENT_FOCUSED, kb_wifi);
  lv_obj_add_event_cb(ta_password, ta_focus_cb, LV_EVENT_FOCUSED, kb_wifi);

  // Add toggle button callback
  static lv_obj_t *toggle_data_wifi[2];
  toggle_data_wifi[0] = kb_wifi;
  toggle_data_wifi[1] = label_toggle_kb_wifi;
  lv_obj_add_event_cb(
      btn_toggle_kb_wifi,
      [](lv_event_t *e) {
        lv_obj_t **data = (lv_obj_t **)lv_event_get_user_data(e);
        if (!data)
          return;
        lv_obj_t *kb = data[0];
        lv_obj_t *label = data[1];

        // Toggle keyboard visibility
        if (lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " HIDE");
        } else {
          lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " SHOW");
        }
      },
      LV_EVENT_CLICKED, toggle_data_wifi);

  // Add connect button callback
  lv_obj_add_event_cb(
      btn_connect,
      [](lv_event_t *e) {
        const char *new_ssid = lv_textarea_get_text(ta_ssid);
        const char *new_password = lv_textarea_get_text(ta_password);

        if (strlen(new_ssid) == 0) {
          Serial.println("SSID cannot be empty!");
          return;
        }

        Serial.printf("Connecting to WiFi: %s\n", new_ssid);
        WiFi.disconnect();
        WiFi.begin(new_ssid, new_password);

        // Show connecting status - find status label (it's the 8th child now)
        lv_obj_t *status = NULL;
        for (int i = 0; i < lv_obj_get_child_cnt(wifi_config_panel); i++) {
          lv_obj_t *child = lv_obj_get_child(wifi_config_panel, i);
          // Look for the status label by checking if it contains text
          if (lv_obj_check_type(child, &lv_label_class)) {
            const char *text = lv_label_get_text(child);
            if (strstr(text, "Connected") || strstr(text, "Not Connected")) {
              status = child;
              break;
            }
          }
        }

        if (status) {
          lv_label_set_text(status, "Connecting...");
          lv_obj_set_style_text_color(status, lv_color_hex(0xffdd00), 0);
        }

        // Wait for connection (with timeout)
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
          delay(500);
          attempts++;
          lv_timer_handler(); // Keep UI responsive
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("WiFi connected!");
          if (status) {
            lv_label_set_text_fmt(status, "Connected to: %s",
                                  WiFi.SSID().c_str());
            lv_obj_set_style_text_color(
                status, lv_color_hex(getCurrentThemeColor()), 0);
          }

          // CRITICAL: Add network to saved list
          AppNetworkManager::addWiFiNetwork(String(new_ssid),
                                            String(new_password));

          // Close the modal after successful connection
          delay(1000);
          close_wifi_config_ui();
          ESP.restart(); // Restart to update WiFi icon
        } else {
          Serial.println("WiFi connection failed!");
          if (status) {
            lv_label_set_text(status, "Connection Failed!");
            lv_obj_set_style_text_color(status, lv_color_hex(0xff4444), 0);
          }
        }
      },
      LV_EVENT_CLICKED, NULL);

  lvgl_port_unlock();
}

void close_wifi_config_ui() {
  if (!wifi_config_panel)
    return;

  lvgl_port_lock(-1);
  lv_obj_del(wifi_config_panel);
  wifi_config_panel = NULL;
  ta_ssid = NULL;
  ta_password = NULL;
  kb_wifi = NULL;
  lvgl_port_unlock();
}

void show_edit_item_ui(int index) {
  edit_item_index = index;
  if (index < 0 || index >= getItemCount())
    return;

  // ESSENTIAL: Load full details (MBID, Tracks) before editing to prevent data
  // loss!
  ensureItemDetailsLoaded(index);

  // Local vars for UI pre-fill
  String e_uniqueID, e_barcode, e_title, e_artist, e_genre, e_notes;
  int e_year = 0;
  std::vector<int> e_ledIndices;

  switch (currentMode) {
  case MODE_BOOK: {
    if (index >= (int)bookLibrary.size())
      return;
    currentEditBook = bookLibrary[index];
    e_uniqueID = currentEditBook.uniqueID.c_str();
    e_barcode = currentEditBook.isbn.c_str();
    e_title = currentEditBook.title.c_str();
    e_artist = currentEditBook.author.c_str();
    e_genre = currentEditBook.genre.c_str();
    e_year = currentEditBook.year;
    e_notes = currentEditBook.notes.c_str();
    e_ledIndices = currentEditBook.ledIndices;
  } break;
  case MODE_CD:
  default: {
    if (index >= (int)cdLibrary.size())
      return;
    currentEditCD = cdLibrary[index];
    e_uniqueID = currentEditCD.uniqueID.c_str();
    e_barcode = currentEditCD.barcode.c_str();
    e_title = currentEditCD.title.c_str();
    e_artist = currentEditCD.artist.c_str();
    e_genre = currentEditCD.genre.c_str();
    e_year = currentEditCD.year;
    e_notes = currentEditCD.notes.c_str();
    e_ledIndices = currentEditCD.ledIndices;
  } break;
  }

  // Set edit mode BEFORE creating the UI
  // edit_item_index = index; // This line is replaced by edit_item_index =
  // index; at the top
  show_add_item_ui(); // Renamed from show_add_item_ui

  // Pre-fill with existing data
  lvgl_port_lock(-1);
  if (ta_uniqueID)
    lv_textarea_set_text(ta_uniqueID, e_uniqueID.c_str());
  if (ta_barcode)
    lv_textarea_set_text(ta_barcode, e_barcode.c_str());

  lv_textarea_set_text(ta_title, e_title.c_str());
  lv_textarea_set_text(ta_artist, e_artist.c_str());
  lv_textarea_set_text(ta_genre, e_genre.c_str());
  lv_textarea_set_text(ta_year, String(e_year).c_str());

  // Convert vector to CSV string
  String ledStr = "";
  for (size_t i = 0; i < e_ledIndices.size(); i++) {
    ledStr += String(e_ledIndices[i]);
    if (i < e_ledIndices.size() - 1)
      ledStr += ",";
  }
  lv_textarea_set_text(ta_led_index, ledStr.c_str());

  if (ta_notes)
    lv_textarea_set_text(ta_notes, e_notes.c_str());

  // Update title to show "EDIT CD" or "EDIT BOOK"
  lv_obj_t *title = lv_obj_get_child(
      add_item_panel, 0); // add_item_panel should be add_item_panel
  String editTitle = " EDIT " + getModeName();
  lv_label_set_text(title, (LV_SYMBOL_EDIT + editTitle).c_str());

  // Add Delete button (only in Edit mode) - top left, next to SAVE
  lv_obj_t *btn_delete =
      lv_btn_create(add_item_panel); // add_item_panel should be add_item_panel
  lv_obj_set_size(btn_delete, 120, 40);
  lv_obj_align(btn_delete, LV_ALIGN_TOP_LEFT, 140, 10);
  lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0xff4444), 0);

  lv_obj_add_event_cb(
      btn_delete,
      [](lv_event_t *e) {
        if (edit_item_index < 0 ||
            edit_item_index >= getItemCount()) // Renamed from edit_item_index
          return;

        String delTitle;
        switch (currentMode) {
        case MODE_BOOK:
          if (edit_item_index < (int)bookLibrary.size()) {
            delTitle = bookLibrary[edit_item_index].title.c_str();
          }
          break;
        case MODE_CD:
        default:
          if (edit_item_index < (int)cdLibrary.size()) {
            delTitle = cdLibrary[edit_item_index].title.c_str();
          }
          break;
        }

        // Create confirmation dialog
        lvgl_port_lock(-1);
        String title = "Delete " + getModeName() + "?";
        lv_obj_t *mbox = lv_msgbox_create(
            NULL, title.c_str(), ("Delete \"" + delTitle + "\"?").c_str(), NULL,
            true);
        lv_obj_set_style_bg_color(mbox, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_text_color(mbox, lv_color_hex(0xffffff), 0);
        lv_obj_center(mbox);

        // YES Button
        lv_obj_t *btn_yes = lv_btn_create(mbox);
        lv_obj_set_size(btn_yes, 100, 40);
        lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 50, -20);
        lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0xff4444), 0);

        lv_obj_add_event_cb(
            btn_yes,
            [](lv_event_t *e) {
              lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);

              if (edit_item_index >= 0 &&
                  edit_item_index <
                      getItemCount()) { // Renamed from edit_item_index
                // Use abstraction layer for deletion
                if (deleteItemAt(
                        edit_item_index)) { // Renamed from edit_item_index
                  Serial.println("✅ Deleted successfully!");

                  // Adjust current index if needed
                  if (currentCDIndex >= getItemCount())
                    currentCDIndex = getItemCount() - 1;
                  if (currentCDIndex < 0)
                    currentCDIndex = 0;

                  update_item_display();
                  close_add_item_ui(); // Renamed from close_add_item_ui
                } else {
                  Serial.println("❌ Failed to delete!");
                }
              }
              lv_msgbox_close(mbox);
            },
            LV_EVENT_CLICKED, mbox);

        lv_obj_t *label_yes = lv_label_create(btn_yes);
        lv_label_set_text(label_yes, "YES");
        lv_obj_center(label_yes);

        // NO Button
        lv_obj_t *btn_no = lv_btn_create(mbox);
        lv_obj_set_size(btn_no, 100, 40);
        lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
        lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x444444), 0);

        lv_obj_add_event_cb(
            btn_no,
            [](lv_event_t *e) {
              lv_obj_t *mbox = (lv_obj_t *)lv_event_get_user_data(e);
              lv_msgbox_close(mbox);
            },
            LV_EVENT_CLICKED, mbox);

        lv_obj_t *label_no = lv_label_create(btn_no);
        lv_label_set_text(label_no, "NO");
        lv_obj_center(label_no);

        lvgl_port_unlock();
      },
      LV_EVENT_CLICKED, NULL);

  lv_obj_t *label_delete = lv_label_create(btn_delete);
  lv_label_set_text(label_delete, LV_SYMBOL_TRASH " DELETE");
  lv_obj_center(label_delete);
  lv_obj_set_style_text_color(label_delete, lv_color_hex(0xffffff), 0);

  lvgl_port_unlock();
}

// --- Add/Edit Callbacks ---
void manual_entry_confirm_cb(lv_event_t *e) {
  lv_obj_t *mbox = lv_event_get_current_target(e);
  const char *btn_txt = lv_msgbox_get_active_btn_text(mbox);
  if (String(btn_txt) == "Yes") {
    if (ta_title)
      lv_obj_add_state(ta_title, LV_STATE_FOCUSED);
  } else {
    if (ta_barcode)
      lv_textarea_set_text(ta_barcode, "");
  }
  lv_msgbox_close(mbox);
}

void fetch_barcode_cb(lv_event_t *e) {
  const char *barcode = lv_textarea_get_text(ta_barcode);
  if (strlen(barcode) == 0)
    return;
  ItemView staged = getCurrentEditItem();
  if (fetchModeMetadata(barcode, staged)) {
    updateCurrentEditItem(staged);
    lv_textarea_set_text(ta_title, staged.title.c_str());
    lv_textarea_set_text(ta_artist, staged.artistOrAuthor.c_str());
    lv_textarea_set_text(ta_genre, staged.genre.c_str());
    lv_textarea_set_text(ta_year, String(staged.year).c_str());

    // Update uniqueID field to show the actual ID (usually the barcode)
    if (ta_uniqueID && staged.uniqueID.length() > 0) {
      lv_textarea_set_text(ta_uniqueID, staged.uniqueID.c_str());
    }

    if (!staged.ledIndices.empty()) {
      String ledStr = "";
      for (size_t i = 0; i < staged.ledIndices.size(); i++) {
        ledStr += String(staged.ledIndices[i]);
        if (i < staged.ledIndices.size() - 1)
          ledStr += ",";
      }
      lv_textarea_set_text(ta_led_index, ledStr.c_str());
    }
  } else {
    static const char *btns[] = {"Yes", "No", ""};
    lv_obj_t *mbox =
        lv_msgbox_create(NULL, "Not Found", "Manual entry?", btns, true);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, manual_entry_confirm_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
  }
}

void close_add_item_ui() { // Renamed from close_add_item_ui
  if (add_item_panel) {    // Renamed from add_item_panel
    lvgl_port_lock(-1);
    lv_obj_del(add_item_panel); // Renamed from add_item_panel
    add_item_panel = NULL;      // Renamed from add_item_panel
    // Reset pointers
    ta_barcode = ta_title = ta_artist = ta_genre = ta_year = NULL;
    ta_led_index = ta_uniqueID = ta_notes = NULL;
    ta_publisher = ta_page_count = ta_current_page = NULL;
    lvgl_port_unlock();
  }
}

void perform_save_item() {
  Serial.println(">> perform_save_item() ENTERED");
  // Get values from form
  const char *title = lv_textarea_get_text(ta_title);

  const char *artist = lv_textarea_get_text(ta_artist);
  const char *genre = lv_textarea_get_text(ta_genre);
  const char *year_str = lv_textarea_get_text(ta_year);
  const char *barcode = lv_textarea_get_text(ta_barcode);
  const char *led_str = lv_textarea_get_text(ta_led_index);
  const char *notes = ta_notes ? lv_textarea_get_text(ta_notes) : "";

  // Validate required fields
  if (strlen(title) == 0 || strlen(artist) == 0) {
    Serial.println("Error: Title and Artist are required!");
    return;
  }

  // Parse LED Indices (Shared Logic)
  std::vector<int> parsedLeds;
  String lStr = String(led_str);

  // Normalize delimiters: Replace spaces with commas to handle "1 2", "1, 2",
  // or "1,2"
  lStr.replace(" ", ",");

  while (lStr.length() > 0) {
    int commaIndex = lStr.indexOf(',');
    if (commaIndex == -1) {
      lStr.trim();
      if (lStr.length() > 0)
        parsedLeds.push_back(lStr.toInt());
      break;
    } else {
      String part = lStr.substring(0, commaIndex);
      part.trim();
      if (part.length() > 0)
        parsedLeds.push_back(part.toInt());
      lStr = lStr.substring(commaIndex + 1);
    }
  }

  // Use Abstraction Layer to manage staging
  ItemView staged = getCurrentEditItem();

  // Preserve fields that aren't in the form
  String preservedCoverFile = staged.coverFile;
  String preservedCoverUrl = staged.coverUrl;
  String preservedUniqueID = staged.uniqueID;
  bool preservedFavorite = staged.favorite;
  String preservedExtraInfo = staged.extraInfo;

  // Update fields from form
  staged.title = String(title);
  staged.artistOrAuthor = String(artist);
  staged.genre = strlen(genre) > 0 ? String(genre) : "Unknown";
  staged.year = strlen(year_str) > 0 ? atoi(year_str) : 0;
  staged.ledIndices = parsedLeds;
  staged.codecOrIsbn = String(barcode);
  staged.notes = String(notes);

  switch (currentMode) {
  case MODE_BOOK:
    if (ta_publisher)
      staged.publisher = String(lv_textarea_get_text(ta_publisher));
    if (ta_page_count)
      staged.pageCount = atoi(lv_textarea_get_text(ta_page_count));
    if (ta_current_page)
      staged.currentPage = atoi(lv_textarea_get_text(ta_current_page));
    break;
  default:
    break;
  }

  // Restore preserved fields (except UniqueID if valid in UI)
  staged.coverFile = preservedCoverFile;
  staged.coverUrl = preservedCoverUrl;
  staged.favorite = preservedFavorite;
  staged.extraInfo = preservedExtraInfo;

  // FIX: Properly save Unique ID from UI if changed
  if (ta_uniqueID) {
    String uiUid = String(lv_textarea_get_text(ta_uniqueID));
    uiUid.trim();
    if (uiUid.length() > 0) {
      staged.uniqueID = uiUid;
    } else {
      staged.uniqueID = preservedUniqueID;
    }
  } else {
    staged.uniqueID = preservedUniqueID;
  }

  // --- DEBUG LOGGING ---
  Serial.println("\n========== SAVING ITEM ==========");
  Serial.printf("Unique ID: '%s'\n", staged.uniqueID.c_str());
  Serial.printf("Title:     '%s'\n", staged.title.c_str());
  Serial.printf("Artist:    '%s'\n", staged.artistOrAuthor.c_str());
  Serial.printf("LEDs:      (Count: %d)\n", staged.ledIndices.size());
  for (int l : staged.ledIndices)
    Serial.printf("  - %d\n", l);
  Serial.println("=================================\n");

  if (edit_item_index >= 0 && edit_item_index < getItemCount()) {
    // === EDIT MODE ===
    setItem(edit_item_index, staged);
    updateCurrentEditItem(staged); // Sync staging struct

    if (saveCurrentEditItem(preservedUniqueID.c_str())) {
      Serial.printf("✅ Saved %s: %s\n", getModeName().c_str(),
                    staged.title.c_str());
      setCurrentItemIndex(edit_item_index);
      update_item_display();
      close_add_item_ui();
    } else {
      Serial.println("❌ Failed to save item to SD!");
    }
  } else {
    // === ADD MODE ===
    if (staged.uniqueID.length() == 0) {
      if (staged.codecOrIsbn.length() > 0)
        staged.uniqueID = staged.codecOrIsbn;
      else
        staged.uniqueID = String(millis()) + "_" + String(random(9999));
    }

    if (staged.ledIndices.empty()) {
      int baseLed = getSettingLedStart();
      staged.ledIndices.push_back(baseLed + getItemCount());
    }

    if (staged.coverFile.length() == 0) {
      String prefix = getUidPrefix();
      staged.coverFile = prefix + staged.uniqueID + ".jpg";
    }

    addItemToLibrary(staged);
    updateCurrentEditItem(staged); // Sync staging struct

    if (saveCurrentEditItem()) {
      Serial.printf("âœ… Added %s: %s\n", getModeName().c_str(),
                    staged.title.c_str());
      setCurrentItemIndex(getItemCount() - 1);
      update_item_display();
      close_add_item_ui(); // Renamed from close_add_item_ui
    } else {
      Serial.println("âŒ Failed to save item!");
    }
  }
}

void save_new_item_cb(lv_event_t *e) {
  Serial.println(">> SAVE BUTTON CLICKED");
  // 1. Basic validation
  const char *title = lv_textarea_get_text(ta_title);
  if (strlen(title) == 0) {
    perform_save_item(); // Let original function handle validation
    return;
  }

  // 2. Duplicate Check
  const char *barcode = lv_textarea_get_text(ta_barcode);
  if (edit_item_index == -1 &&
      strlen(barcode) > 0) { // Renamed from edit_item_index
    int count = getItemCount();
    String targetID =
        String(barcode); // Use barcode as targetID for duplicate check
    // Linear search for ID (RAM only for speed)
    for (int i = 0; i < count; i++) {
      if (getItemAtRAM(i).uniqueID == targetID) {
        ItemView item = getItemAt(i); // Fetch full item for display
        // Show Alert
        lvgl_port_lock(-1);
        lv_obj_t *mbox = lv_msgbox_create(
            NULL, "Duplicate",
            ("Barcode exists:\n" + item.title + "\nAdd anyway?").c_str(), NULL,
            true);
        lv_obj_center(mbox);
        lv_obj_set_style_bg_color(mbox, lv_color_hex(0x222222), 0);
        lv_obj_set_style_text_color(mbox, lv_color_hex(0xffffff), 0);

        // YES
        lv_obj_t *btn_yes = lv_btn_create(mbox);
        lv_obj_set_size(btn_yes, 80, 40);
        lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 30, -20);
        lv_obj_set_style_bg_color(btn_yes, lv_color_hex(getCurrentThemeColor()),
                                  0);
        lv_obj_add_event_cb(
            btn_yes,
            [](lv_event_t *e) {
              lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
              perform_save_item();
            },
            LV_EVENT_CLICKED, mbox);
        lv_obj_t *l_y = lv_label_create(btn_yes);
        lv_label_set_text(l_y, "YES");
        lv_obj_center(l_y);
        lv_obj_set_style_text_color(l_y, lv_color_hex(0x000000), 0);

        // NO
        lv_obj_t *btn_no = lv_btn_create(mbox);
        lv_obj_set_size(btn_no, 80, 40);
        lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -30, -20);
        lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x555555), 0);
        lv_obj_add_event_cb(
            btn_no,
            [](lv_event_t *e) {
              lv_msgbox_close((lv_obj_t *)lv_event_get_user_data(e));
            },
            LV_EVENT_CLICKED, mbox);
        lv_obj_t *l_n = lv_label_create(btn_no);
        lv_label_set_text(l_n, "NO");
        lv_obj_center(l_n);
        lv_obj_set_style_text_color(l_n, lv_color_hex(0xffffff), 0);

        lvgl_port_unlock();
        return;
      }
    }
  }

  perform_save_item();
}

void show_add_item_ui() {
  if (add_item_panel)
    return; // Already open

  // Reset global edit tracking objects ONLY if adding new item
  if (edit_item_index == -1) {
    currentEditCD = CD();
    currentEditBook = Book();
  }

  // Calculate default LED index using the same logic as the core system
  int nextLed = getNextLedIndex();

  // Guide User: Light up next available slot (if adding)
  if (edit_item_index == -1) {
    if (led_master_on && nextLed >= 0 && nextLed < led_count) {
      FastLED.clear();
      leds[nextLed] = CRGB::White; // Show white guide light
      FastLED.show();
    }
  }

  lvgl_port_lock(-1);

  add_item_panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(add_item_panel, 700, 450);
  lv_obj_center(add_item_panel);
  lv_obj_add_style(add_item_panel, &style_modal_panel, 0); // Apply Modal Style
  lv_obj_set_style_bg_color(add_item_panel, lv_color_hex(0x0d0d0d),
                            0); // Slightly darker
  lv_obj_set_scroll_dir(add_item_panel,
                        LV_DIR_VER); // Enable vertical scrolling
  lv_obj_set_scrollbar_mode(add_item_panel,
                            LV_SCROLLBAR_MODE_ON);   // Always show scrollbar
  lv_obj_set_style_pad_right(add_item_panel, 12, 0); // Make room for scrollbar
  // Make scrollbar more visible
  lv_obj_set_style_bg_color(
      add_item_panel, lv_color_hex(getCurrentThemeColor()), LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(add_item_panel, LV_OPA_70, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(add_item_panel, 8,
                         LV_PART_SCROLLBAR); // Wider scrollbar

  // Title
  lv_obj_t *title = lv_label_create(add_item_panel);
  String action = (edit_item_index >= 0) ? "EDIT" : "ADD NEW";
  String titleText = " " + action + " " + getModeName();
  lv_label_set_text(title, (LV_SYMBOL_PLUS + titleText).c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
  lv_obj_add_style(title, &style_text_header, 0); // Apply Global Header Style

  // Hide/Show Keyboard Toggle (before close button)
  lv_obj_t *btn_toggle_kb_add = lv_btn_create(add_item_panel);
  lv_obj_set_size(btn_toggle_kb_add, 100, 40);
  lv_obj_align(btn_toggle_kb_add, LV_ALIGN_TOP_RIGHT, -70, 10);
  lv_obj_set_style_bg_color(btn_toggle_kb_add, lv_color_hex(0x444444), 0);
  lv_obj_t *label_toggle_kb_add = lv_label_create(btn_toggle_kb_add);
  lv_label_set_text(label_toggle_kb_add, LV_SYMBOL_KEYBOARD " SHOW");
  lv_obj_center(label_toggle_kb_add);
  lv_obj_set_style_text_color(label_toggle_kb_add, lv_color_hex(0xffffff), 0);

  // Close button
  lv_obj_t *btn_close = lv_btn_create(add_item_panel);
  lv_obj_set_size(btn_close, 50, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btn_close, &style_btn_close, 0); // Apply Global Style
  lv_obj_add_event_cb(
      btn_close, [](lv_event_t *e) { close_add_item_ui(); }, LV_EVENT_CLICKED,
      NULL);
  lv_obj_t *label_close = lv_label_create(btn_close);
  lv_label_set_text(label_close, LV_SYMBOL_CLOSE);
  lv_obj_center(label_close);
  lv_obj_set_style_text_color(label_close, lv_color_hex(0xff4444), 0);

  // Save button (top left)
  lv_obj_t *btn_save = lv_btn_create(add_item_panel);
  lv_obj_set_size(btn_save, 120, 40);
  lv_obj_align(btn_save, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_set_style_bg_color(btn_save, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_event_cb(btn_save, save_new_item_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label_save = lv_label_create(btn_save);
  String saveText = " SAVE " + getModeName();
  lv_label_set_text(label_save, (LV_SYMBOL_SAVE + saveText).c_str());
  lv_obj_center(label_save);
  lv_obj_set_style_text_color(label_save, lv_color_hex(0x000000), 0);

  int y_offset = 85;
  int field_height = 50;

  // Unique ID field (Read Only)
  lv_obj_t *label_uid = lv_label_create(add_item_panel);
  lv_label_set_text(label_uid, "Unique ID:");
  lv_obj_align(label_uid, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_uid, lv_color_hex(0xaaaaaa), 0);

  ta_uniqueID = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_uniqueID, 400, 40);
  lv_obj_align(ta_uniqueID, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_uniqueID, true);
  lv_textarea_set_text(ta_uniqueID, "(Auto-generated)");
  lv_obj_set_style_bg_color(ta_uniqueID, lv_color_hex(0x111111), 0);
  lv_obj_set_style_text_color(ta_uniqueID, lv_color_hex(0x888888), 0);
  lv_obj_set_style_border_width(ta_uniqueID, 0, 0);
  lv_obj_clear_flag(ta_uniqueID, LV_OBJ_FLAG_CLICKABLE); // Read-only

  y_offset += field_height;

  // Barcode field
  lv_obj_t *label_barcode = lv_label_create(add_item_panel);
  lv_label_set_text(label_barcode, getCodeLabel().c_str());
  lv_obj_align(label_barcode, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_barcode, lv_color_hex(0xaaaaaa), 0);

  ta_barcode = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_barcode, 400, 40);
  lv_obj_align(ta_barcode, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_barcode, true);
  lv_textarea_set_placeholder_text(ta_barcode, "Enter code...");
  lv_obj_set_style_bg_color(ta_barcode, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_barcode, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_barcode,
                                lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_style(ta_barcode, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  // Fetch button
  lv_obj_t *btn_fetch = lv_btn_create(add_item_panel);
  lv_obj_set_size(btn_fetch, 100, 40);
  lv_obj_align(btn_fetch, LV_ALIGN_TOP_LEFT, 530, y_offset - 5);
  lv_obj_set_style_bg_color(btn_fetch, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_event_cb(btn_fetch, fetch_barcode_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *label_fetch = lv_label_create(btn_fetch);
  lv_label_set_text(label_fetch, "FETCH");
  lv_obj_center(label_fetch);
  lv_obj_set_style_text_color(label_fetch, lv_color_hex(0x000000), 0);

  y_offset += field_height;

  // Title field
  lv_obj_t *label_title = lv_label_create(add_item_panel);
  lv_label_set_text(label_title, "Title:");
  lv_obj_align(label_title, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_title, lv_color_hex(0xaaaaaa), 0);

  ta_title = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_title, 540, 40);
  lv_obj_align(ta_title, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_title, true);
  lv_obj_set_style_bg_color(ta_title, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_title, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_title, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_title, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  y_offset += field_height;

  // Artist field
  lv_obj_t *label_artist = lv_label_create(add_item_panel);
  lv_label_set_text(label_artist, getArtistLabel().c_str());
  lv_obj_align(label_artist, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_artist, lv_color_hex(0xaaaaaa), 0);

  ta_artist = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_artist, 540, 40);
  lv_obj_align(ta_artist, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_artist, true);
  lv_obj_set_style_bg_color(ta_artist, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_artist, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_artist, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_artist, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  y_offset += field_height;

  // Genre, Year, LED Index (3 columns)
  lv_obj_t *label_genre = lv_label_create(add_item_panel);
  lv_label_set_text(label_genre, "Genre:");
  lv_obj_align(label_genre, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_genre, lv_color_hex(0xaaaaaa), 0);

  ta_genre = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_genre, 150, 40);
  lv_obj_align(ta_genre, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_genre, true);
  lv_obj_set_style_bg_color(ta_genre, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_genre, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_genre, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_genre, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  lv_obj_t *label_year = lv_label_create(add_item_panel);
  lv_label_set_text(label_year, "Year:");
  lv_obj_align(label_year, LV_ALIGN_TOP_LEFT, 290, y_offset);
  lv_obj_set_style_text_color(label_year, lv_color_hex(0xaaaaaa), 0);

  ta_year = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_year, 100, 40);
  lv_obj_align(ta_year, LV_ALIGN_TOP_LEFT, 350, y_offset - 5);
  lv_textarea_set_one_line(ta_year, true);
  lv_textarea_set_accepted_chars(ta_year, "0123456789");
  lv_obj_set_style_bg_color(ta_year, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_year, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_year, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_year, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  lv_obj_t *label_led = lv_label_create(add_item_panel);
  lv_label_set_text(label_led, "LED#:");
  lv_obj_align(label_led, LV_ALIGN_TOP_LEFT, 470, y_offset);
  lv_obj_set_style_text_color(label_led, lv_color_hex(0xffffff), 0);

  ta_led_index = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_led_index, 100, 40);
  lv_obj_align(ta_led_index, LV_ALIGN_TOP_LEFT, 530, y_offset - 5);
  lv_textarea_set_one_line(ta_led_index, true);
  lv_textarea_set_accepted_chars(ta_led_index,
                                 "0123456789, "); // Allow CSV and Spaces
  lv_textarea_set_text(ta_led_index, "");

  if (edit_item_index == -1 && nextLed < led_count) {
    lv_textarea_set_text(ta_led_index, String(nextLed).c_str());
  }
  lv_obj_set_style_bg_color(ta_led_index, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_led_index, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_led_index,
                                lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_style(ta_led_index, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  // Real-time LED feedback
  lv_obj_add_event_cb(
      ta_led_index,
      [](lv_event_t *e) {
        if (led_master_on) {
          const char *txt = lv_textarea_get_text(lv_event_get_target(e));
          String ledStr = String(txt);

          FastLED.clear();

          while (ledStr.length() > 0) {
            int comma = ledStr.indexOf(',');
            int ledNum = -1;

            if (comma == -1) {
              if (ledStr.length() > 0)
                ledNum = ledStr.toInt();
              ledStr = "";
            } else {
              ledNum = ledStr.substring(0, comma).toInt();
              ledStr = ledStr.substring(comma + 1);
            }

            if (ledNum >= 0 && ledNum < led_count) {
              leds[ledNum] = COLOR_TEMPORARY;
            }
          }

          FastLED.show();
        }
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // Notes field
  y_offset += field_height;
  lv_obj_t *label_notes = lv_label_create(add_item_panel);
  lv_label_set_text(label_notes, "Notes:");
  lv_obj_align(label_notes, LV_ALIGN_TOP_LEFT, 20, y_offset);
  lv_obj_set_style_text_color(label_notes, lv_color_hex(0xaaaaaa), 0);

  ta_notes = lv_textarea_create(add_item_panel);
  lv_obj_set_size(ta_notes, 540, 40);
  lv_obj_align(ta_notes, LV_ALIGN_TOP_LEFT, 120, y_offset - 5);
  lv_textarea_set_one_line(ta_notes, true);
  lv_obj_set_style_bg_color(ta_notes, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_text_color(ta_notes, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_color(ta_notes, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_add_style(ta_notes, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);

  // Keyboard (shared for all text areas) - positioned after last field
  y_offset += field_height + 10;
  lv_obj_t *kb_add = lv_keyboard_create(add_item_panel);
  lv_obj_set_size(kb_add, 680, 160);
  lv_obj_align(kb_add, LV_ALIGN_TOP_MID, 0, y_offset);
  lv_obj_set_style_bg_color(kb_add, lv_color_hex(0x1a1a1a), 0);
  lv_obj_add_flag(kb_add, LV_OBJ_FLAG_HIDDEN); // Hidden by default

  // Event callback to show keyboard when text area is focused
  auto ta_focus_cb = [](lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
  };

  // Attach keyboard to all text areas
  lv_obj_add_event_cb(ta_barcode, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_title, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_artist, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_genre, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_year, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_led_index, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);
  lv_obj_add_event_cb(ta_notes, ta_focus_cb, LV_EVENT_FOCUSED, kb_add);

  // Add toggle button callback
  static lv_obj_t *toggle_data_add[2];
  toggle_data_add[0] = kb_add;
  toggle_data_add[1] = label_toggle_kb_add;
  lv_obj_add_event_cb(
      btn_toggle_kb_add,
      [](lv_event_t *e) {
        lv_obj_t **data = (lv_obj_t **)lv_event_get_user_data(e);
        if (!data)
          return;
        lv_obj_t *kb = data[0];
        lv_obj_t *label = data[1];

        if (lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " HIDE");
        } else {
          lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(label, LV_SYMBOL_KEYBOARD " SHOW");
        }
      },
      LV_EVENT_CLICKED, toggle_data_add);

  // Add spacer at bottom
  lv_obj_t *spacer = lv_obj_create(add_item_panel);
  lv_obj_set_size(spacer, 10, 450);
  lv_obj_align(spacer, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);

  lvgl_port_unlock();
}

void apply_filters() {
  filter_active = true;
  update_filtered_leds();

  int match_count = 0;
  int total = getItemCount();
  for (int i = 0; i < total; i++) {
    if (is_item_match(i)) {
      match_count++;
    }
  }

  String status_text = LV_SYMBOL_DIRECTORY " Filtered: " + String(match_count) +
                       " of " + String(getItemCount()) + " " +
                       getModeNamePlural();
  if (filter_genre.length() > 0)
    status_text += " | " + filter_genre;
  if (filter_decade > 0) {
    if (filter_decade >= 100)
      status_text += " | " + String(1900 + filter_decade) + "s";
    else
      status_text += " | " + String(filter_decade) + "s";
  }
  if (filter_favorites_only)
    status_text += " | " LV_SYMBOL_BELL " Favorites";

  lvgl_port_lock(-1);
  if (label_filter_status) {
    lv_label_set_text(label_filter_status, status_text.c_str());
    lv_obj_clear_flag(label_filter_status, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_port_unlock();

  // Reset index to first match
  for (int i = 0; i < total; i++) {
    if (is_item_match(i)) {
      setCurrentItemIndex(i);
      break;
    }
  }

  update_item_display();
  close_filter_ui();
}

void clear_filters() {
  filter_active = false;
  filter_genre = "";
  filter_decade = 0;
  filter_favorites_only = false;

  lvgl_port_lock(-1);
  if (label_filter_status)
    lv_obj_add_flag(label_filter_status, LV_OBJ_FLAG_HIDDEN);
  lvgl_port_unlock();

  update_item_display();
  close_filter_ui();
}

void close_filter_ui() {
  if (filter_panel) {
    lvgl_port_lock(-1);
    lv_obj_del(filter_panel);
    filter_panel = NULL;
    dd_genre_filter = NULL;
    dd_decade_filter = NULL;
    cb_fav_filter = NULL;
    lvgl_port_unlock();
  }
}

void show_filter_ui() {
  if (filter_panel)
    return;

  lvgl_port_lock(-1);

  filter_panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(filter_panel, 600, 420);
  lv_obj_center(filter_panel);
  lv_obj_set_style_bg_color(filter_panel, lv_color_hex(0x0d0d0d), 0);
  lv_obj_set_style_border_color(filter_panel, lv_color_hex(0x00aaff), 0);
  lv_obj_set_style_border_width(filter_panel, 3, 0);
  lv_obj_set_style_radius(filter_panel, 10, 0);

  lv_obj_t *title = lv_label_create(filter_panel);
  String filterTitle = " FILTER " + getModeNamePlural();
  lv_label_set_text(title, (LV_SYMBOL_SETTINGS + filterTitle).c_str());
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
  lv_obj_set_style_text_color(title, lv_color_hex(0x00aaff), 0);

  lv_obj_t *btn_close = lv_btn_create(filter_panel);
  lv_obj_set_size(btn_close, 50, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_style(btn_close, &style_btn_close, 0);
  lv_obj_add_event_cb(
      btn_close, [](lv_event_t *e) { close_filter_ui(); }, LV_EVENT_CLICKED,
      NULL);
  lv_obj_t *l_c = lv_label_create(btn_close);
  lv_label_set_text(l_c, LV_SYMBOL_CLOSE);
  lv_obj_center(l_c);

  int y_offset = 70;

  // Genre
  lv_obj_t *label_genre = lv_label_create(filter_panel);
  lv_label_set_text(label_genre, "Genre:");
  lv_obj_align(label_genre, LV_ALIGN_TOP_LEFT, 30, y_offset);
  lv_obj_set_style_text_color(label_genre, lv_color_hex(0xaaaaaa), 0);

  dd_genre_filter = lv_dropdown_create(filter_panel);
  lv_obj_set_size(dd_genre_filter, 500, 40);
  lv_obj_align(dd_genre_filter, LV_ALIGN_TOP_LEFT, 30, y_offset + 25);

  String genres = "All";
  std::vector<String> unique_genres;
  int totalCount = getItemCount();
  for (int i = 0; i < totalCount; i++) {
    ItemView iv = getItemAtRAM(i);
    bool found = false;
    for (const auto &g : unique_genres) {
      if (g.equalsIgnoreCase(iv.genre)) {
        found = true;
        break;
      }
    }
    if (!found && iv.genre.length() > 0) {
      unique_genres.push_back(iv.genre);
      genres += "\n" + iv.genre;
    }
  }
  lv_dropdown_set_options(dd_genre_filter, genres.c_str());

  y_offset += 90;

  // Decade
  lv_obj_t *label_decade = lv_label_create(filter_panel);
  lv_label_set_text(label_decade, "Decade:");
  lv_obj_align(label_decade, LV_ALIGN_TOP_LEFT, 30, y_offset);
  lv_obj_set_style_text_color(label_decade, lv_color_hex(0xaaaaaa), 0);

  dd_decade_filter = lv_dropdown_create(filter_panel);
  lv_obj_set_size(dd_decade_filter, 500, 40);
  lv_obj_align(dd_decade_filter, LV_ALIGN_TOP_LEFT, 30, y_offset + 25);
  lv_dropdown_set_options(dd_decade_filter,
                          "All\n60s\n70s\n80s\n90s\n2000s\n2010s\n2020s");

  y_offset += 90;

  // Favorites
  cb_fav_filter = lv_checkbox_create(filter_panel);
  lv_checkbox_set_text(cb_fav_filter, "Favorites Only");
  lv_obj_align(cb_fav_filter, LV_ALIGN_TOP_LEFT, 30, y_offset);
  lv_obj_set_style_text_color(cb_fav_filter, lv_color_hex(0xffffff), 0);

  y_offset += 60;

  // APPLY
  lv_obj_t *btn_apply = lv_btn_create(filter_panel);
  lv_obj_set_size(btn_apply, 200, 50);
  lv_obj_align(btn_apply, LV_ALIGN_BOTTOM_LEFT, 50, -20);
  lv_obj_set_style_bg_color(btn_apply, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_add_event_cb(
      btn_apply,
      [](lv_event_t *e) {
        // Read filters
        uint16_t sel_genre = lv_dropdown_get_selected(dd_genre_filter);
        if (sel_genre == 0)
          filter_genre = "";
        else {
          char buf[64];
          lv_dropdown_get_selected_str(dd_genre_filter, buf, sizeof(buf));
          filter_genre = String(buf);
        }

        uint16_t sel_decade = lv_dropdown_get_selected(dd_decade_filter);
        const int decade_map[] = {0, 60, 70, 80, 90, 0, 10, 20};
        filter_decade = (sel_decade < 8) ? decade_map[sel_decade] : 0;

        filter_favorites_only =
            lv_obj_has_state(cb_fav_filter, LV_STATE_CHECKED);
        apply_filters();
      },
      LV_EVENT_CLICKED, NULL);
  lv_obj_t *label_apply = lv_label_create(btn_apply);
  lv_label_set_text(label_apply, LV_SYMBOL_OK " APPLY");
  lv_obj_center(label_apply);
  lv_obj_set_style_text_color(label_apply, lv_color_hex(0x000000), 0);

  // CLEAR
  lv_obj_t *btn_clear = lv_btn_create(filter_panel);
  lv_obj_set_size(btn_clear, 200, 50);
  lv_obj_align(btn_clear, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
  lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0xff8800), 0);
  lv_obj_add_event_cb(
      btn_clear, [](lv_event_t *e) { clear_filters(); }, LV_EVENT_CLICKED,
      NULL);
  lv_obj_t *label_clear = lv_label_create(btn_clear);
  lv_label_set_text(label_clear, LV_SYMBOL_CLOSE " CLEAR");
  lv_obj_center(label_clear);
  lv_obj_set_style_text_color(label_clear, lv_color_hex(0x000000), 0);

  lvgl_port_unlock();
}

void show_led_selector_ui(lv_obj_t *target_ta) {
  if (!target_ta) {
    Serial.println("Error: target_ta is NULL in show_led_selector_ui");
    return;
  }
  lvgl_port_lock(-1);

  Serial.println("Opening LED Selector...");

  // Create Modal
  lv_obj_t *panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(panel, 750, 460);
  lv_obj_center(panel);
  lv_obj_add_style(panel, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x111111), 0);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "Select LED Indicators");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 10);
  lv_obj_add_style(title, &style_text_header, 0);

  // Parse existing
  const char *txt = lv_textarea_get_text(target_ta);
  std::vector<int> *selected = new std::vector<int>();
  String s = String(txt);
  while (s.length() > 0) {
    int comma = s.indexOf(',');
    if (comma == -1) {
      if (s.length() > 0)
        selected->push_back(s.toInt());
      break;
    }
    selected->push_back(s.substring(0, comma).toInt());
    s = s.substring(comma + 1);
  }

  // Create Data Struct for Pagination
  SelectorData *data = new SelectorData{target_ta, selected, 0, NULL, NULL};

  // Grid Container
  data->grid = lv_obj_create(panel);
  lv_obj_set_size(data->grid, 700, 320);
  lv_obj_align(data->grid, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_bg_color(data->grid, lv_color_hex(0x000000), 0);

  // Paging Controls
  lv_obj_t *btn_prev_pg = lv_btn_create(panel);
  lv_obj_set_size(btn_prev_pg, 80, 40);
  lv_obj_align(btn_prev_pg, LV_ALIGN_BOTTOM_LEFT, 20, -10);
  lv_label_set_text(lv_label_create(btn_prev_pg), LV_SYMBOL_LEFT " PREV");

  lv_obj_t *btn_next_pg = lv_btn_create(panel);
  lv_obj_set_size(btn_next_pg, 80, 40);
  lv_obj_align(btn_next_pg, LV_ALIGN_BOTTOM_RIGHT, -20, -10);
  lv_label_set_text(lv_label_create(btn_next_pg), "NEXT " LV_SYMBOL_RIGHT);

  data->lbl_page = lv_label_create(panel);
  lv_obj_align(data->lbl_page, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_label_set_text(data->lbl_page, "Page 1");

  // Define update function lambda
  auto update_grid = [](SelectorData *d) {
    lv_obj_clean(d->grid);
    int total_leds = 600; // led_count global?
    int per_page = 50;
    int start = d->page * per_page;
    int end = start + per_page;
    if (end > total_leds)
      end = total_leds;

    // We can't use 0 as width in flex container if we want wrapped grid?
    // Using Flex Layout for grid items
    lv_obj_set_flex_flow(d->grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(d->grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    for (int i = start; i < end; i++) {
      lv_obj_t *btn = lv_btn_create(d->grid);
      lv_obj_set_size(btn, 60, 40);
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);

      // Check if selected
      for (int sel : *(d->vec)) {
        if (sel == i) {
          lv_obj_set_style_bg_color(btn, lv_color_hex(0x00ff00), 0);
          break;
        }
      }

      lv_obj_t *l = lv_label_create(btn);
      lv_label_set_text_fmt(l, "%d", i);
      lv_obj_center(l);

      // Toggle Callback
      lv_obj_set_user_data(btn, (void *)(intptr_t)i);
      lv_obj_add_event_cb(
          btn,
          [](lv_event_t *e) {
            lv_obj_t *b = lv_event_get_target(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(b);
            // We need access to 'd'. How to pass 'd' to this inner callback?
            // Easier way: Store 'd' in the grid user data or something.
            // But 'd' is passed to update_grid.
            // Let's attach 'd' to the button too?
            // Actually, we can just use the panel's user data if we store 'd'
            // there.
            // Wait, this lambda doesn't capture 'd'.
            // Let's attach 'd' to button user data is tricky if we store index.
            // Let's rely on grandparent?
            // HACK: Re-traverse up?
            // Better: Pass a struct { index, d }?
            // Simplest: Check color.
            lv_color_t c = lv_obj_get_style_bg_color(b, 0);
            if (c.full == lv_color_hex(0x00ff00).full) {
              // Deselect
              lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
              // Remove from vector - HARD without 'd' pointer.
              // FIX: Attach 'd' to the grid object, and get it from parent.
            } else {
              // Select
              lv_obj_set_style_bg_color(b, lv_color_hex(0x00ff00), 0);
            }
            // We really need to update the vector.
            // Let's try to get 'd' from grid.
            lv_obj_t *grid = lv_obj_get_parent(b);
            SelectorData *dataPtr = (SelectorData *)lv_obj_get_user_data(grid);
            if (dataPtr) {
              bool found = false;
              for (auto it = dataPtr->vec->begin(); it != dataPtr->vec->end();
                   ++it) {
                if (*it == idx) {
                  dataPtr->vec->erase(it);
                  found = true;
                  break;
                }
              }
              if (!found) {
                dataPtr->vec->push_back(idx);
              }
              // Update TA immediately? Or on close?
              // Update TA immediately to see results?
              // No, do it on "DONE" or close.
              // Logic says: The vector is live.
            }
          },
          LV_EVENT_CLICKED, NULL);
    }

    lv_label_set_text_fmt(d->lbl_page, "Page %d", d->page + 1);
  };

  // Store data pointer in grid for button callbacks
  lv_obj_set_user_data(data->grid, data);

  // Initial Update
  update_grid(data);

  // Prev/Next Callbacks
  lv_obj_set_user_data(btn_prev_pg, data);
  lv_obj_add_event_cb(
      btn_prev_pg,
      [](lv_event_t *e) {
        SelectorData *d =
            (SelectorData *)lv_obj_get_user_data(lv_event_get_target(e));
        if (d->page > 0) {
          d->page--;
          // We need to call update_grid(d). Since update_grid is a lambda
          // variable it's not available here easily unless captured or global
          // function. FIX: Make update_grid a real function or copy logic.
          // Copying logic for now (duplicate code alert, but safe).
          // Actually, let's just trigger a "refresh" event on the grid?
          // Or make a helper function in implementation file.
          // Re-implementing simplified update logic for navigation:
          lv_event_send(d->grid, LV_EVENT_REFRESH, NULL);
        }
      },
      LV_EVENT_CLICKED, NULL);

  lv_obj_set_user_data(btn_next_pg, data);
  lv_obj_add_event_cb(
      btn_next_pg,
      [](lv_event_t *e) {
        SelectorData *d =
            (SelectorData *)lv_obj_get_user_data(lv_event_get_target(e));
        d->page++;
        // Boundary check?
        lv_event_send(d->grid, LV_EVENT_REFRESH, NULL);
      },
      LV_EVENT_CLICKED, NULL);

  // "Refresh" event handler for grid to redraw
  lv_obj_add_event_cb(
      data->grid,
      [](lv_event_t *e) {
        SelectorData *d =
            (SelectorData *)lv_obj_get_user_data(lv_event_get_target(e));
        // Paste update logic here or call helper...
        // Since we can't easily share the lambda, we will just put the logic
        // inside this callback! And the initial call will manually trigger it.

        lv_obj_t *grid = d->grid;
        lv_obj_clean(grid);
        int total_leds = led_count; // Global
        int per_page = 40; // Fit in 700x320? 60x40 buttons. 10 cols? 10*60=600.
        int start = d->page * per_page;
        int end = start + per_page;
        if (end > total_leds)
          end = total_leds;

        for (int i = start; i < end; i++) {
          lv_obj_t *btn = lv_btn_create(grid);
          lv_obj_set_size(btn, 60, 40);
          bool isSel = false;
          for (int s : *(d->vec))
            if (s == i)
              isSel = true;

          lv_obj_set_style_bg_color(
              btn, lv_color_hex(isSel ? 0x00aa00 : 0x333333), 0);
          lv_label_set_text_fmt(lv_label_create(btn), "%d", i);

          // Button Click
          lv_obj_set_user_data(btn, (void *)(intptr_t)i);
          lv_obj_add_event_cb(
              btn,
              [](lv_event_t *e) {
                lv_obj_t *b = lv_event_get_target(e);
                int idx = (int)(intptr_t)lv_obj_get_user_data(b);
                lv_obj_t *g = lv_obj_get_parent(b);
                SelectorData *sd = (SelectorData *)lv_obj_get_user_data(g);

                bool found = false;
                for (auto it = sd->vec->begin(); it != sd->vec->end(); ++it) {
                  if (*it == idx) {
                    sd->vec->erase(it);
                    found = true;
                    break;
                  }
                }
                if (!found)
                  sd->vec->push_back(idx);

                // Visual toggle
                lv_obj_set_style_bg_color(
                    b, lv_color_hex(!found ? 0x00aa00 : 0x333333), 0);
              },
              LV_EVENT_CLICKED, NULL);
        }
        lv_label_set_text_fmt(d->lbl_page, "Page %d", d->page + 1);
      },
      LV_EVENT_REFRESH, NULL);

  // Trigger initial draw
  lv_event_send(data->grid, LV_EVENT_REFRESH, NULL);

  // DONE Button
  lv_obj_t *btn_done = lv_btn_create(panel);
  lv_obj_set_size(btn_done, 100, 50);
  lv_obj_align(btn_done, LV_ALIGN_TOP_RIGHT, -20, 10);
  lv_obj_set_style_bg_color(btn_done, lv_color_hex(0x0088ff), 0);
  lv_label_set_text(lv_label_create(btn_done), LV_SYMBOL_OK " DONE");
  lv_obj_set_user_data(btn_done, data);
  lv_obj_add_event_cb(
      btn_done,
      [](lv_event_t *e) {
        SelectorData *d =
            (SelectorData *)lv_obj_get_user_data(lv_event_get_target(e));

        // Serialize vector to CSV
        String csv = "";
        for (size_t i = 0; i < d->vec->size(); i++) {
          csv += String((*d->vec)[i]);
          if (i < d->vec->size() - 1)
            csv += ",";
        }
        lv_textarea_set_text(d->ta, csv.c_str());

        // Cleanup
        lv_obj_del(lv_obj_get_parent(lv_event_get_target(e))); // Delete Panel
        delete d->vec;
        delete d;
        lvgl_port_unlock();
      },
      LV_EVENT_CLICKED, NULL);

  // We are already inside a lock, but the callback above unlocks it?
  // No, the callback above deletes the panel and unlocks.
  // Wait, the outer function calls lvgl_port_unlock().
  // We should NOT unlock in the callback if it's async?
  // Callbacks run in LVGL task. They should lock/unlock if they do heavy stuff?
  // Usually LVGL callbacks don't need explicit lock if called by LVGL task.
  // But here we are deleting objects.
  // The 'lvgl_port_unlock' at the end of this function matches the lock at the
  // start. The callback needs to handle itself.

  lvgl_port_unlock();
}

// --- Settings UI Helpers ---
extern const char *SETTING_COLOR_NAMES;
extern CRGB setting_color_values[];

static int get_color_index(CRGB c) {
  for (int i = 0; i < 7; i++) {
    if (c.r == setting_color_values[i].r && c.g == setting_color_values[i].g &&
        c.b == setting_color_values[i].b)
      return i;
  }
  return 0;
}

extern bool settings_reboot_needed;

struct ConfirmationData {
  lv_event_cb_t yes_callback;
  lv_event_cb_t no_callback;
  void *user_data;
  lv_obj_t *modal;
};

void show_confirmation_popup(const char *title, const char *message,
                             lv_event_cb_t yes_cb, lv_event_cb_t no_cb,
                             void *user_data) {
  lvgl_port_lock(-1);
  lv_obj_t *modal = lv_obj_create(lv_scr_act());
  lv_obj_set_size(modal, 500, 250);
  lv_obj_center(modal);
  lv_obj_add_style(modal, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x0d0d0d), 0);

  lv_obj_t *lbl_title = lv_label_create(modal);
  lv_label_set_text(lbl_title, title);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_add_style(lbl_title, &style_text_header, 0);

  lv_obj_t *lbl_msg = lv_label_create(modal);
  lv_label_set_text(lbl_msg, message);
  lv_obj_set_width(lbl_msg, 450);
  lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
  lv_obj_align(lbl_msg, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_style_text_align(lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xFFFFFF), 0);

  ConfirmationData *conf_data =
      new ConfirmationData{yes_cb, no_cb, user_data, modal};

  lv_obj_t *btn_yes = lv_btn_create(modal);
  lv_obj_set_size(btn_yes, 120, 50);
  lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 50, -20);
  lv_obj_set_style_bg_color(btn_yes, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_t *lbl_yes = lv_label_create(btn_yes);
  lv_label_set_text(lbl_yes, "YES");
  lv_obj_center(lbl_yes);
  lv_obj_add_event_cb(
      btn_yes,
      [](lv_event_t *e) {
        ConfirmationData *d = (ConfirmationData *)lv_event_get_user_data(e);
        if (d->yes_callback)
          d->yes_callback(e);
        lv_obj_del(d->modal);
        delete d;
      },
      LV_EVENT_CLICKED, conf_data);

  lv_obj_t *btn_no = lv_btn_create(modal);
  lv_obj_set_size(btn_no, 120, 50);
  lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
  lv_obj_set_style_bg_color(btn_no, lv_color_hex(0xff4444), 0);
  lv_obj_t *lbl_no = lv_label_create(btn_no);
  lv_label_set_text(lbl_no, "NO");
  lv_obj_center(lbl_no);
  lv_obj_add_event_cb(
      btn_no,
      [](lv_event_t *e) {
        ConfirmationData *d = (ConfirmationData *)lv_event_get_user_data(e);
        if (d->no_callback)
          d->no_callback(e);
        lv_obj_del(d->modal);
        delete d;
      },
      LV_EVENT_CLICKED, conf_data);

  lvgl_port_unlock();
}

struct InfoData {
  lv_event_cb_t cb;
  void *data;
  lv_obj_t *modal;
};

void show_info_popup(const char *title, const char *message,
                     lv_event_cb_t ok_cb, void *user_data) {
  lvgl_port_lock(-1);

  lv_obj_t *modal = lv_obj_create(lv_scr_act());
  lv_obj_set_size(modal, 450, 250);
  lv_obj_center(modal);
  lv_obj_add_style(modal, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x222222), 0);
  lv_obj_set_style_border_color(modal, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_border_width(modal, 2, 0);

  lv_obj_t *lbl_title = lv_label_create(modal);
  lv_label_set_text(lbl_title, title);
  lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 20);
  lv_obj_add_style(lbl_title, &style_text_header, 0);

  lv_obj_t *lbl_msg = lv_label_create(modal);
  lv_label_set_text(lbl_msg, message);
  lv_obj_set_width(lbl_msg, 400);
  lv_label_set_long_mode(lbl_msg, LV_LABEL_LONG_WRAP);
  lv_obj_align(lbl_msg, LV_ALIGN_TOP_MID, 0, 70);
  lv_obj_set_style_text_align(lbl_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl_msg, lv_color_hex(0xFFFFFF), 0);

  InfoData *info_data = new InfoData{ok_cb, user_data, modal};

  lv_obj_t *btn_ok = lv_btn_create(modal);
  lv_obj_set_size(btn_ok, 120, 50);
  lv_obj_align(btn_ok, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_add_style(btn_ok, &style_btn_header_green, 0);

  lv_obj_t *lbl_ok = lv_label_create(btn_ok);
  lv_label_set_text(lbl_ok, "OK");
  lv_obj_center(lbl_ok);

  lv_obj_add_event_cb(
      btn_ok,
      [](lv_event_t *e) {
        InfoData *d = (InfoData *)lv_event_get_user_data(e);
        if (d->cb) {
          d->cb(e);
        }
        lv_obj_del(d->modal);
        delete d;
      },
      LV_EVENT_CLICKED, info_data);

  lvgl_port_unlock();
}

void show_settings_ui() {
  lvgl_port_lock(-1);

  settings_reboot_needed = false; // Reset flag on open

  lv_obj_t *panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(panel, 750, 450);
  lv_obj_center(panel);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(getCurrentThemeColor()), 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_radius(panel, 10, 0);
  lv_obj_add_style(panel, &style_modal_panel, 0);

  // Keyboard (Shared) - Created early
  lv_obj_t *kb = lv_keyboard_create(panel);
  lv_obj_set_size(kb, 700, 140);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      kb,
      [](lv_event_t *e) {
        lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(
      kb,
      [](lv_event_t *e) {
        lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
      },
      LV_EVENT_CANCEL, NULL);

  // Close Button
  lv_obj_t *btn_close = lv_btn_create(panel);
  lv_obj_set_size(btn_close, 40, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, -10);
  lv_obj_add_style(btn_close, &style_btn_close, 0);
  lv_obj_add_event_cb(
      btn_close,
      [](lv_event_t *e) {
        lv_obj_del(lv_obj_get_parent(lv_event_get_target(e)));
        saveSettings();
        if (settings_reboot_needed) {
          Serial.println("Rebooting for new settings...");
          lv_obj_t *mbox =
              lv_msgbox_create(NULL, "Restarting",
                               "Settings changed. Rebooting...", NULL, false);
          lv_obj_center(mbox);
          // Fix text color
          lv_obj_t *content = lv_msgbox_get_content(mbox);
          if (content)
            lv_obj_set_style_text_color(content, lv_color_hex(0xFFFFFF), 0);
          lv_obj_t *title_lbl = lv_msgbox_get_title(mbox);
          if (title_lbl)
            lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
          lv_timer_create([](lv_timer_t *t) { ESP.restart(); }, 1000, NULL);
        }
      },
      LV_EVENT_CLICKED, NULL);

  lv_obj_t *l_close = lv_label_create(btn_close);
  lv_label_set_text(l_close, LV_SYMBOL_CLOSE);
  lv_obj_center(l_close);
  lv_obj_set_style_text_color(l_close, lv_color_hex(0xff4444), 0);

  // Title
  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "SETTINGS");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(getCurrentThemeColor()), 0);

  // TAB VIEW
  lv_obj_t *tabview = lv_tabview_create(panel, LV_DIR_RIGHT, 100);
  lv_obj_set_size(tabview, 710, 350);
  lv_obj_align(tabview, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(tabview, lv_color_hex(0x1a1a1a), 0);

  // Tab Styling
  lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tabview);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x222222), 0);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(getCurrentThemeColor()),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(tab_btns, lv_color_hex(getCurrentThemeColor()),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

  // TABS
  lv_obj_t *tab1 = lv_tabview_add_tab(tabview, LV_SYMBOL_BULLET " LEDs");
  lv_obj_t *tab2 = lv_tabview_add_tab(tabview, LV_SYMBOL_WIFI " Web");
  lv_obj_t *tab3 = lv_tabview_add_tab(tabview, LV_SYMBOL_SETTINGS " Features");
  lv_obj_t *tab4 = lv_tabview_add_tab(tabview, LV_SYMBOL_DRIVE " System");
  lv_obj_t *tab5 = lv_tabview_add_tab(tabview, LV_SYMBOL_EDIT " Theme");

  // === TAB 1: LED Settings ===
  int led_y = 15;

  // Use WLED Switch
  lv_obj_t *sw_wled = lv_switch_create(tab1);
  lv_obj_set_size(sw_wled, 50, 25);
  lv_obj_align(sw_wled, LV_ALIGN_TOP_LEFT, 200, led_y);
  if (led_use_wled)
    lv_obj_add_state(sw_wled, LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      sw_wled,
      [](lv_event_t *e) {
        led_use_wled =
            lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        settings_reboot_needed = true;
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *l_wled = lv_label_create(tab1);
  lv_label_set_text(l_wled, "Use WLED Sync:");
  lv_obj_align(l_wled, LV_ALIGN_TOP_LEFT, 20, led_y + 5);
  lv_obj_set_style_text_color(l_wled, lv_color_hex(0xffffff), 0);

  // WLED IP
  lv_obj_t *lbl_ip = lv_label_create(tab1);
  lv_label_set_text(lbl_ip, "WLED IP:");
  lv_obj_align(lbl_ip, LV_ALIGN_TOP_LEFT, 20, led_y + 55);
  lv_obj_set_style_text_color(lbl_ip, lv_color_hex(0xcccccc), 0);

  lv_obj_t *ta_ip = lv_textarea_create(tab1);
  lv_obj_set_size(ta_ip, 200, 38);
  lv_obj_align(ta_ip, LV_ALIGN_TOP_LEFT, 140, led_y + 48);
  lv_textarea_set_one_line(ta_ip, true);
  lv_textarea_set_text(ta_ip, wled_ip.c_str());
  lv_obj_add_style(ta_ip, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_ip,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_ip,
      [](lv_event_t *e) {
        wled_ip = lv_textarea_get_text(lv_event_get_target(e));
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // LED Count
  lv_obj_t *lbl_cnt = lv_label_create(tab1);
  lv_label_set_text(lbl_cnt, "Total LED Count:");
  lv_obj_align(lbl_cnt, LV_ALIGN_TOP_LEFT, 20, led_y + 105);
  lv_obj_set_style_text_color(lbl_cnt, lv_color_hex(0xcccccc), 0);

  lv_obj_t *ta_cnt = lv_textarea_create(tab1);
  lv_obj_set_size(ta_cnt, 120, 38);
  lv_obj_align(ta_cnt, LV_ALIGN_TOP_LEFT, 200, led_y + 98);
  lv_textarea_set_one_line(ta_cnt, true);
  lv_textarea_set_text(ta_cnt, String(led_count).c_str());
  lv_obj_add_style(ta_cnt, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_cnt,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_cnt,
      [](lv_event_t *e) {
        String s = lv_textarea_get_text(lv_event_get_target(e));
        if (s.length() > 0) {
          led_count = s.toInt();
          settings_reboot_needed = true;
        }
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // Brightness Slider
  lv_obj_t *lbl_bright = lv_label_create(tab1);
  lv_label_set_text(lbl_bright, "Brightness:");
  lv_obj_align(lbl_bright, LV_ALIGN_TOP_LEFT, 20, led_y + 155);
  lv_obj_set_style_text_color(lbl_bright, lv_color_hex(0xffffff), 0);

  lv_obj_t *slider_bright = lv_slider_create(tab1);
  lv_obj_set_size(slider_bright, 200, 15);
  lv_obj_align(slider_bright, LV_ALIGN_TOP_LEFT, 140, led_y + 160);
  lv_slider_set_range(slider_bright, 0, 255);
  lv_slider_set_value(slider_bright, led_brightness, LV_ANIM_OFF);
  lv_obj_add_event_cb(
      slider_bright,
      [](lv_event_t *e) {
        led_brightness = lv_slider_get_value(lv_event_get_target(e));
        FastLED.setBrightness(led_brightness);
        FastLED.show();
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // === TAB 2: Web Interface ===
  int web_y = 10;

  lv_obj_t *lbl_pin = lv_label_create(tab2);
  lv_label_set_text(lbl_pin, "Web Access PIN:");
  lv_obj_align(lbl_pin, LV_ALIGN_TOP_LEFT, 20, web_y + 5);
  lv_obj_set_style_text_color(lbl_pin, lv_color_hex(0xffffff), 0);

  lv_obj_t *ta_pin = lv_textarea_create(tab2);
  lv_obj_set_size(ta_pin, 150, 40);
  lv_obj_align(ta_pin, LV_ALIGN_TOP_LEFT, 180, web_y);
  lv_textarea_set_one_line(ta_pin, true);
  lv_textarea_set_text(ta_pin, web_pin.c_str());
  lv_obj_add_style(ta_pin, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_pin,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_pin,
      [](lv_event_t *e) {
        web_pin = lv_textarea_get_text(lv_event_get_target(e));
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lbl_mdns = lv_label_create(tab2);
  lv_label_set_text(lbl_mdns, "mDNS Name (.local):");
  lv_obj_align(lbl_mdns, LV_ALIGN_TOP_LEFT, 20, web_y + 55);
  lv_obj_set_style_text_color(lbl_mdns, lv_color_hex(0xffffff), 0);

  lv_obj_t *ta_mdns = lv_textarea_create(tab2);
  lv_obj_set_size(ta_mdns, 200, 40);
  lv_obj_align(ta_mdns, LV_ALIGN_TOP_LEFT, 180, web_y + 50);
  lv_textarea_set_one_line(ta_mdns, true);
  lv_textarea_set_text(ta_mdns, mdns_name.c_str());
  lv_obj_add_style(ta_mdns, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_mdns,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_mdns,
      [](lv_event_t *e) {
        mdns_name = lv_textarea_get_text(lv_event_get_target(e));
        settings_reboot_needed = true;
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // === TAB 3: Features ===
  lv_obj_t *lbl_cache = lv_label_create(tab3);
  lv_label_set_text(lbl_cache, "Nav Cache Size (per side):");
  lv_obj_align(lbl_cache, LV_ALIGN_TOP_LEFT, 20, 15);
  lv_obj_set_style_text_color(lbl_cache, lv_color_hex(0xffffff), 0);

  lv_obj_t *dd_cache = lv_dropdown_create(tab3);
  lv_dropdown_set_options(dd_cache, "5 Items\n10 Items\n15 Items");
  lv_obj_set_width(dd_cache, 120);
  lv_obj_align(dd_cache, LV_ALIGN_TOP_LEFT, 250, 10);

  // Set current selection
  if (setting_cache_size == 15)
    lv_dropdown_set_selected(dd_cache, 2);
  else if (setting_cache_size == 10)
    lv_dropdown_set_selected(dd_cache, 1);
  else
    lv_dropdown_set_selected(dd_cache, 0);

  lv_obj_add_event_cb(
      dd_cache,
      [](lv_event_t *e) {
        lv_obj_t *dd = lv_event_get_target(e);
        int sel = lv_dropdown_get_selected(dd);
        if (sel == 0)
          setting_cache_size = 5;
        else if (sel == 1)
          setting_cache_size = 10;
        else if (sel == 2)
          setting_cache_size = 15;
        settings_reboot_needed = true;
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  int feat_y = 65;
  lv_obj_t *sw_cds = lv_switch_create(tab3);
  lv_obj_set_size(sw_cds, 50, 25);
  lv_obj_align(sw_cds, LV_ALIGN_TOP_LEFT, 180, feat_y);
  if (setting_enable_cds)
    lv_obj_add_state(sw_cds, LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      sw_cds,
      [](lv_event_t *e) {
        setting_enable_cds =
            lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        settings_reboot_needed = true;
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lbl_cds = lv_label_create(tab3);
  lv_label_set_text(lbl_cds, "Enable CDs:");
  lv_obj_align(lbl_cds, LV_ALIGN_TOP_LEFT, 20, feat_y + 5);
  lv_obj_set_style_text_color(lbl_cds, lv_color_hex(0xffffff), 0);

  lv_obj_t *lbl_cd_start = lv_label_create(tab3);
  lv_label_set_text(lbl_cd_start, "CDs LED Start:");
  lv_obj_align(lbl_cd_start, LV_ALIGN_TOP_LEFT, 20, feat_y + 45);
  lv_obj_set_style_text_color(lbl_cd_start, lv_color_hex(0xcccccc), 0);

  lv_obj_t *ta_cd_start = lv_textarea_create(tab3);
  lv_obj_set_size(ta_cd_start, 120, 40);
  lv_obj_align(ta_cd_start, LV_ALIGN_TOP_LEFT, 180, feat_y + 35);
  lv_textarea_set_one_line(ta_cd_start, true);
  lv_textarea_set_text(ta_cd_start, String(setting_cds_led_start).c_str());
  lv_obj_add_style(ta_cd_start, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_cd_start,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_cd_start,
      [](lv_event_t *e) {
        String s = lv_textarea_get_text(lv_event_get_target(e));
        if (s.length() > 0) {
          setting_cds_led_start = s.toInt();
          settings_reboot_needed = true;
        }
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // --- BOOK SETTINGS ---
  int book_y = feat_y + 90;

  lv_obj_t *sw_books = lv_switch_create(tab3);
  lv_obj_set_size(sw_books, 50, 25);
  lv_obj_align(sw_books, LV_ALIGN_TOP_LEFT, 180, book_y);
  if (setting_enable_books)
    lv_obj_add_state(sw_books, LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      sw_books,
      [](lv_event_t *e) {
        setting_enable_books =
            lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        settings_reboot_needed = true;
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lbl_books = lv_label_create(tab3);
  lv_label_set_text(lbl_books, "Enable Books:");
  lv_obj_align(lbl_books, LV_ALIGN_TOP_LEFT, 20, book_y + 5);
  lv_obj_set_style_text_color(lbl_books, lv_color_hex(0xffffff), 0);

  lv_obj_t *lbl_led_start = lv_label_create(tab3);
  lv_label_set_text(lbl_led_start, "Books LED Start:");
  lv_obj_align(lbl_led_start, LV_ALIGN_TOP_LEFT, 20, book_y + 45);
  lv_obj_set_style_text_color(lbl_led_start, lv_color_hex(0xcccccc), 0);

  lv_obj_t *ta_led_start = lv_textarea_create(tab3);
  lv_obj_set_size(ta_led_start, 120, 40);
  lv_obj_align(ta_led_start, LV_ALIGN_TOP_LEFT, 180, book_y + 35);
  lv_textarea_set_one_line(ta_led_start, true);
  lv_textarea_set_text(ta_led_start, String(setting_books_led_start).c_str());
  lv_obj_add_style(ta_led_start, &style_textarea_cursor,
                   LV_PART_CURSOR | LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      ta_led_start,
      [](lv_event_t *e) {
        lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb, lv_event_get_target(e));
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_move_foreground(kb);
      },
      LV_EVENT_CLICKED, kb);
  lv_obj_add_event_cb(
      ta_led_start,
      [](lv_event_t *e) {
        String s = lv_textarea_get_text(lv_event_get_target(e));
        if (s.length() > 0) {
          setting_books_led_start = s.toInt();
          settings_reboot_needed = true;
        }
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  // === TAB 4: System ===
  // No flex flow here - using explicit positioning for two-column layout

  // DIAGNOSTICS SECTION (Right Side)
  lv_obj_t *l_diag_title = lv_label_create(tab4);
  lv_label_set_text(l_diag_title, "System Diagnostics:");
  lv_obj_align(l_diag_title, LV_ALIGN_TOP_LEFT, 260, 20);
  lv_obj_set_style_text_color(l_diag_title,
                              lv_color_hex(getCurrentThemeColor()), 0);

  lv_obj_t *l_diag_info = lv_label_create(tab4);
  char diag_buf[1024]; // Increased size for more info

  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t usedHeap = totalHeap - freeHeap;

  size_t freePsram = ESP.getFreePsram();
  size_t totalPsram = ESP.getPsramSize();
  size_t usedPsram = totalPsram - freePsram;

  uint64_t sd_total = 0;
  uint64_t sd_used = 0;
  bool sd_ok = false;

  // CRITICAL: Pull CS LOW to talk to SD card
  if (sdExpander)
    sdExpander->digitalWrite(SD_CS, LOW);

  if (SD.cardType() != CARD_NONE) {
    sd_total = SD.totalBytes() / (1024 * 1024);
    sd_used = SD.usedBytes() / (1024 * 1024);
    sd_ok = true;
  }

  if (sdExpander)
    sdExpander->digitalWrite(SD_CS, HIGH);

  snprintf(diag_buf, sizeof(diag_buf),
           "NETWORK:\n"
           "  IP: %s\n"
           "  mDNS: %s.local\n"
           "  WiFi: %s\n\n"
           "MEMORY:\n"
           "  Int. Heap: %u/%u KB\n"
           "  PSRAM: %u/%u KB\n\n"
           "STORAGE (SD):\n"
           "  Status: %s\n"
           "  Used: %llu MB\n"
           "  Total: %llu MB\n\n"
           "STATS:\n"
           "  Items in RAM: %d\n"
           "  Uptime: %u min",
           WiFi.localIP().toString().c_str(), mdns_name.c_str(),
           WiFi.SSID().c_str(), usedHeap / 1024, totalHeap / 1024,
           usedPsram / 1024, totalPsram / 1024, sd_ok ? "Mounted" : "FAILED",
           sd_used, sd_total, getItemCount(), millis() / 60000);

  lv_label_set_text(l_diag_info, diag_buf);
  lv_obj_align(l_diag_info, LV_ALIGN_TOP_LEFT, 260, 50);
  lv_obj_set_style_text_color(l_diag_info, lv_color_hex(0xaaaaaa), 0);
  lv_obj_set_style_text_font(l_diag_info, &lv_font_montserrat_12,
                             0); // Smaller font to fit more

  logMemoryUsage("Settings UI Opened");

  // RESTART BUTTON (Left Side)
  lv_obj_t *btn_reboot_man = lv_btn_create(tab4);
  lv_obj_set_size(btn_reboot_man, 220, 50);
  lv_obj_align(btn_reboot_man, LV_ALIGN_TOP_LEFT, 20, 20); // Top left position
  lv_obj_set_style_bg_color(btn_reboot_man, lv_color_hex(0x555555), 0);

  lv_obj_t *l_reboot_man = lv_label_create(btn_reboot_man);
  lv_label_set_text(l_reboot_man, LV_SYMBOL_REFRESH " RESTART DEVICE");
  lv_obj_center(l_reboot_man);
  lv_obj_set_style_text_color(l_reboot_man, lv_color_hex(0xffffff), 0);

  lv_obj_add_event_cb(
      btn_reboot_man,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Restart Device", "Are you sure you want to reboot the device?",
            [](lv_event_t *e) {
              // YES callback - restart device
              ESP.restart();
            },
            NULL // NO callback - just closes
        );
      },
      LV_EVENT_CLICKED, NULL);

  // FACTORY RESET BUTTON
  lv_obj_t *btn_reset = lv_btn_create(tab4);
  lv_obj_set_size(btn_reset, 220, 50);
  lv_obj_align(btn_reset, LV_ALIGN_TOP_LEFT, 20, 80); // Explicit position
  lv_obj_set_style_bg_color(btn_reset, lv_color_hex(0xff4444), 0);

  lv_obj_t *l_reset = lv_label_create(btn_reset);
  lv_label_set_text(l_reset, LV_SYMBOL_TRASH " FACTORY RESET");
  lv_obj_center(l_reset);
  lv_obj_set_style_text_color(l_reset, lv_color_hex(0xffffff), 0);

  lv_obj_add_event_cb(
      btn_reset,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Factory Reset",
            "Wipe all settings and WiFi credentials? Device will reboot.",
            [](lv_event_t *e) {
              preferences.begin("settings", false);
              preferences.clear();
              preferences.end();
              preferences.begin("wifi", false);
              preferences.clear();
              preferences.end();
              ESP.restart();
            });
      },
      LV_EVENT_CLICKED, NULL);

  // WIPE CDS BUTTON
  lv_obj_t *btn_wipe_cds = lv_btn_create(tab4);
  lv_obj_set_size(btn_wipe_cds, 220, 50);
  lv_obj_align(btn_wipe_cds, LV_ALIGN_TOP_LEFT, 20, 140); // Explicit position
  lv_obj_set_style_bg_color(btn_wipe_cds, lv_color_hex(0xff8800), 0);

  lv_obj_t *l_wipe_cds = lv_label_create(btn_wipe_cds);
  lv_label_set_text(l_wipe_cds, LV_SYMBOL_TRASH " WIPE ALL CDS");
  lv_obj_center(l_wipe_cds);
  lv_obj_set_style_text_color(l_wipe_cds, lv_color_hex(0xffffff), 0);

  lv_obj_add_event_cb(
      btn_wipe_cds,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Wipe CD Data", "Delete ALL CDs? Cannot be undone!",
            [](lv_event_t *e) {
              if (Storage.wipeLibrary(MODE_CD)) {
                show_info_popup("Success", "CD Data Wiped. Rebooting...",
                                [](lv_event_t *e) { ESP.restart(); });
              } else {
                show_info_popup("Error", "Failed to wipe CD data.");
              }
            });
      },
      LV_EVENT_CLICKED, NULL);

  // WIPE BOOKS BUTTON
  lv_obj_t *btn_wipe_books = lv_btn_create(tab4);
  lv_obj_set_size(btn_wipe_books, 220, 50);
  lv_obj_align(btn_wipe_books, LV_ALIGN_TOP_LEFT, 20, 200); // Explicit position
  lv_obj_set_style_bg_color(btn_wipe_books, lv_color_hex(0xff8800), 0);

  lv_obj_t *l_wipe_books = lv_label_create(btn_wipe_books);
  lv_label_set_text(l_wipe_books, LV_SYMBOL_TRASH " WIPE ALL BOOKS");
  lv_obj_center(l_wipe_books);
  lv_obj_set_style_text_color(l_wipe_books, lv_color_hex(0xffffff), 0);

  lv_obj_add_event_cb(
      btn_wipe_books,
      [](lv_event_t *e) {
        show_confirmation_popup(
            "Wipe Book Data", "Delete ALL Books? Cannot be undone!",
            [](lv_event_t *e) {
              if (Storage.wipeLibrary(MODE_BOOK)) {
                show_info_popup("Success", "Book Data Wiped. Rebooting...",
                                [](lv_event_t *e) { ESP.restart(); });
              } else {
                show_info_popup("Error", "Failed to wipe Book data.");
              }
            });
      },
      LV_EVENT_CLICKED, NULL);

  // === TAB 5: Theme Settings ===
  lv_obj_set_flex_flow(tab5, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tab5, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // Helper to create a theme column
  struct ThemeContext {
    uint32_t *var;
    lv_obj_t *prev;
    lv_obj_t *hex;
    lv_obj_t *mode;
  };

  auto create_theme_section = [&](lv_obj_t *parent, const char *title,
                                  uint32_t *targetVar, const char *icon) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_set_size(col, 280, 320);
    lv_obj_set_style_bg_opa(col, 0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_gap(col, 8, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(col);
    lv_label_set_text_fmt(lbl, "%s %s", icon, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

    // Preview Square
    lv_obj_t *preview = lv_obj_create(col);
    lv_obj_set_size(preview, 240, 30);
    lv_obj_set_style_bg_color(preview, lv_color_hex(*targetVar), 0);
    lv_obj_set_style_radius(preview, 5, 0);
    lv_obj_set_style_border_width(preview, 1, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(0x444444), 0);

    lv_obj_t *lbl_hex = lv_label_create(preview);
    lv_obj_center(lbl_hex);
    lv_obj_set_style_text_font(lbl_hex, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hex, lv_color_hex(0x000000), 0);
    char hexStr[10];
    sprintf(hexStr, "#%06X", (unsigned int)*targetVar);
    lv_label_set_text(lbl_hex, hexStr);

    lv_obj_t *lbl_help = lv_label_create(col);
    lv_label_set_text(lbl_help, "Tap & hold center to change Palette Mode");
    lv_obj_set_style_text_font(lbl_help, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_help, lv_color_hex(0x888888), 0);

    lv_obj_t *cw = lv_colorwheel_create(col, true);
    lv_obj_set_size(cw, 160, 160);
    lv_obj_set_style_arc_width(cw, 15, 0);
    lv_colorwheel_set_rgb(cw, lv_color_hex(*targetVar));

    lv_obj_t *lbl_mode = lv_label_create(col);
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(getCurrentThemeColor()),
                                0);
    lv_label_set_text(lbl_mode, "MODE: HUE");

    ThemeContext *ctx = new ThemeContext{targetVar, preview, lbl_hex, lbl_mode};

    lv_obj_add_event_cb(
        cw,
        [](lv_event_t *e) {
          ThemeContext *c = (ThemeContext *)lv_event_get_user_data(e);
          lv_obj_t *wheel = lv_event_get_target(e);

          if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
            lv_color_t color = lv_colorwheel_get_rgb(wheel);
            *(c->var) = lv_color_to32(color);
            lv_obj_set_style_bg_color(c->prev, color, 0);
            char buf[10];
            sprintf(buf, "#%06X", (unsigned int)*(c->var));
            lv_label_set_text(c->hex, buf);
            uint8_t bright = (color.ch.red * 299 + color.ch.green * 587 +
                              color.ch.blue * 114) /
                             1000;
            lv_obj_set_style_text_color(
                c->hex, lv_color_hex(bright > 128 ? 0 : 0xFFFFFF), 0);
          }

          lv_colorwheel_mode_t m = lv_colorwheel_get_color_mode(wheel);
          if (m == LV_COLORWHEEL_MODE_HUE)
            lv_label_set_text(c->mode, "MODE: HUE");
          else if (m == LV_COLORWHEEL_MODE_SATURATION)
            lv_label_set_text(c->mode, "MODE: SATURATION");
          else if (m == LV_COLORWHEEL_MODE_VALUE)
            lv_label_set_text(c->mode, "MODE: BRIGHTNESS");
        },
        LV_EVENT_ALL, ctx);

    lv_obj_add_event_cb(
        col,
        [](lv_event_t *e) { delete (ThemeContext *)lv_event_get_user_data(e); },
        LV_EVENT_DELETE, ctx);
  };

  create_theme_section(tab5, "CD MODE THEME", &setting_theme_cd,
                       LV_SYMBOL_AUDIO);
  create_theme_section(tab5, "BOOK MODE THEME", &setting_theme_book,
                       LV_SYMBOL_FILE);

  lvgl_port_unlock();
}

// Bulk cover art check and download
void performBulkCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("performBulkCheck: WiFi not connected");
    return;
  }

  lvgl_port_lock(-1);
  lv_obj_t *sync_modal = lv_obj_create(lv_scr_act());
  lv_obj_set_size(sync_modal, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(sync_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(sync_modal, LV_OPA_COVER, 0);

  lv_obj_t *bar = lv_bar_create(sync_modal);
  lv_obj_set_size(bar, 400, 20);
  lv_obj_center(bar);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(getCurrentThemeColor()),
                            LV_PART_INDICATOR);

  lv_obj_t *lbl = lv_label_create(sync_modal);
  lv_label_set_text(lbl, "Initializing Sync...");
  lv_obj_align_to(lbl, bar, LV_ALIGN_OUT_TOP_MID, 0, -10);

  lv_obj_t *btn_skip = lv_btn_create(sync_modal);
  lv_obj_set_size(btn_skip, 100, 50);
  lv_obj_align(btn_skip, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_label_set_text(lv_label_create(btn_skip), "SKIP");
  lv_obj_add_event_cb(
      btn_skip, [](lv_event_t *e) { is_sync_stopping = true; },
      LV_EVENT_CLICKED, NULL);

  lvgl_port_unlock();

  int total = getItemCount();
  is_sync_stopping = false;
  for (int i = 0; i < total; i++) {
    if (is_sync_stopping)
      break;
    ItemView item = getItemAtRAM(i);
    lvgl_port_lock(-1);
    lv_bar_set_value(bar, (i * 100) / total, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl, "Checking %d/%d: %s", i + 1, total,
                          item.title.c_str());
    lvgl_port_unlock();

    // Check if cover is missing
    if (item.coverFile.length() < 5) {
      // Note: Covers are fetched via Search button (btn_search_clicked)
      Serial.printf("Missing cover for: %s\n", item.title.c_str());
    }
    delay(50);
  }

  lvgl_port_lock(-1);
  lv_obj_del(sync_modal);
  lvgl_port_unlock();

  Serial.println("performBulkCheck complete");
}

// QR Code UI Functions
void show_qr_ui() {
  lvgl_port_lock(-1);

  // Create Modal (Maximize but keep margins)
  lv_obj_t *panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(panel, 620, 460); // 620x460 (Fits 800x480 comfortably)
  lv_obj_center(panel);
  lv_obj_add_style(panel, &style_modal_panel, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  // Header
  lv_obj_t *h1 = lv_label_create(panel);
  lv_label_set_text(h1, LV_SYMBOL_HOME " WEB INTERFACE");
  lv_obj_align(h1, LV_ALIGN_TOP_MID, 0, 5); // Raised to top
  lv_obj_add_style(h1, &style_text_header, 0);

  // Close Button
  lv_obj_t *btn_close = lv_btn_create(panel);
  lv_obj_set_size(btn_close, 45, 40);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -5, 5);
  lv_obj_add_style(btn_close, &style_btn_close, 0); // Standard Style
  lv_obj_add_event_cb(
      btn_close,
      [](lv_event_t *e) {
        lv_obj_del(lv_obj_get_parent(lv_event_get_target(e)));
      },
      LV_EVENT_CLICKED, NULL);
  lv_obj_t *l_close = lv_label_create(btn_close);
  lv_label_set_text(l_close, LV_SYMBOL_CLOSE);
  lv_obj_center(l_close);
  lv_obj_set_style_text_color(l_close, lv_color_hex(0xff4444), 0);

  // URL Box (Prominent)
  lv_obj_t *url_box = lv_obj_create(panel);
  lv_obj_set_size(url_box, 500, 45);
  lv_obj_align(url_box, LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_set_style_bg_color(url_box, lv_color_hex(0x151515), 0);
  lv_obj_set_style_border_color(url_box, lv_color_hex(getCurrentThemeColor()),
                                0);
  lv_obj_set_style_border_width(url_box, 1, 0);
  lv_obj_clear_flag(url_box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *url_lbl = lv_label_create(url_box);
  lv_label_set_text(url_lbl, "http://mylibrary.local");
  lv_obj_center(url_lbl);
  lv_obj_set_style_text_color(url_lbl, lv_color_hex(getCurrentThemeColor()), 0);

  // Feature List Container
  lv_obj_t *list = lv_obj_create(panel);
  lv_obj_set_size(list, 580, 340);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 115); // Adjusted Y pos
  lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(list, 8, 0); // Spacing between items

  // Helper Lambda
  auto add_row = [&](const char *icon, const char *name, const char *path,
                     const char *desc) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_size(row, 540, 55); // Wider, shorter height
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Icon (Left)
    lv_obj_t *icn = lv_label_create(row);
    lv_label_set_text(icn, icon);
    lv_obj_align(icn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_text_color(icn, lv_color_hex(getCurrentThemeColor()), 0);

    // Name (Top Line)
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text_fmt(lbl, "%s   %s", name, path); // Name + Path
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 45, 8);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

    // Desc (Bottom Line)
    lv_obj_t *d = lv_label_create(row);
    lv_label_set_text(d, desc);
    lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 45, -8);
    lv_obj_set_style_text_color(d, lv_color_hex(0xaaaaaa), 0);
  };

  String remoteTitle = "Remote " + getModeName() + " Control";
  String remoteDesc =
      "Play, pause, select " + getModeNamePlural() + " remotely";
  add_row(LV_SYMBOL_PLAY, remoteTitle.c_str(), "/browse", remoteDesc.c_str());

  String scanTitle = getScannerTitle();
  String scanDesc = "Add " + getModeNamePlural() + " via phone camera";
  add_row(LV_SYMBOL_PLUS, scanTitle.c_str(), "/scan", scanDesc.c_str());

  String coverTitle = getArtToolTitle();
  add_row(LV_SYMBOL_IMAGE, coverTitle.c_str(), "/link",
          "Fix missing album/book covers");
  add_row(LV_SYMBOL_SAVE, "Backup & Restore", "/backup",
          "Save/Load database to PC");
  add_row(LV_SYMBOL_FILE, "User Manual", "/manual", "Read the user guide");

  // Library Mode Toggle (Only if Books Enabled)
  if (setting_enable_books) {
    lv_obj_t *row_mode = lv_obj_create(list);
    lv_obj_set_size(row_mode, 540, 55);
    lv_obj_set_style_bg_color(row_mode, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(row_mode, 0, 0);
    lv_obj_clear_flag(row_mode, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(
        row_mode,
        [](lv_event_t *e) {
          // Toggle Mode using abstraction
          MediaMode newMode = getOtherMode();

          // Save Preference (Settings Namespace)
          preferences.begin("settings", false);
          preferences.putInt("mode", (int)newMode);
          preferences.end();

          // Show Feedback
          lv_obj_t *panel = lv_obj_create(lv_scr_act());
          lv_obj_set_size(panel, 320, 180);
          lv_obj_center(panel);
          lv_obj_add_style(panel, &style_modal_panel, 0);
          lv_obj_set_style_bg_color(panel, lv_color_hex(0x000000), 0);

          lv_obj_t *title = lv_label_create(panel);
          lv_label_set_text(title, "Switching Mode");
          lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
          lv_obj_add_style(title, &style_text_header, 0);

          lv_obj_t *msg = lv_label_create(panel);
          lv_label_set_text(msg, "Restarting device...");
          lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
          lv_obj_set_style_text_color(msg, lv_color_hex(0xcccccc), 0);

          // Restart after short delay
          lv_timer_create([](lv_timer_t *t) { ESP.restart(); }, 1000, NULL);
        },
        LV_EVENT_CLICKED, NULL);

    // Icon (Switch symbol based on TARGET mode)
    lv_obj_t *icn_mode = lv_label_create(row_mode);
    lv_label_set_text(icn_mode, getOtherModeIcon());
    lv_obj_align(icn_mode, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_text_color(icn_mode, lv_color_hex(0xffdd00), 0);

    // Title (Switch to OTHER mode)
    lv_obj_t *lbl_mode = lv_label_create(row_mode);
    String switchText = "Switch to " + getOtherModeNamePlural();
    switchText.toUpperCase();
    lv_label_set_text(lbl_mode, switchText.c_str());
    lv_obj_align(lbl_mode, LV_ALIGN_TOP_LEFT, 45, 8);
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(0xffffff), 0);

    // Desc
    lv_obj_t *desc_mode = lv_label_create(row_mode);
    lv_label_set_text(desc_mode, "Toggle Library Mode (Restarts)");
    lv_obj_align(desc_mode, LV_ALIGN_BOTTOM_LEFT, 45, -8);
    lv_obj_set_style_text_color(desc_mode, lv_color_hex(0xaaaaaa), 0);
  }

  // Settings Button - Manual Add
  lv_obj_t *row_settings = lv_obj_create(list);
  lv_obj_set_size(row_settings, 540, 55);
  lv_obj_set_style_bg_color(row_settings, lv_color_hex(0x1a1a1a), 0);
  lv_obj_set_style_border_width(row_settings, 0, 0);
  lv_obj_clear_flag(row_settings, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(
      row_settings, [](lv_event_t *e) { show_settings_ui(); }, LV_EVENT_CLICKED,
      NULL);

  lv_obj_t *icn_sett = lv_label_create(row_settings);
  lv_label_set_text(icn_sett, LV_SYMBOL_SETTINGS);
  lv_obj_align(icn_sett, LV_ALIGN_LEFT_MID, 10, 0);
  lv_obj_set_style_text_color(icn_sett, lv_color_hex(getCurrentThemeColor()),
                              0);

  lv_obj_t *lbl_sett = lv_label_create(row_settings);
  lv_label_set_text(lbl_sett, "Device Settings");
  lv_obj_align(lbl_sett, LV_ALIGN_TOP_LEFT, 45, 8);
  lv_obj_set_style_text_color(lbl_sett, lv_color_hex(0xffffff), 0);

  lv_obj_t *d_sett = lv_label_create(row_settings);
  lv_label_set_text(d_sett, "Configure Books, LEDs, & More");
  lv_obj_align(d_sett, LV_ALIGN_BOTTOM_LEFT, 45, -8);
  lv_obj_set_style_text_color(d_sett, lv_color_hex(0xaaaaaa), 0);

  lvgl_port_unlock();
}

void close_qr_ui() {
  // Placeholder for closing QR UI
  Serial.println("close_qr_ui() called");
}

void trigger_screensaver() {
  if (is_screen_off)
    return;
  Serial.println("[SLEEP] Entering Screen Saver Mode...");
  is_screen_off = true;
  if (sdExpander) {
    sdExpander->digitalWrite(LCD_BL, LOW);
  }
  FastLED.clear();
  FastLED.show();
  if (led_use_wled)
    AppNetworkManager::forceUpdateWLED();
}

void wake_screen() {
  if (!is_screen_off)
    return;
  Serial.println("[WAKE] Waking up...");
  is_screen_off = false;
  if (sdExpander) {
    sdExpander->digitalWrite(LCD_BL, HIGH);
  }
  update_item_display();
}
