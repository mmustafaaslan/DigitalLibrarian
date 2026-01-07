#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "NetworkManager.h" // Ensure this is available or forward declare if possible, but UIManager seems to use it.
#include <Arduino.h>
#include <lvgl.h>
#include <vector>

// --- Public UI Functions ---
// --- Global UI Variables ---
extern int edit_item_index;
extern lv_obj_t *ta_led_index;

// --- Public UI Functions ---
void setupMainUI();
void updateDisplay();
void update_item_display();

// --- Panels & Modals ---
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
void show_tracklist_ui(int index);
void close_tracklist_ui();
void show_chapter_list_ui(int index);
void show_lyrics_popup(String trackTitle, String lyricsText);
void close_lyrics_popup();
void show_qr_ui();
void close_qr_ui();
void show_led_selector_ui(lv_obj_t *target_ta);

// --- Helpers ---
void load_and_show_cover(String filename);
void performBulkCheck();
void update_filtered_leds();
void trigger_screensaver();
void wake_screen();
void show_confirmation_popup(const char *title, const char *message,
                             lv_event_cb_t yes_cb, lv_event_cb_t no_cb = NULL,
                             void *user_data = NULL);
void show_info_popup(const char *title, const char *message,
                     lv_event_cb_t ok_cb = NULL, void *user_data = NULL);
void selectRandomWithEffect();

// --- UI Objects ---
extern lv_obj_t *label_title;
extern lv_obj_t *label_artist;
extern lv_obj_t *label_genre;
extern lv_obj_t *label_year;
extern lv_obj_t *label_led;
extern lv_obj_t *label_notes;
extern lv_obj_t *label_favorites;
extern lv_obj_t *label_counter;
extern lv_obj_t *img_cover;
extern lv_obj_t *img_cover_container;
extern lv_obj_t *label_cover_url;
extern lv_obj_t *btn_search;
extern lv_obj_t *label_favorite;
extern lv_obj_t *label_extra_info;
extern lv_obj_t *label_filter_status;
extern lv_obj_t *btn_tracklist;
extern lv_obj_t *btn_prev;
extern lv_obj_t *btn_next;
extern lv_obj_t *btn_edit;

// --- Add/Edit Modal Objects ---
extern lv_obj_t *ta_barcode;
extern lv_obj_t *ta_title;
extern lv_obj_t *ta_artist;
extern lv_obj_t *ta_genre;
extern lv_obj_t *ta_year;
extern lv_obj_t *ta_led_index;
extern lv_obj_t *ta_uniqueID;
extern lv_obj_t *ta_notes;

// --- Panel Objects ---
extern lv_obj_t *tracklist_panel;
extern lv_obj_t *lyrics_panel;
extern lv_obj_t *search_panel;
extern lv_obj_t *add_item_panel;
extern lv_obj_t *wifi_config_panel;
extern lv_obj_t *filter_panel;

// --- Structs ---
struct SelectorData {
  lv_obj_t *ta;
  std::vector<int> *vec;
  int page;
  lv_obj_t *grid;
  lv_obj_t *lbl_page;
};

// --- Globals (Needed for callbacks/logic) ---
extern int edit_item_index;
extern bool sort_by_artist;

#endif // UI_MANAGER_H
