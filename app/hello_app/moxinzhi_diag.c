/****************************************************************************
 * Contest 2026 team 208 - retained LCD/touch diagnostics
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "moxinzhi_diag.h"

#define MZ_DIAG_UPDATE_MS 250

static FAR lv_obj_t *mz_diag_block(FAR lv_obj_t *parent,
                                   int32_t x, int32_t y,
                                   int32_t width, int32_t height,
                                   uint32_t color)
{
  FAR lv_obj_t *block = lv_obj_create(parent);

  lv_obj_remove_style_all(block);
  lv_obj_set_pos(block, x, y);
  lv_obj_set_size(block, width, height);
  lv_obj_set_style_bg_color(block, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
  lv_obj_remove_flag(block,
                     LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return block;
}

static void mz_diag_bars(FAR lv_obj_t *screen, int32_t x, int32_t y,
                         int32_t width, int32_t height)
{
  static const uint32_t g_colors[] =
  {
    0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
    0xff00ff, 0xff0000, 0x0000ff, 0x000000
  };

  int32_t left;
  int32_t right;
  int i;

  for (i = 0; i < 8; i++)
    {
      left = x + width * i / 8;
      right = x + width * (i + 1) / 8;
      mz_diag_block(screen, left, y, right - left, height, g_colors[i]);
    }
}

static void mz_diag_grayscale(FAR lv_obj_t *screen, int32_t x, int32_t y,
                              int32_t width, int32_t height)
{
  uint32_t level;
  uint32_t color;
  int32_t left;
  int32_t right;
  int i;

  for (i = 0; i < 8; i++)
    {
      level = (uint32_t)i * 255 / 7;
      color = level << 16 | level << 8 | level;
      left = x + width * i / 8;
      right = x + width * (i + 1) / 8;
      mz_diag_block(screen, left, y, right - left, height, color);
    }
}

static void mz_diag_corner(FAR lv_obj_t *screen, int32_t x, int32_t y,
                           int32_t xdir, int32_t ydir)
{
  int32_t horizontal_x = xdir > 0 ? x : x - 10;
  int32_t horizontal_y = ydir > 0 ? y : y - 2;
  int32_t vertical_x = xdir > 0 ? x : x - 2;
  int32_t vertical_y = ydir > 0 ? y : y - 10;

  mz_diag_block(screen, horizontal_x, horizontal_y, 10, 2, 0xffffff);
  mz_diag_block(screen, vertical_x, vertical_y, 2, 10, 0xffffff);
}

static void mz_diag_corners(FAR lv_obj_t *screen, int32_t width,
                            int32_t height)
{
  mz_diag_corner(screen, 2, 2, 1, 1);
  mz_diag_corner(screen, width - 2, 2, -1, 1);
  mz_diag_corner(screen, 2, height - 2, 1, -1);
  mz_diag_corner(screen, width - 2, height - 2, -1, -1);
}

static void mz_diag_timer_cb(FAR lv_timer_t *timer)
{
  FAR struct mz_diag_s *diag = lv_timer_get_user_data(timer);

  diag->refresh_count++;
  diag->blink_on = !diag->blink_on;
  lv_label_set_text_fmt(diag->refresh_label, "Refresh: %lu",
                        (unsigned long)diag->refresh_count);
  lv_obj_set_style_bg_color(diag->blink_block,
                            lv_color_hex(diag->blink_on ? 0x27d17f :
                                                          0x202020),
                            0);
}

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
static void mz_diag_touch_cb(FAR lv_event_t *event)
{
  FAR struct mz_diag_s *diag = lv_event_get_user_data(event);
  FAR lv_obj_t *layer = lv_event_get_current_target(event);
  FAR lv_indev_t *indev = lv_indev_active();
  lv_point_t point;
  int32_t width;
  int32_t height;
  int32_t cross_x;
  int32_t cross_y;

  if (indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &point);
  width = lv_obj_get_width(layer);
  height = lv_obj_get_height(layer);
  point.x = LV_CLAMP(0, point.x, width - 1);
  point.y = LV_CLAMP(0, point.y, height - 1);
  cross_x = LV_MIN(point.x > 0 ? point.x - 1 : 0, width - 3);
  cross_y = LV_MIN(point.y > 0 ? point.y - 1 : 0, height - 3);

  lv_obj_set_x(diag->touch_vline, cross_x);
  lv_obj_set_y(diag->touch_hline, cross_y);
  lv_obj_remove_flag(diag->touch_vline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(diag->touch_hline, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(diag->touch_label, "Touch: %ld, %ld",
                        (long)point.x, (long)point.y);
}

static void mz_diag_touch_layer(FAR lv_obj_t *screen,
                                FAR struct mz_diag_s *diag,
                                int32_t width, int32_t height,
                                int32_t label_y, int32_t margin)
{
  FAR lv_obj_t *layer = lv_obj_create(screen);

  lv_obj_remove_style_all(layer);
  lv_obj_set_pos(layer, 0, 0);
  lv_obj_set_size(layer, width, height);
  lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);

  diag->touch_vline = mz_diag_block(layer, 0, 0, 3, height, 0x000000);
  diag->touch_hline = mz_diag_block(layer, 0, 0, width, 3, 0x000000);
  mz_diag_block(diag->touch_vline, 1, 0, 1, height, 0xffffff);
  mz_diag_block(diag->touch_hline, 0, 1, width, 1, 0xffffff);
  lv_obj_add_flag(diag->touch_vline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(diag->touch_hline, LV_OBJ_FLAG_HIDDEN);

  diag->touch_label = lv_label_create(layer);
  lv_label_set_text(diag->touch_label, "Touch: ---, ---");
  lv_obj_set_pos(diag->touch_label, margin, label_y);
  lv_obj_set_width(diag->touch_label, width - margin * 2);
  lv_obj_set_style_text_color(diag->touch_label, lv_color_hex(0xffffff), 0);
  lv_obj_remove_flag(diag->touch_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(layer, mz_diag_touch_cb, LV_EVENT_PRESSED, diag);
  lv_obj_add_event_cb(layer, mz_diag_touch_cb, LV_EVENT_PRESSING, diag);
}
#endif

static FAR lv_obj_t *mz_diag_back_button(FAR lv_obj_t *screen,
                                         lv_event_cb_t back_cb,
                                         FAR void *back_data)
{
  FAR lv_obj_t *button = lv_button_create(screen);
  FAR lv_obj_t *label = lv_label_create(button);

  lv_obj_set_pos(button, 5, 4);
  lv_obj_set_size(button, 36, 28);
  lv_obj_set_style_radius(button, 4, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x171717), 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_label_set_text(label, LV_SYMBOL_LEFT);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, back_cb, LV_EVENT_CLICKED, back_data);
  return button;
}

int mz_diag_start(FAR struct mz_diag_s *diag, FAR lv_display_t *display,
                  bool touch_available, lv_event_cb_t back_cb,
                  FAR void *back_data)
{
  FAR lv_obj_t *screen = lv_screen_active();
  FAR lv_obj_t *title;
  int32_t width;
  int32_t height;
  int32_t margin;
  int32_t gap;
  int32_t header_height;
  int32_t status_height;
  int32_t pattern_height;
  int32_t color_height;
  int32_t gray_height;
  int32_t color_y;
  int32_t gray_y;
  int32_t status_y;

  if (diag == NULL || display == NULL || back_cb == NULL)
    {
      return -EINVAL;
    }

  mz_diag_stop(diag);
  memset(diag, 0, sizeof(*diag));
  diag->touch_available = touch_available;

  width = lv_display_get_horizontal_resolution(display);
  height = lv_display_get_vertical_resolution(display);
  if (width < 160 || height < 120)
    {
      return -ERANGE;
    }

  margin = width >= 200 ? 6 : 3;
  gap = 3;
  header_height = LV_CLAMP(24, height / 10, 34);
  status_height = touch_available ? 48 : 28;
  pattern_height = height - header_height - status_height - margin * 2 -
                   gap * 2;
  if (pattern_height < 24)
    {
      return -ERANGE;
    }

  color_height = pattern_height * 2 / 3;
  gray_height = pattern_height - color_height;
  color_y = margin + header_height;
  gray_y = color_y + color_height + gap;
  status_y = gray_y + gray_height + gap;

  lv_obj_clean(screen);
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(screen, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(screen, 2, 0);
  lv_obj_set_style_border_post(screen, true, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  title = lv_label_create(screen);
  lv_label_set_text_fmt(title, "HARDWARE TEST  %ldx%ld",
                        (long)width, (long)height);
  lv_obj_set_width(title, width - 88);
  lv_obj_set_pos(title, 44, margin + 4);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

  mz_diag_bars(screen, margin, color_y, width - margin * 2, color_height);
  mz_diag_grayscale(screen, margin, gray_y, width - margin * 2,
                    gray_height);

  diag->refresh_label = lv_label_create(screen);
  lv_label_set_text(diag->refresh_label, "Refresh: 0");
  lv_obj_set_pos(diag->refresh_label, margin, status_y + 3);
  lv_obj_set_style_text_color(diag->refresh_label,
                              lv_color_hex(0xffffff), 0);

  diag->blink_block = mz_diag_block(screen, width - margin - 16,
                                    status_y + 3, 16, 16, 0x202020);
  lv_obj_set_style_border_color(diag->blink_block, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_width(diag->blink_block, 1, 0);
  mz_diag_corners(screen, width, height);

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  if (touch_available)
    {
      mz_diag_touch_layer(screen, diag, width, height, status_y + 24,
                          margin);
    }
  else
    {
      diag->touch_label = lv_label_create(screen);
      lv_label_set_text(diag->touch_label, "Touch: unavailable");
      lv_obj_set_pos(diag->touch_label, margin, status_y + 24);
      lv_obj_set_style_text_color(diag->touch_label,
                                  lv_color_hex(0xff5c5c), 0);
    }
#endif

  mz_diag_back_button(screen, back_cb, back_data);
  diag->refresh_timer = lv_timer_create(mz_diag_timer_cb,
                                        MZ_DIAG_UPDATE_MS, diag);
  if (diag->refresh_timer == NULL)
    {
      lv_obj_clean(screen);
      return -ENOMEM;
    }

  diag->active = true;
  return 0;
}

void mz_diag_stop(FAR struct mz_diag_s *diag)
{
  if (diag != NULL && diag->active)
    {
      if (diag->refresh_timer != NULL)
        {
          lv_timer_delete(diag->refresh_timer);
        }

      diag->refresh_timer = NULL;
      diag->active = false;
    }
}
