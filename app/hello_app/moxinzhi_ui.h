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
#include "moxinzhi_model.h"

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

struct mz_ui_s
{
  FAR lv_display_t *display;
  FAR struct mz_model_s *model;
  FAR lv_obj_t *root;
  FAR lv_obj_t *content;
  FAR lv_obj_t *nav_buttons[MZ_PAGE_COUNT];
  FAR lv_font_t *font_cn;
  FAR const lv_font_t *font_text;
  struct mz_diag_s diag;
  struct mz_nav_event_s nav_events[MZ_PAGE_COUNT];
  struct mz_prompt_event_s prompt_events[MZ_PROMPT_COUNT];
  enum mz_page_e page;
  uint8_t notice;
  bool chinese;
  bool touch_available;
  bool compact;
  bool reset_confirm;
};

int mz_ui_init(FAR struct mz_ui_s *ui, FAR lv_display_t *display,
               FAR struct mz_model_s *model, bool touch_available,
               bool start_diagnostic);
void mz_ui_deinit(FAR struct mz_ui_s *ui);

#endif /* __MOXINZHI_UI_H */
