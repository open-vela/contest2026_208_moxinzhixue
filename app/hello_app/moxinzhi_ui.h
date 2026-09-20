/****************************************************************************
 * Contest 2026 team 208 - product UI
 ****************************************************************************/

#ifndef __MOXINZHI_UI_H
#define __MOXINZHI_UI_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#include "moxinzhi_diag.h"
#include "moxinzhi_input.h"
#include "moxinzhi_model.h"
#include "moxinzhi_platform.h"

enum mz_page_e
{
  MZ_PAGE_HOME = 0,
  MZ_PAGE_ASK,
  MZ_PAGE_CARDS,
  MZ_PAGE_STATS,
  MZ_PAGE_SETTINGS,
  MZ_PAGE_COUNT
};

struct mz_ui_s;

struct mz_nav_event_s
{
  FAR struct mz_ui_s *ui;
  enum mz_page_e page;
};

struct mz_prompt_event_s
{
  FAR struct mz_ui_s *ui;
  uint8_t index;
};

struct mz_wifi_event_s
{
  FAR struct mz_ui_s *ui;
  uint8_t index;
};

struct mz_setting_event_s
{
  FAR struct mz_ui_s *ui;
  uint8_t value;
};

struct mz_ui_s
{
  FAR lv_display_t *display;
  FAR struct mz_model_s *model;
  FAR lv_obj_t *root;
  FAR lv_obj_t *content;
  FAR lv_obj_t *nav_buttons[MZ_PAGE_COUNT];
  FAR lv_obj_t *ai_status_bar;
  FAR lv_obj_t *ai_status_label;
  FAR lv_obj_t *ai_transcript;
  FAR lv_obj_t *ai_empty;
  FAR lv_obj_t *ai_prompt_buttons[MZ_PROMPT_COUNT];
  FAR lv_obj_t *ai_question_panel;
  FAR lv_obj_t *ai_question_label;
  FAR lv_obj_t *ai_answer_panel;
  FAR lv_obj_t *ai_topic_label;
  FAR lv_obj_t *ai_answer_label;
  FAR lv_obj_t *ai_save_button;
  FAR lv_obj_t *ai_save_label;
  FAR lv_obj_t *ai_text_button;
  FAR lv_obj_t *ai_bottom_anchor;
  FAR lv_obj_t *volume_popup;
  FAR lv_obj_t *volume_popup_label;
  FAR lv_obj_t *volume_popup_bar;
  FAR lv_timer_t *volume_popup_timer;
  FAR lv_font_t *font_cn;
  FAR const lv_font_t *font_text;
  struct mz_diag_s diag;
  struct mz_input_s input;
  struct mz_platform_binding_s platform;
  struct mz_platform_status_s platform_status;
  struct mz_nav_event_s nav_events[MZ_PAGE_COUNT];
  struct mz_prompt_event_s prompt_events[MZ_PROMPT_COUNT];
  struct mz_wifi_event_s wifi_events[MZ_PLATFORM_WIFI_SCAN_MAX];
  struct mz_setting_event_s language_events[2];
  struct mz_setting_event_s theme_events[2];
  FAR lv_obj_t *volume_slider;
  FAR lv_obj_t *brightness_slider;
  char wifi_ssid[MZ_PLATFORM_SSID_MAX];
  char wifi_password[MZ_PLATFORM_PASSWORD_MAX];
  char ask_question[MZ_QUESTION_MAX];
  int32_t ai_scroll_y;
  enum mz_page_e page;
  uint8_t notice;
  bool chinese;
  bool chinese_available;
  bool dark_theme;
  bool touch_available;
  bool compact;
  bool reset_confirm;
  bool ai_follow_bottom;
  bool ai_ptt_pressed;
  bool ai_ptt_started;
  bool ai_ptt_cancelled;
  bool wifi_scan_requested;
};

int mz_ui_init(FAR struct mz_ui_s *ui, FAR lv_display_t *display,
               FAR struct mz_model_s *model, bool touch_available,
               bool start_diagnostic);
int mz_ui_init_with_platform(
  FAR struct mz_ui_s *ui, FAR lv_display_t *display,
  FAR struct mz_model_s *model, bool touch_available,
  FAR const struct mz_platform_binding_s *platform,
  bool start_diagnostic);
void mz_ui_bind_platform(
  FAR struct mz_ui_s *ui,
  FAR const struct mz_platform_binding_s *platform);
int mz_ui_hardware_ptt_press(FAR struct mz_ui_s *ui);
void mz_ui_hardware_ptt_release(FAR struct mz_ui_s *ui, bool cancel);
int mz_ui_hardware_volume_step(FAR struct mz_ui_s *ui, int delta);
void mz_ui_show_volume_popup(FAR struct mz_ui_s *ui, uint8_t volume);
void mz_ui_platform_changed(FAR struct mz_ui_s *ui);
void mz_ui_model_changed(FAR struct mz_ui_s *ui);
void mz_ui_deinit(FAR struct mz_ui_s *ui);

#endif /* __MOXINZHI_UI_H */
