#include "UI_Styles.h"
#include "AppGlobals.h"
#include <lvgl.h>

lv_style_t style_btn_header_green;
lv_style_t style_btn_close;
lv_style_t style_modal_panel;
lv_style_t style_text_header;
lv_style_t style_textarea_cursor;

void ui_styles_init() {
  // 1. Header Button Style (Green, Transparent, Flat)
  lv_style_init(&style_btn_header_green);
  lv_style_set_bg_opa(&style_btn_header_green, LV_OPA_TRANSP);
  lv_style_set_border_color(&style_btn_header_green,
                            lv_color_hex(getCurrentThemeColor()));
  lv_style_set_border_width(&style_btn_header_green, 2);
  lv_style_set_shadow_width(&style_btn_header_green, 0);
  lv_style_set_text_color(&style_btn_header_green,
                          lv_color_hex(getCurrentThemeColor()));

  // 2. Close Button Style (Red, Transparent, Flat)
  lv_style_init(&style_btn_close);
  lv_style_set_bg_opa(&style_btn_close, LV_OPA_TRANSP);
  lv_style_set_border_color(&style_btn_close, lv_color_hex(0xff4444));
  lv_style_set_border_width(&style_btn_close, 2);
  lv_style_set_shadow_width(&style_btn_close, 0);
  lv_style_set_text_color(&style_btn_close, lv_color_hex(0xff4444));

  // 3. Modal Panel (Black bg, Green border)
  lv_style_init(&style_modal_panel);
  lv_style_set_bg_color(&style_modal_panel, lv_color_hex(0x000000));
  lv_style_set_bg_opa(&style_modal_panel, LV_OPA_COVER);
  lv_style_set_border_color(&style_modal_panel,
                            lv_color_hex(getCurrentThemeColor()));
  lv_style_set_border_width(&style_modal_panel, 2);
  lv_style_set_radius(&style_modal_panel, 10);

  // 4. Header Text (Green, Title Font)
  lv_style_init(&style_text_header);
  lv_style_set_text_color(&style_text_header,
                          lv_color_hex(getCurrentThemeColor()));
  lv_style_set_text_font(&style_text_header, &lv_font_montserrat_16);

  // 5. Global Textarea Cursor Style
  lv_style_init(&style_textarea_cursor);
  lv_style_set_border_color(&style_textarea_cursor, lv_color_hex(0xffffff));
  lv_style_set_border_width(&style_textarea_cursor, 2);
  lv_style_set_border_side(&style_textarea_cursor, LV_BORDER_SIDE_LEFT);
  lv_style_set_bg_opa(&style_textarea_cursor, LV_OPA_TRANSP);
  lv_style_set_anim_time(&style_textarea_cursor, 500);
}
