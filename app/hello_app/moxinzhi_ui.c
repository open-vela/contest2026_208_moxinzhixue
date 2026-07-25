/****************************************************************************
 * Contest 2026 team 208 - product UI
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "moxinzhi_storage.h"
#include "moxinzhi_ui.h"

#define MZ_COLOR_BG       0xf4f5f1
#define MZ_COLOR_SURFACE  0xffffff
#define MZ_COLOR_INK      0x17201d
#define MZ_COLOR_MUTED    0x66706b
#define MZ_COLOR_LINE     0xdde2dc
#define MZ_COLOR_GREEN    0x177a57
#define MZ_COLOR_GREEN_BG 0xe3f1e9
#define MZ_COLOR_BLUE     0x356d91
#define MZ_COLOR_BLUE_BG  0xe4eef5
#define MZ_COLOR_AMBER    0xd28a22
#define MZ_COLOR_AMBER_BG 0xf8edd7
#define MZ_COLOR_RED      0xb94a48
#define MZ_COLOR_RED_BG   0xf8e5e4

#ifndef CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_FONT_PATH
#  define MZ_FONT_PATH "/resource/fonts/MiSans-Normal.ttf"
#else
#  define MZ_FONT_PATH \
     CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_FONT_PATH
#endif

enum mz_notice_e
{
  MZ_NOTICE_NONE = 0,
  MZ_NOTICE_SAVED,
  MZ_NOTICE_MASTERED,
  MZ_NOTICE_REVIEW,
  MZ_NOTICE_ERROR,
  MZ_NOTICE_RESET
};

static void mz_ui_show_page(FAR struct mz_ui_s *ui, enum mz_page_e page);
static void mz_ui_build_shell(FAR struct mz_ui_s *ui);

static FAR const char *mz_ui_text(FAR const struct mz_ui_s *ui,
                                  FAR const char *zh, FAR const char *en)
{
  return ui->chinese ? zh : en;
}

static void mz_ui_base_obj(FAR lv_obj_t *obj, uint32_t color)
{
  lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_set_style_shadow_width(obj, 0, 0);
}

static FAR lv_obj_t *mz_ui_label(FAR struct mz_ui_s *ui,
                                 FAR lv_obj_t *parent,
                                 FAR const char *text, int32_t width,
                                 uint32_t color)
{
  FAR lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  if (width != 0)
    {
      lv_obj_set_width(label, width);
    }

  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(label, ui->font_text, 0);
  lv_obj_set_style_text_line_space(label, 3, 0);
  return label;
}

static FAR lv_obj_t *mz_ui_panel(FAR struct mz_ui_s *ui,
                                 FAR lv_obj_t *parent, int32_t height)
{
  FAR lv_obj_t *panel = lv_obj_create(parent);

  lv_obj_set_width(panel, LV_PCT(100));
  lv_obj_set_height(panel, height);
  mz_ui_base_obj(panel, MZ_COLOR_SURFACE);
  lv_obj_set_style_radius(panel, 6, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(MZ_COLOR_LINE), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_pad_all(panel, ui->compact ? 8 : 10, 0);
  lv_obj_set_style_pad_row(panel, 6, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  return panel;
}

static FAR lv_obj_t *mz_ui_row(FAR lv_obj_t *parent, int32_t height)
{
  FAR lv_obj_t *row = lv_obj_create(parent);

  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, height);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

static FAR lv_obj_t *mz_ui_button(FAR struct mz_ui_s *ui,
                                  FAR lv_obj_t *parent,
                                  FAR const char *text, int32_t width,
                                  uint32_t color, lv_event_cb_t callback,
                                  FAR void *user_data)
{
  FAR lv_obj_t *button = lv_button_create(parent);
  FAR lv_obj_t *label;

  lv_obj_set_width(button, width);
  lv_obj_set_height(button, ui->compact ? 36 : 40);
  lv_obj_set_style_radius(button, 5, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_hor(button, 8, 0);
  label = mz_ui_label(ui, button, text, LV_PCT(100), 0xffffff);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    }

  return button;
}

static FAR lv_obj_t *mz_ui_outline_button(FAR struct mz_ui_s *ui,
                                          FAR lv_obj_t *parent,
                                          FAR const char *text,
                                          int32_t width, uint32_t color,
                                          lv_event_cb_t callback,
                                          FAR void *user_data)
{
  FAR lv_obj_t *button = mz_ui_button(ui, parent, text, width,
                                      MZ_COLOR_SURFACE, callback, user_data);
  FAR lv_obj_t *label = lv_obj_get_child(button, 0);

  lv_obj_set_style_border_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return button;
}

static void mz_ui_title(FAR struct mz_ui_s *ui, FAR const char *zh,
                        FAR const char *en, FAR const char *sub_zh,
                        FAR const char *sub_en)
{
  FAR lv_obj_t *title = mz_ui_label(ui, ui->content,
                                    mz_ui_text(ui, zh, en),
                                    LV_PCT(100), MZ_COLOR_INK);

  lv_obj_set_style_text_font(title,
                             ui->chinese ? ui->font_text :
                             &lv_font_montserrat_20, 0);
  if (sub_zh != NULL && sub_en != NULL)
    {
      mz_ui_label(ui, ui->content, mz_ui_text(ui, sub_zh, sub_en),
                  LV_PCT(100), MZ_COLOR_MUTED);
    }
}

static void mz_ui_notice(FAR struct mz_ui_s *ui)
{
  FAR const char *text = NULL;
  uint32_t color = MZ_COLOR_GREEN_BG;
  uint32_t ink = MZ_COLOR_GREEN;
  FAR lv_obj_t *banner;

  switch (ui->notice)
    {
      case MZ_NOTICE_SAVED:
        text = mz_ui_text(ui, "知识卡已保存", "Knowledge card saved");
        break;
      case MZ_NOTICE_MASTERED:
        text = mz_ui_text(ui, "已标记掌握", "Marked as mastered");
        break;
      case MZ_NOTICE_REVIEW:
        text = mz_ui_text(ui, "已加入复习", "Added to review");
        color = MZ_COLOR_AMBER_BG;
        ink = MZ_COLOR_AMBER;
        break;
      case MZ_NOTICE_ERROR:
        text = mz_ui_text(ui, "本地保存失败", "Local save failed");
        color = MZ_COLOR_RED_BG;
        ink = MZ_COLOR_RED;
        break;
      case MZ_NOTICE_RESET:
        text = mz_ui_text(ui, "学习数据已清空", "Learning data reset");
        break;
      default:
        break;
    }

  if (text != NULL)
    {
      banner = lv_obj_create(ui->content);
      lv_obj_set_size(banner, LV_PCT(100), ui->compact ? 30 : 34);
      mz_ui_base_obj(banner, color);
      lv_obj_set_style_radius(banner, 5, 0);
      lv_obj_set_style_pad_all(banner, 7, 0);
      lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
      mz_ui_label(ui, banner, text, LV_PCT(100), ink);
    }

  ui->notice = MZ_NOTICE_NONE;
}

static void mz_ui_nav_cb(FAR lv_event_t *event)
{
  FAR struct mz_nav_event_s *nav = lv_event_get_user_data(event);

  nav->ui->reset_confirm = false;
  mz_ui_show_page(nav->ui, nav->page);
}

static void mz_ui_go_ask_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_ui_show_page(ui, MZ_PAGE_ASK);
}

static void mz_ui_go_cards_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_ui_show_page(ui, MZ_PAGE_CARDS);
}

static void mz_ui_prompt_cb(FAR lv_event_t *event)
{
  FAR struct mz_prompt_event_s *prompt = lv_event_get_user_data(event);
  FAR const char *question = mz_ai_service_prompt(prompt->index,
                                                  prompt->ui->chinese);

  if (mz_model_ask(prompt->ui->model, question) < 0)
    {
      prompt->ui->notice = MZ_NOTICE_ERROR;
    }

  mz_ui_show_page(prompt->ui, MZ_PAGE_ASK);
}

static void mz_ui_save_card_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  int ret = mz_model_save_latest(ui->model);

  if (ret < 0 && ret != -EALREADY)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_ASK);
      return;
    }

  ui->notice = MZ_NOTICE_SAVED;
  mz_ui_show_page(ui, MZ_PAGE_CARDS);
}

static void mz_ui_master_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->notice = mz_model_rate_current(ui->model, true) < 0 ?
               MZ_NOTICE_ERROR : MZ_NOTICE_MASTERED;
  mz_ui_show_page(ui, MZ_PAGE_CARDS);
}

static void mz_ui_review_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->notice = mz_model_rate_current(ui->model, false) < 0 ?
               MZ_NOTICE_ERROR : MZ_NOTICE_REVIEW;
  mz_ui_show_page(ui, MZ_PAGE_CARDS);
}

static void mz_ui_next_card_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_model_advance_card(ui->model);
  mz_ui_show_page(ui, MZ_PAGE_CARDS);
}

static void mz_ui_goal_cb(FAR lv_event_t *event)
{
  FAR lv_obj_t *button = lv_event_get_current_target(event);
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  uint8_t goal = (uint8_t)(uintptr_t)lv_obj_get_user_data(button);

  ui->notice = mz_model_set_daily_goal(ui->model, goal) < 0 ?
               MZ_NOTICE_ERROR : MZ_NOTICE_NONE;
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_diag_back_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_diag_stop(&ui->diag);
  mz_ui_build_shell(ui);
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_diag_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  if (mz_diag_start(&ui->diag, ui->display, ui->touch_available,
                    mz_ui_diag_back_cb, ui) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_build_shell(ui);
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
    }
}

static void mz_ui_reset_begin_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->reset_confirm = true;
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_reset_cancel_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->reset_confirm = false;
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_reset_confirm_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->notice = mz_model_reset(ui->model) < 0 ?
               MZ_NOTICE_ERROR : MZ_NOTICE_RESET;
  ui->reset_confirm = false;
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_metric(FAR struct mz_ui_s *ui, FAR lv_obj_t *row,
                         FAR const char *label_zh, FAR const char *label_en,
                         unsigned long value, uint32_t color)
{
  FAR lv_obj_t *panel = mz_ui_panel(ui, row, ui->compact ? 58 : 66);
  FAR lv_obj_t *number;

  lv_obj_set_width(panel, 0);
  lv_obj_set_flex_grow(panel, 1);
  number = mz_ui_label(ui, panel, "", LV_PCT(100), color);
  lv_label_set_text_fmt(number, "%lu", value);
  lv_obj_set_style_text_font(number, &lv_font_montserrat_20, 0);
  mz_ui_label(ui, panel, mz_ui_text(ui, label_zh, label_en),
              LV_PCT(100), MZ_COLOR_MUTED);
}

static void mz_ui_home(FAR struct mz_ui_s *ui)
{
  struct mz_stats_s stats;
  FAR lv_obj_t *hero;
  FAR lv_obj_t *row;
  FAR lv_obj_t *value;

  mz_model_get_stats(ui->model, &stats);
  mz_ui_title(ui, "今天学点什么？", "What will you learn?",
              "墨芯智学 · 纯本地学习", "MOXIN · ON-DEVICE LEARNING");

  hero = mz_ui_panel(ui, ui->content, ui->compact ? 88 : 104);
  mz_ui_label(ui, hero,
              mz_ui_text(ui, "从一个好问题开始", "Start with a good question"),
              LV_PCT(100), MZ_COLOR_INK);
  mz_ui_button(ui, hero, mz_ui_text(ui, "开始 AI 问答", "Open AI Q&A"),
               LV_PCT(100), MZ_COLOR_GREEN, mz_ui_go_ask_cb, ui);

  row = mz_ui_row(ui->content, ui->compact ? 58 : 66);
  mz_ui_metric(ui, row, "知识卡", "Cards", stats.active_cards,
               MZ_COLOR_BLUE);
  mz_ui_metric(ui, row, "已掌握", "Mastered", stats.mastered_cards,
               MZ_COLOR_GREEN);

  if (stats.active_cards > 0)
    {
      value = mz_ui_outline_button(ui, ui->content,
                                   mz_ui_text(ui, "继续学习知识卡",
                                              "Continue knowledge cards"),
                                   LV_PCT(100), MZ_COLOR_BLUE,
                                   mz_ui_go_cards_cb, ui);
      lv_obj_set_style_bg_color(value, lv_color_hex(MZ_COLOR_BLUE_BG), 0);
    }
}

static void mz_ui_ask(FAR struct mz_ui_s *ui)
{
  FAR lv_obj_t *button;
  FAR lv_obj_t *answer;
  FAR lv_obj_t *tag;
  unsigned int i;

  mz_ui_title(ui, "AI 问答", "AI Q&A", "今日问题", "TODAY'S QUESTIONS");
  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      button = mz_ui_outline_button(ui, ui->content,
                                    mz_ai_service_prompt(i, ui->chinese),
                                    LV_PCT(100), MZ_COLOR_BLUE,
                                    mz_ui_prompt_cb,
                                    &ui->prompt_events[i]);
      lv_obj_set_height(button, ui->compact ? 40 : 44);
    }

  if (!ui->model->has_latest)
    {
      return;
    }

  answer = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  tag = mz_ui_label(ui, answer, ui->model->latest.topic,
                    LV_PCT(100), MZ_COLOR_BLUE);
  lv_obj_set_style_text_font(tag, ui->font_text, 0);
  mz_ui_label(ui, answer, ui->model->latest.question,
              LV_PCT(100), MZ_COLOR_INK);
  mz_ui_label(ui, answer, ui->model->latest.answer,
              LV_PCT(100), MZ_COLOR_MUTED);
  button = mz_ui_button(ui, answer,
                        ui->model->latest_saved ?
                        mz_ui_text(ui, "已保存", "Saved") :
                        mz_ui_text(ui, "保存为知识卡", "Save as card"),
                        LV_PCT(100), MZ_COLOR_GREEN,
                        mz_ui_save_card_cb, ui);
  if (ui->model->latest_saved)
    {
      lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static FAR const char *mz_ui_card_state(FAR struct mz_ui_s *ui,
                                        uint8_t state)
{
  if (state == MZ_CARD_MASTERED)
    {
      return mz_ui_text(ui, "已掌握", "MASTERED");
    }

  if (state == MZ_CARD_REVIEW)
    {
      return mz_ui_text(ui, "待复习", "REVIEW");
    }

  return mz_ui_text(ui, "新卡片", "NEW CARD");
}

static void mz_ui_cards(FAR struct mz_ui_s *ui)
{
  FAR const struct mz_card_s *card = mz_model_current_card(ui->model);
  FAR lv_obj_t *panel;
  FAR lv_obj_t *row;
  FAR lv_obj_t *label;

  mz_ui_title(ui, "知识卡", "Knowledge cards", NULL, NULL);
  if (card == NULL)
    {
      panel = mz_ui_panel(ui, ui->content, ui->compact ? 92 : 112);
      mz_ui_label(ui, panel,
                  mz_ui_text(ui, "还没有知识卡", "No knowledge cards yet"),
                  LV_PCT(100), MZ_COLOR_MUTED);
      mz_ui_button(ui, panel, mz_ui_text(ui, "去问一个问题", "Ask a question"),
                   LV_PCT(100), MZ_COLOR_GREEN, mz_ui_go_ask_cb, ui);
      return;
    }

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  row = mz_ui_row(panel, ui->compact ? 24 : 28);
  label = mz_ui_label(ui, row, card->topic, 0, MZ_COLOR_BLUE);
  lv_obj_set_flex_grow(label, 1);
  mz_ui_label(ui, row, mz_ui_card_state(ui, card->state),
              0, card->state == MZ_CARD_REVIEW ? MZ_COLOR_AMBER :
                                                 MZ_COLOR_GREEN);
  mz_ui_label(ui, panel, card->question, LV_PCT(100), MZ_COLOR_INK);
  mz_ui_label(ui, panel, card->answer, LV_PCT(100), MZ_COLOR_MUTED);

  row = mz_ui_row(ui->content, ui->compact ? 38 : 42);
  mz_ui_button(ui, row, mz_ui_text(ui, "掌握", "Mastered"),
               0, MZ_COLOR_GREEN, mz_ui_master_cb, ui);
  lv_obj_set_flex_grow(lv_obj_get_child(row, 0), 1);
  mz_ui_button(ui, row, mz_ui_text(ui, "未掌握", "Review"),
               0, MZ_COLOR_AMBER, mz_ui_review_cb, ui);
  lv_obj_set_flex_grow(lv_obj_get_child(row, 1), 1);
  mz_ui_outline_button(ui, row, LV_SYMBOL_RIGHT, 38, MZ_COLOR_BLUE,
                       mz_ui_next_card_cb, ui);
}

static void mz_ui_stats(FAR struct mz_ui_s *ui)
{
  struct mz_stats_s stats;
  FAR lv_obj_t *panel;
  FAR lv_obj_t *row;
  FAR lv_obj_t *bar;
  FAR lv_obj_t *label;
  unsigned long percent;

  mz_model_get_stats(ui->model, &stats);
  percent = stats.active_cards == 0 ? 0 :
            (unsigned long)stats.mastered_cards * 100 / stats.active_cards;
  mz_ui_title(ui, "学习统计", "Learning stats", NULL, NULL);

  panel = mz_ui_panel(ui, ui->content, ui->compact ? 70 : 82);
  row = mz_ui_row(panel, 24);
  label = mz_ui_label(ui, row,
                      mz_ui_text(ui, "卡片掌握度", "Card mastery"),
                      0, MZ_COLOR_INK);
  lv_obj_set_flex_grow(label, 1);
  label = mz_ui_label(ui, row, "", 0, MZ_COLOR_GREEN);
  lv_label_set_text_fmt(label, "%lu%%", percent);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  bar = lv_bar_create(panel);
  lv_obj_set_size(bar, LV_PCT(100), 10);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, percent, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(MZ_COLOR_LINE),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(MZ_COLOR_GREEN),
                            LV_PART_INDICATOR);

  row = mz_ui_row(ui->content, ui->compact ? 58 : 66);
  mz_ui_metric(ui, row, "提问", "Questions", stats.ask_count,
               MZ_COLOR_BLUE);
  mz_ui_metric(ui, row, "保存", "Saved", stats.saved_total,
               MZ_COLOR_AMBER);
  row = mz_ui_row(ui->content, ui->compact ? 58 : 66);
  mz_ui_metric(ui, row, "掌握次数", "Master actions",
               stats.mastered_actions, MZ_COLOR_GREEN);
  mz_ui_metric(ui, row, "复习次数", "Review actions",
               stats.review_actions, MZ_COLOR_RED);
}

static void mz_ui_goal_button(FAR struct mz_ui_s *ui, FAR lv_obj_t *row,
                              uint8_t goal)
{
  FAR lv_obj_t *button;
  char text[8];

  snprintf(text, sizeof(text), "%u", (unsigned int)goal);
  if (ui->model->meta.daily_goal == goal)
    {
      button = mz_ui_button(ui, row, text, 0, MZ_COLOR_GREEN,
                            mz_ui_goal_cb, ui);
    }
  else
    {
      button = mz_ui_outline_button(ui, row, text, 0, MZ_COLOR_GREEN,
                                    mz_ui_goal_cb, ui);
    }

  lv_obj_set_user_data(button, (FAR void *)(uintptr_t)goal);
  lv_obj_set_flex_grow(button, 1);
}

static void mz_ui_settings(FAR struct mz_ui_s *ui)
{
  FAR lv_obj_t *panel;
  FAR lv_obj_t *row;
  FAR lv_obj_t *label;
  char status[96];

  mz_ui_title(ui, "设置", "Settings", NULL, NULL);
  panel = mz_ui_panel(ui, ui->content, ui->compact ? 86 : 96);
  mz_ui_label(ui, panel, mz_ui_text(ui, "每日目标", "Daily card goal"),
              LV_PCT(100), MZ_COLOR_INK);
  row = mz_ui_row(panel, ui->compact ? 36 : 40);
  mz_ui_goal_button(ui, row, 3);
  mz_ui_goal_button(ui, row, 5);
  mz_ui_goal_button(ui, row, 10);

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  snprintf(status, sizeof(status), "%s  %s",
           ui->model->storage_status < 0 ? "ERROR" : "KVDB",
           mz_storage_backend_path());
  mz_ui_label(ui, panel, status, LV_PCT(100),
              ui->model->storage_status < 0 ? MZ_COLOR_RED :
                                              MZ_COLOR_MUTED);
  snprintf(status, sizeof(status), "%s: %s   %s: %s",
           mz_ui_text(ui, "触摸", "Touch"),
           ui->touch_available ? "OK" : "N/A",
           mz_ui_text(ui, "界面", "UI"),
           ui->chinese ? "ZH" : "EN");
  mz_ui_label(ui, panel, status, LV_PCT(100), MZ_COLOR_MUTED);

  mz_ui_outline_button(ui, ui->content,
                       mz_ui_text(ui, "硬件诊断", "Hardware diagnostics"),
                       LV_PCT(100), MZ_COLOR_BLUE, mz_ui_diag_cb, ui);

  if (!ui->reset_confirm)
    {
      mz_ui_outline_button(ui, ui->content,
                           mz_ui_text(ui, "清空学习数据", "Reset learning data"),
                           LV_PCT(100), MZ_COLOR_RED,
                           mz_ui_reset_begin_cb, ui);
    }
  else
    {
      panel = mz_ui_panel(ui, ui->content, ui->compact ? 80 : 90);
      mz_ui_label(ui, panel,
                  mz_ui_text(ui, "确认清空全部本地学习数据？",
                             "Reset all local learning data?"),
                  LV_PCT(100), MZ_COLOR_RED);
      row = mz_ui_row(panel, ui->compact ? 36 : 40);
      mz_ui_outline_button(ui, row, mz_ui_text(ui, "取消", "Cancel"),
                           0, MZ_COLOR_MUTED, mz_ui_reset_cancel_cb, ui);
      lv_obj_set_flex_grow(lv_obj_get_child(row, 0), 1);
      mz_ui_button(ui, row, mz_ui_text(ui, "确认清空", "Reset"),
                   0, MZ_COLOR_RED, mz_ui_reset_confirm_cb, ui);
      lv_obj_set_flex_grow(lv_obj_get_child(row, 1), 1);
    }
}

static void mz_ui_show_page(FAR struct mz_ui_s *ui, enum mz_page_e page)
{
  unsigned int i;

  if (ui->content == NULL || page >= MZ_PAGE_COUNT)
    {
      return;
    }

  ui->page = page;
  lv_obj_clean(ui->content);
  for (i = 0; i < MZ_PAGE_COUNT; i++)
    {
      lv_obj_set_style_bg_color(ui->nav_buttons[i],
                                lv_color_hex(i == page ?
                                             MZ_COLOR_GREEN_BG :
                                             MZ_COLOR_SURFACE), 0);
      lv_obj_set_style_text_color(lv_obj_get_child(ui->nav_buttons[i], 0),
                                  lv_color_hex(i == page ?
                                               MZ_COLOR_GREEN :
                                               MZ_COLOR_MUTED), 0);
    }

  mz_ui_notice(ui);
  switch (page)
    {
      case MZ_PAGE_HOME:
        mz_ui_home(ui);
        break;
      case MZ_PAGE_ASK:
        mz_ui_ask(ui);
        break;
      case MZ_PAGE_CARDS:
        mz_ui_cards(ui);
        break;
      case MZ_PAGE_STATS:
        mz_ui_stats(ui);
        break;
      case MZ_PAGE_SETTINGS:
        mz_ui_settings(ui);
        break;
      default:
        break;
    }

  lv_obj_scroll_to_y(ui->content, 0, LV_ANIM_OFF);
}

static FAR lv_obj_t *mz_ui_nav_button(FAR struct mz_ui_s *ui,
                                      FAR lv_obj_t *nav,
                                      FAR const char *symbol,
                                      enum mz_page_e page)
{
  FAR lv_obj_t *button = lv_button_create(nav);
  FAR lv_obj_t *label = lv_label_create(button);

  lv_obj_set_width(button, 0);
  lv_obj_set_height(button, LV_PCT(100));
  lv_obj_set_flex_grow(button, 1);
  lv_obj_set_style_radius(button, 4, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(MZ_COLOR_SURFACE), 0);
  lv_label_set_text(label, symbol);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(MZ_COLOR_MUTED), 0);
  lv_obj_center(label);
  ui->nav_events[page].ui = ui;
  ui->nav_events[page].page = page;
  lv_obj_add_event_cb(button, mz_ui_nav_cb, LV_EVENT_CLICKED,
                      &ui->nav_events[page]);
  return button;
}

static void mz_ui_build_shell(FAR struct mz_ui_s *ui)
{
  FAR lv_obj_t *header;
  FAR lv_obj_t *brand;
  FAR lv_obj_t *local;
  FAR lv_obj_t *nav;
  int32_t width = lv_display_get_horizontal_resolution(ui->display);
  int32_t height = lv_display_get_vertical_resolution(ui->display);
  int32_t header_height = ui->compact ? 32 : 38;
  int32_t nav_height = ui->compact ? 38 : 44;
  unsigned int i;
  static const FAR char *g_nav_symbols[MZ_PAGE_COUNT] =
  {
    LV_SYMBOL_HOME, "AI", LV_SYMBOL_LIST,
    LV_SYMBOL_CHARGE, LV_SYMBOL_SETTINGS
  };

  ui->root = lv_screen_active();
  lv_obj_clean(ui->root);
  mz_ui_base_obj(ui->root, MZ_COLOR_BG);
  lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

  header = lv_obj_create(ui->root);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_size(header, width, header_height);
  mz_ui_base_obj(header, MZ_COLOR_SURFACE);
  lv_obj_set_style_border_color(header, lv_color_hex(MZ_COLOR_LINE), 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_pad_hor(header, 10, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  brand = mz_ui_label(ui, header,
                      mz_ui_text(ui, "墨芯智学", "MOXIN LEARN"),
                      0, MZ_COLOR_INK);
  lv_obj_set_flex_grow(brand, 1);
  lv_obj_set_style_text_font(brand,
                             ui->chinese ? ui->font_text :
                             &lv_font_montserrat_16, 0);
  local = mz_ui_label(ui, header,
                      mz_ui_text(ui, "本地", "LOCAL"),
                      0, MZ_COLOR_GREEN);
  lv_obj_set_style_text_font(local,
                             ui->chinese ? ui->font_text :
                             &lv_font_montserrat_12, 0);

  ui->content = lv_obj_create(ui->root);
  lv_obj_set_pos(ui->content, 0, header_height);
  lv_obj_set_size(ui->content, width, height - header_height - nav_height);
  mz_ui_base_obj(ui->content, MZ_COLOR_BG);
  lv_obj_set_style_pad_all(ui->content, ui->compact ? 7 : 10, 0);
  lv_obj_set_style_pad_row(ui->content, ui->compact ? 6 : 8, 0);
  lv_obj_set_flex_flow(ui->content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui->content, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(ui->content, LV_SCROLLBAR_MODE_AUTO);

  nav = lv_obj_create(ui->root);
  lv_obj_set_pos(nav, 0, height - nav_height);
  lv_obj_set_size(nav, width, nav_height);
  mz_ui_base_obj(nav, MZ_COLOR_SURFACE);
  lv_obj_set_style_border_color(nav, lv_color_hex(MZ_COLOR_LINE), 0);
  lv_obj_set_style_border_width(nav, 1, 0);
  lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_pad_all(nav, 3, 0);
  lv_obj_set_style_pad_column(nav, 3, 0);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
  for (i = 0; i < MZ_PAGE_COUNT; i++)
    {
      ui->nav_buttons[i] = mz_ui_nav_button(ui, nav, g_nav_symbols[i], i);
    }
}

int mz_ui_init(FAR struct mz_ui_s *ui, FAR lv_display_t *display,
               FAR struct mz_model_s *model, bool touch_available,
               bool start_diagnostic)
{
  int32_t height;
  unsigned int i;

  if (ui == NULL || display == NULL || model == NULL)
    {
      return -EINVAL;
    }

  memset(ui, 0, sizeof(*ui));
  ui->display = display;
  ui->model = model;
  ui->touch_available = touch_available;
  height = lv_display_get_vertical_resolution(display);
  ui->compact = height <= 240;
  ui->font_text = LV_FONT_DEFAULT;

#if LV_USE_FREETYPE
  if (access(MZ_FONT_PATH, R_OK) == 0)
    {
      ui->font_cn = lv_freetype_font_create(
        MZ_FONT_PATH, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
        LV_FREETYPE_FONT_STYLE_NORMAL);
      if (ui->font_cn != NULL)
        {
          ui->font_text = ui->font_cn;
          ui->chinese = true;
        }
    }
#endif

  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      ui->prompt_events[i].ui = ui;
      ui->prompt_events[i].index = i;
    }

  if (start_diagnostic)
    {
      return mz_diag_start(&ui->diag, display, touch_available,
                           mz_ui_diag_back_cb, ui);
    }

  mz_ui_build_shell(ui);
  mz_ui_show_page(ui, MZ_PAGE_HOME);
  return 0;
}

void mz_ui_deinit(FAR struct mz_ui_s *ui)
{
  if (ui == NULL)
    {
      return;
    }

  mz_diag_stop(&ui->diag);
  if (ui->root != NULL)
    {
      lv_obj_clean(ui->root);
    }

#if LV_USE_FREETYPE
  if (ui->font_cn != NULL)
    {
      lv_freetype_font_delete(ui->font_cn);
      ui->font_cn = NULL;
    }
#endif
}
