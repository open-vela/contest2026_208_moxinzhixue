/****************************************************************************
 * Contest 2026 team 208 - retained LCD/touch diagnostics
 ****************************************************************************/

#ifndef __MOXINZHI_DIAG_H
#define __MOXINZHI_DIAG_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

struct mz_diag_s
{
  FAR lv_obj_t *refresh_label;
  FAR lv_obj_t *blink_block;
  FAR lv_timer_t *refresh_timer;
  uint32_t refresh_count;
  bool blink_on;
  bool active;
  bool touch_available;

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  FAR lv_obj_t *touch_label;
  FAR lv_obj_t *touch_hline;
  FAR lv_obj_t *touch_vline;
#endif
};

int mz_diag_start(FAR struct mz_diag_s *diag, FAR lv_display_t *display,
                  bool touch_available, lv_event_cb_t back_cb,
                  FAR void *back_data);
void mz_diag_stop(FAR struct mz_diag_s *diag);

#endif /* __MOXINZHI_DIAG_H */
