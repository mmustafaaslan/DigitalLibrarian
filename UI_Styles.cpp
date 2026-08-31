#include "UI_Styles.h"
#include "AppGlobals.h"
#include <lvgl.h>

lv_style_t style_btn_header_green;
lv_style_t style_btn_close;
lv_style_t style_modal_panel;
lv_style_t style_text_header;
lv_style_t style_textarea_cursor;
lv_style_t style_screen;
lv_style_t style_topbar;
lv_style_t style_surface;
lv_style_t style_icon_button;
lv_style_t style_icon_button_pressed;
lv_style_t style_primary_button;
lv_style_t style_secondary_button;
lv_style_t style_meta_chip;
lv_style_t style_text_muted;

void ui_styles_init() {
  const lv_color_t accent = lv_color_hex(getCurrentThemeColor());
  const lv_color_t canvas = lv_color_hex(0x090D12);
  const lv_color_t topbar = lv_color_hex(0x0F151D);
  const lv_color_t surface = lv_color_hex(0x141C25);
  const lv_color_t surfaceRaised = lv_color_hex(0x1B2632);
  const lv_color_t outline = lv_color_hex(0x2A3948);
  const lv_color_t textPrimary = lv_color_hex(0xF4F7FA);
  const lv_color_t textMuted = lv_color_hex(0x8FA1B3);

  // Base screen and structural surfaces.
  lv_style_init(&style_screen);
  lv_style_set_bg_color(&style_screen, canvas);
  lv_style_set_bg_opa(&style_screen, LV_OPA_COVER);
  lv_style_set_text_color(&style_screen, textPrimary);

  lv_style_init(&style_topbar);
  lv_style_set_bg_color(&style_topbar, topbar);
  lv_style_set_bg_opa(&style_topbar, LV_OPA_COVER);
  lv_style_set_border_color(&style_topbar, outline);
  lv_style_set_border_width(&style_topbar, 1);
  lv_style_set_border_side(&style_topbar, LV_BORDER_SIDE_BOTTOM);
  lv_style_set_radius(&style_topbar, 0);
  lv_style_set_pad_all(&style_topbar, 0);

  lv_style_init(&style_surface);
  lv_style_set_bg_color(&style_surface, surface);
  lv_style_set_bg_opa(&style_surface, LV_OPA_COVER);
  lv_style_set_border_color(&style_surface, outline);
  lv_style_set_border_width(&style_surface, 1);
  lv_style_set_radius(&style_surface, 14);
  lv_style_set_shadow_color(&style_surface, lv_color_hex(0x000000));
  lv_style_set_shadow_width(&style_surface, 12);
  lv_style_set_shadow_opa(&style_surface, LV_OPA_30);
  lv_style_set_shadow_ofs_y(&style_surface, 5);
  lv_style_set_pad_all(&style_surface, 0);

  // Compact header actions.
  lv_style_init(&style_icon_button);
  lv_style_set_bg_color(&style_icon_button, surfaceRaised);
  lv_style_set_bg_opa(&style_icon_button, LV_OPA_COVER);
  lv_style_set_border_color(&style_icon_button, outline);
  lv_style_set_border_width(&style_icon_button, 1);
  lv_style_set_radius(&style_icon_button, 10);
  lv_style_set_shadow_width(&style_icon_button, 0);
  lv_style_set_text_color(&style_icon_button, textPrimary);
  lv_style_set_pad_all(&style_icon_button, 0);

  lv_style_init(&style_icon_button_pressed);
  lv_style_set_bg_color(&style_icon_button_pressed, accent);
  lv_style_set_border_color(&style_icon_button_pressed, accent);
  lv_style_set_text_color(&style_icon_button_pressed, canvas);
  lv_style_set_transform_width(&style_icon_button_pressed, -2);
  lv_style_set_transform_height(&style_icon_button_pressed, -2);

  // Primary and secondary navigation actions.
  lv_style_init(&style_primary_button);
  lv_style_set_bg_color(&style_primary_button, accent);
  lv_style_set_bg_opa(&style_primary_button, LV_OPA_COVER);
  lv_style_set_border_width(&style_primary_button, 0);
  lv_style_set_radius(&style_primary_button, 14);
  lv_style_set_shadow_color(&style_primary_button, accent);
  lv_style_set_shadow_width(&style_primary_button, 14);
  lv_style_set_shadow_opa(&style_primary_button, LV_OPA_20);
  lv_style_set_shadow_ofs_y(&style_primary_button, 4);
  lv_style_set_text_color(&style_primary_button, canvas);

  lv_style_init(&style_secondary_button);
  lv_style_set_bg_color(&style_secondary_button, surfaceRaised);
  lv_style_set_bg_opa(&style_secondary_button, LV_OPA_COVER);
  lv_style_set_border_color(&style_secondary_button, outline);
  lv_style_set_border_width(&style_secondary_button, 1);
  lv_style_set_radius(&style_secondary_button, 14);
  lv_style_set_shadow_width(&style_secondary_button, 0);
  lv_style_set_text_color(&style_secondary_button, textPrimary);

  lv_style_init(&style_meta_chip);
  lv_style_set_bg_color(&style_meta_chip, lv_color_hex(0x101820));
  lv_style_set_bg_opa(&style_meta_chip, LV_OPA_COVER);
  lv_style_set_border_color(&style_meta_chip, outline);
  lv_style_set_border_width(&style_meta_chip, 1);
  lv_style_set_radius(&style_meta_chip, 8);
  lv_style_set_pad_left(&style_meta_chip, 8);
  lv_style_set_pad_right(&style_meta_chip, 8);
  lv_style_set_pad_top(&style_meta_chip, 4);
  lv_style_set_pad_bottom(&style_meta_chip, 4);
  lv_style_set_text_color(&style_meta_chip, accent);

  lv_style_init(&style_text_muted);
  lv_style_set_text_color(&style_text_muted, textMuted);

  // Header button compatibility style used by panels throughout the app.
  lv_style_init(&style_btn_header_green);
  lv_style_set_bg_color(&style_btn_header_green, surfaceRaised);
  lv_style_set_bg_opa(&style_btn_header_green, LV_OPA_COVER);
  lv_style_set_border_color(&style_btn_header_green, outline);
  lv_style_set_border_width(&style_btn_header_green, 1);
  lv_style_set_radius(&style_btn_header_green, 10);
  lv_style_set_shadow_width(&style_btn_header_green, 0);
  lv_style_set_text_color(&style_btn_header_green, accent);

  // 2. Close Button Style (Red, Transparent, Flat)
  lv_style_init(&style_btn_close);
  lv_style_set_bg_opa(&style_btn_close, LV_OPA_TRANSP);
  lv_style_set_border_color(&style_btn_close, lv_color_hex(0xff4444));
  lv_style_set_border_width(&style_btn_close, 2);
  lv_style_set_shadow_width(&style_btn_close, 0);
  lv_style_set_text_color(&style_btn_close, lv_color_hex(0xff4444));

  // Modal panels inherit the same elevated surface language.
  lv_style_init(&style_modal_panel);
  lv_style_set_bg_color(&style_modal_panel, surface);
  lv_style_set_bg_opa(&style_modal_panel, LV_OPA_COVER);
  lv_style_set_border_color(&style_modal_panel, outline);
  lv_style_set_border_width(&style_modal_panel, 1);
  lv_style_set_radius(&style_modal_panel, 16);
  lv_style_set_shadow_color(&style_modal_panel, lv_color_hex(0x000000));
  lv_style_set_shadow_width(&style_modal_panel, 20);
  lv_style_set_shadow_opa(&style_modal_panel, LV_OPA_50);
  lv_style_set_shadow_ofs_y(&style_modal_panel, 8);

  // 4. Header Text (Green, Title Font)
  lv_style_init(&style_text_header);
  lv_style_set_text_color(&style_text_header, textPrimary);
  lv_style_set_text_font(&style_text_header, &lv_font_montserrat_16);

  // 5. Global Textarea Cursor Style
  lv_style_init(&style_textarea_cursor);
  lv_style_set_border_color(&style_textarea_cursor, lv_color_hex(0xffffff));
  lv_style_set_border_width(&style_textarea_cursor, 2);
  lv_style_set_border_side(&style_textarea_cursor, LV_BORDER_SIDE_LEFT);
  lv_style_set_bg_opa(&style_textarea_cursor, LV_OPA_TRANSP);
  lv_style_set_anim_time(&style_textarea_cursor, 500);
}
