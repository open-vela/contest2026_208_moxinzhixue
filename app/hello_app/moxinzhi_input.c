/****************************************************************************
 * Contest 2026 team 208 - reusable touch input panel
 ****************************************************************************/

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "moxinzhi_input.h"

#define MZ_INPUT_BG       0x101815
#define MZ_INPUT_SURFACE  0x17231e
#define MZ_INPUT_FIELD    0x0d1511
#define MZ_INPUT_KEY      0x26362e
#define MZ_INPUT_INK      0xf5f8f6
#define MZ_INPUT_MUTED    0xb4c3bc
#define MZ_INPUT_LINE     0x4b5d54
#define MZ_INPUT_GREEN    0x177a57

struct mz_pinyin_entry_s
{
  FAR const char *key;
  FAR const char *words[MZ_INPUT_CANDIDATE_COUNT];
};

static const struct mz_pinyin_entry_s g_pinyin[] =
{
  {"ni",       {"你", "你好", "年"}},
  {"hao",      {"好", "号码", "好的"}},
  {"nihao",    {"你好", "您好", NULL}},
  {"xue",      {"学", "学习", "学校"}},
  {"xuexi",    {"学习", NULL, NULL}},
  {"xuesheng", {"学生", NULL, NULL}},
  {"xuexiao",  {"学校", NULL, NULL}},
  {"laoshi",   {"老师", NULL, NULL}},
  {"zhong",    {"中", "中文", "中心"}},
  {"zhongwen", {"中文", NULL, NULL}},
  {"wang",     {"网", "网络", "网站"}},
  {"wangluo",  {"网络", NULL, NULL}},
  {"shebei",   {"设备", NULL, NULL}},
  {"mima",     {"密码", NULL, NULL}},
  {"jia",      {"家", "家庭", "加"}}
};

static void mz_input_update_candidates(FAR struct mz_input_s *input)
{
  FAR const char *text;
  FAR const char *key;
  FAR const struct mz_pinyin_entry_s *entry = NULL;
  size_t length;
  size_t i;

  if (!input->pinyin || input->candidate_row == NULL)
    {
      return;
    }

  text = lv_textarea_get_text(input->textarea);
  length = strlen(text);
  key = text + length;
  while (key > text && isalpha((unsigned char)key[-1]))
    {
      key--;
    }

  if (*key != '\0')
    {
      for (i = 0; i < sizeof(g_pinyin) / sizeof(g_pinyin[0]); i++)
        {
          if (strcmp(key, g_pinyin[i].key) == 0)
            {
              entry = &g_pinyin[i];
              break;
            }
        }
    }

  for (i = 0; i < MZ_INPUT_CANDIDATE_COUNT; i++)
    {
      FAR lv_obj_t *button = input->candidate_buttons[i];
      FAR lv_obj_t *label = lv_obj_get_child(button, 0);

      input->candidates[i] = entry == NULL ? NULL : entry->words[i];
      if (input->candidates[i] == NULL)
        {
          lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_label_set_text(label, input->candidates[i]);
          lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void mz_input_value_cb(FAR lv_event_t *event)
{
  FAR struct mz_input_s *input = lv_event_get_user_data(event);

  mz_input_update_candidates(input);
}

static void mz_input_candidate_cb(FAR lv_event_t *event)
{
  FAR struct mz_input_candidate_event_s *candidate =
    lv_event_get_user_data(event);
  FAR struct mz_input_s *input = candidate->input;
  FAR const char *old_text;
  FAR const char *word;
  char value[MZ_INPUT_TEXT_MAX];
  size_t prefix;

  if (input == NULL || candidate->index >= MZ_INPUT_CANDIDATE_COUNT)
    {
      return;
    }

  word = input->candidates[candidate->index];
  if (word == NULL)
    {
      return;
    }

  old_text = lv_textarea_get_text(input->textarea);
  prefix = strlen(old_text);
  while (prefix > 0 && isalpha((unsigned char)old_text[prefix - 1]))
    {
      prefix--;
    }

  snprintf(value, sizeof(value), "%.*s%s", (int)prefix, old_text, word);
  lv_textarea_set_text(input->textarea, value);
  lv_textarea_set_cursor_pos(input->textarea, LV_TEXTAREA_CURSOR_LAST);
  mz_input_update_candidates(input);
}

static void mz_input_finish(FAR struct mz_input_s *input, bool submit)
{
  mz_input_submit_t submit_cb;
  mz_input_cancel_t cancel_cb;
  FAR void *context;
  char value[MZ_INPUT_TEXT_MAX];

  if (input == NULL || input->overlay == NULL || input->closing)
    {
      return;
    }

  input->closing = true;
  snprintf(value, sizeof(value), "%s",
           input->textarea == NULL ? "" :
           lv_textarea_get_text(input->textarea));
  submit_cb = input->submit;
  cancel_cb = input->cancel;
  context = input->context;

  lv_obj_delete_async(input->overlay);
  input->overlay = NULL;
  input->textarea = NULL;
  input->keyboard = NULL;
  input->candidate_row = NULL;
  input->closing = false;

  if (submit)
    {
      if (submit_cb != NULL)
        {
          submit_cb(context, value);
        }
    }
  else if (cancel_cb != NULL)
    {
      cancel_cb(context);
    }
}

static void mz_input_ready_cb(FAR lv_event_t *event)
{
  mz_input_finish(lv_event_get_user_data(event), true);
}

static void mz_input_cancel_cb(FAR lv_event_t *event)
{
  mz_input_finish(lv_event_get_user_data(event), false);
}

int mz_input_open(FAR struct mz_input_s *input, FAR lv_obj_t *parent,
                  FAR const struct mz_input_config_s *config)
{
  FAR lv_obj_t *panel;
  FAR lv_obj_t *title;
  FAR lv_obj_t *button;
  FAR lv_obj_t *label;
  size_t max_length;
  unsigned int i;

  if (input == NULL || parent == NULL || config == NULL)
    {
      return -EINVAL;
    }

  if (input->overlay != NULL)
    {
      return -EBUSY;
    }

  memset(input, 0, sizeof(*input));
  input->font = config->font == NULL ? LV_FONT_DEFAULT : config->font;
  input->submit = config->submit;
  input->cancel = config->cancel;
  input->context = config->context;
  input->pinyin = config->pinyin && config->mode == MZ_INPUT_TEXT;

  input->overlay = lv_obj_create(parent);
  lv_obj_remove_style_all(input->overlay);
  lv_obj_set_size(input->overlay, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(input->overlay, 0, 0);
  lv_obj_set_style_bg_color(input->overlay, lv_color_hex(MZ_INPUT_BG), 0);
  lv_obj_set_style_bg_opa(input->overlay, LV_OPA_70, 0);
  lv_obj_remove_flag(input->overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(input->overlay);

  panel = lv_obj_create(input->overlay);
  lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
  lv_obj_set_pos(panel, 0, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(MZ_INPUT_SURFACE), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 0, 0);
  lv_obj_set_style_pad_all(panel, 6, 0);
  lv_obj_set_style_pad_row(panel, 5, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  title = lv_label_create(panel);
  lv_label_set_text(title, config->title == NULL ? "Input" : config->title);
  lv_obj_set_width(title, LV_PCT(100));
  lv_obj_set_style_text_font(title, input->font, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(MZ_INPUT_INK), 0);

  input->textarea = lv_textarea_create(panel);
  lv_obj_set_size(input->textarea, LV_PCT(100), 42);
  lv_textarea_set_one_line(input->textarea, true);
  lv_textarea_set_text(input->textarea,
                       config->initial == NULL ? "" : config->initial);
  lv_textarea_set_placeholder_text(input->textarea,
                                   config->placeholder == NULL ? "" :
                                                                 config->placeholder);
  max_length = config->max_length == 0 ? MZ_INPUT_TEXT_MAX - 1 :
                                         config->max_length;
  if (max_length >= MZ_INPUT_TEXT_MAX)
    {
      max_length = MZ_INPUT_TEXT_MAX - 1;
    }

  lv_textarea_set_max_length(input->textarea, (uint32_t)max_length);
  lv_textarea_set_password_mode(input->textarea,
                                config->mode == MZ_INPUT_PASSWORD);
  lv_obj_set_style_text_font(input->textarea, input->font, LV_PART_MAIN);
  lv_obj_set_style_text_color(input->textarea,
                              lv_color_hex(MZ_INPUT_INK), LV_PART_MAIN);
  lv_obj_set_style_bg_color(input->textarea,
                            lv_color_hex(MZ_INPUT_FIELD), LV_PART_MAIN);
  lv_obj_set_style_border_color(input->textarea,
                                lv_color_hex(MZ_INPUT_GREEN), 0);
  lv_obj_add_event_cb(input->textarea, mz_input_value_cb,
                      LV_EVENT_VALUE_CHANGED, input);
  lv_obj_add_event_cb(input->textarea, mz_input_ready_cb,
                      LV_EVENT_READY, input);
  lv_obj_add_event_cb(input->textarea, mz_input_cancel_cb,
                      LV_EVENT_CANCEL, input);

  input->candidate_row = lv_obj_create(panel);
  lv_obj_remove_style_all(input->candidate_row);
  lv_obj_set_size(input->candidate_row, LV_PCT(100), 34);
  lv_obj_set_flex_flow(input->candidate_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(input->candidate_row, 4, 0);
  lv_obj_remove_flag(input->candidate_row, LV_OBJ_FLAG_SCROLLABLE);
  if (!input->pinyin)
    {
      lv_obj_add_flag(input->candidate_row, LV_OBJ_FLAG_HIDDEN);
    }

  for (i = 0; i < MZ_INPUT_CANDIDATE_COUNT; i++)
    {
      button = lv_button_create(input->candidate_row);
      lv_obj_set_width(button, 0);
      lv_obj_set_height(button, 30);
      lv_obj_set_flex_grow(button, 1);
      lv_obj_set_style_radius(button, 4, 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(MZ_INPUT_KEY), 0);
      lv_obj_set_style_shadow_width(button, 0, 0);
      label = lv_label_create(button);
      lv_label_set_text(label, "");
      lv_obj_set_style_text_font(label, input->font, 0);
      lv_obj_set_style_text_color(label, lv_color_hex(MZ_INPUT_INK), 0);
      lv_obj_center(label);
      input->candidate_buttons[i] = button;
      input->candidate_events[i].input = input;
      input->candidate_events[i].index = i;
      lv_obj_add_event_cb(button, mz_input_candidate_cb, LV_EVENT_CLICKED,
                          &input->candidate_events[i]);
      lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }

  input->keyboard = lv_keyboard_create(panel);
  lv_obj_set_width(input->keyboard, LV_PCT(100));
  lv_obj_set_height(input->keyboard, 0);
  lv_obj_set_flex_grow(input->keyboard, 1);
  lv_keyboard_set_textarea(input->keyboard, input->textarea);
  lv_keyboard_set_popovers(input->keyboard, false);
  lv_keyboard_set_mode(input->keyboard,
                       config->mode == MZ_INPUT_NUMBER ?
                       LV_KEYBOARD_MODE_NUMBER :
                       LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_style_text_font(input->keyboard, &lv_font_montserrat_14,
                             LV_PART_ITEMS);
  lv_obj_set_style_bg_color(input->keyboard,
                            lv_color_hex(MZ_INPUT_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_color(input->keyboard,
                            lv_color_hex(MZ_INPUT_KEY), LV_PART_ITEMS);
  lv_obj_set_style_text_color(input->keyboard,
                              lv_color_hex(MZ_INPUT_INK), LV_PART_ITEMS);
  lv_obj_set_style_border_color(input->keyboard,
                                lv_color_hex(MZ_INPUT_LINE), LV_PART_ITEMS);
  lv_obj_add_event_cb(input->keyboard, mz_input_ready_cb,
                      LV_EVENT_READY, input);
  lv_obj_add_event_cb(input->keyboard, mz_input_cancel_cb,
                      LV_EVENT_CANCEL, input);

  mz_input_update_candidates(input);
  return 0;
}

void mz_input_close(FAR struct mz_input_s *input)
{
  if (input == NULL || input->overlay == NULL)
    {
      return;
    }

  lv_obj_delete(input->overlay);
  memset(input, 0, sizeof(*input));
}

bool mz_input_is_open(FAR const struct mz_input_s *input)
{
  return input != NULL && input->overlay != NULL;
}
