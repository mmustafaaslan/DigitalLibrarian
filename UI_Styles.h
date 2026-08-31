#ifndef UI_STYLES_H
#define UI_STYLES_H

#include <lvgl.h>

extern lv_style_t style_btn_header_green;
extern lv_style_t style_btn_close;
extern lv_style_t style_modal_panel;
extern lv_style_t style_text_header;
extern lv_style_t style_textarea_cursor;
extern lv_style_t style_screen;
extern lv_style_t style_topbar;
extern lv_style_t style_surface;
extern lv_style_t style_icon_button;
extern lv_style_t style_icon_button_pressed;
extern lv_style_t style_primary_button;
extern lv_style_t style_secondary_button;
extern lv_style_t style_meta_chip;
extern lv_style_t style_text_muted;

void ui_styles_init();

#endif
