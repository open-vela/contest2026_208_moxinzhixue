/****************************************************************************
 * Contest 2026 team 208 - reusable touch input panel
 ****************************************************************************/

#ifndef __MOXINZHI_INPUT_H
#define __MOXINZHI_INPUT_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <lvgl/lvgl.h>

#define MZ_INPUT_TEXT_MAX 384
#define MZ_INPUT_CANDIDATE_COUNT 3

enum mz_input_mode_e
{
  MZ_INPUT_TEXT = 0,
  MZ_INPUT_NUMBER,
  MZ_INPUT_PASSWORD
};

typedef void (*mz_input_submit_t)(FAR void *context,
                                  FAR const char *value);
typedef void (*mz_input_cancel_t)(FAR void *context);

struct mz_input_config_s
{
  FAR const char *title;
  FAR const char *placeholder;
  FAR const char *initial;
  FAR const lv_font_t *font;
  enum mz_input_mode_e mode;
  size_t max_length;
  bool pinyin;
  mz_input_submit_t submit;
  mz_input_cancel_t cancel;
  FAR void *context;
};

struct mz_input_s;

struct mz_input_candidate_event_s
{
  FAR struct mz_input_s *input;
  uint8_t index;
};

struct mz_input_s
{
  FAR lv_obj_t *overlay;
  FAR lv_obj_t *textarea;
  FAR lv_obj_t *keyboard;
  FAR lv_obj_t *candidate_row;
  FAR lv_obj_t *candidate_buttons[MZ_INPUT_CANDIDATE_COUNT];
  FAR const char *candidates[MZ_INPUT_CANDIDATE_COUNT];
  struct mz_input_candidate_event_s
    candidate_events[MZ_INPUT_CANDIDATE_COUNT];
  FAR const lv_font_t *font;
  mz_input_submit_t submit;
  mz_input_cancel_t cancel;
  FAR void *context;
  bool pinyin;
  bool closing;
};

int mz_input_open(FAR struct mz_input_s *input, FAR lv_obj_t *parent,
                  FAR const struct mz_input_config_s *config);
void mz_input_close(FAR struct mz_input_s *input);
bool mz_input_is_open(FAR const struct mz_input_s *input);

#endif /* __MOXINZHI_INPUT_H */
