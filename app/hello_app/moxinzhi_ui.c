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

#define MZ_AI_SCROLL_THRESHOLD 12
#define MZ_VOLUME_POPUP_MS     900

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
  MZ_NOTICE_WIFI,
  MZ_NOTICE_PAIRING,
  MZ_NOTICE_SETTING,
  MZ_NOTICE_ERROR,
  MZ_NOTICE_RESET
};

static void mz_ui_show_page(FAR struct mz_ui_s *ui, enum mz_page_e page);
static void mz_ui_build_shell(FAR struct mz_ui_s *ui);
static void mz_ui_question_cb(FAR lv_event_t *event);
static void mz_ui_anki_reload_cb(FAR lv_event_t *event);
static void mz_ui_update_ai(FAR struct mz_ui_s *ui, bool force_follow);
static void mz_ui_cancel_ai(FAR struct mz_ui_s *ui);
static int mz_ui_refresh_platform(FAR struct mz_ui_s *ui);

static FAR const char *mz_ui_text(FAR const struct mz_ui_s *ui,
                                  FAR const char *zh, FAR const char *en)
{
  return ui->chinese ? zh : en;
}

static uint32_t mz_ui_color(FAR const struct mz_ui_s *ui, uint32_t color)
{
  if (!ui->dark_theme)
    {
      return color;
    }

  switch (color)
    {
      case MZ_COLOR_BG:
        return 0x101815;
      case MZ_COLOR_SURFACE:
        return 0x18231f;
      case MZ_COLOR_INK:
        return 0xf0f5f2;
      case MZ_COLOR_MUTED:
        return 0xa6b2ac;
      case MZ_COLOR_LINE:
        return 0x34413b;
      case MZ_COLOR_GREEN_BG:
        return 0x193a2d;
      case MZ_COLOR_BLUE_BG:
        return 0x1c3342;
      case MZ_COLOR_AMBER_BG:
        return 0x3d2f18;
      case MZ_COLOR_RED_BG:
        return 0x3d2323;
      default:
        return color;
    }
}

static void mz_ui_base_obj(FAR struct mz_ui_s *ui, FAR lv_obj_t *obj,
                           uint32_t color)
{
  lv_obj_set_style_bg_color(obj, lv_color_hex(mz_ui_color(ui, color)), 0);
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

  lv_obj_set_style_text_color(label,
                              lv_color_hex(mz_ui_color(ui, color)), 0);
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
  mz_ui_base_obj(ui, panel, MZ_COLOR_SURFACE);
  lv_obj_set_style_radius(panel, 6, 0);
  lv_obj_set_style_border_color(panel,
                                lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)),
                                0);
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
  lv_obj_set_style_bg_color(button,
                            lv_color_hex(mz_ui_color(ui, color)), 0);
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

  lv_obj_set_style_border_color(button,
                                lv_color_hex(mz_ui_color(ui, color)), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_text_color(label,
                              lv_color_hex(mz_ui_color(ui, color)), 0);
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
      case MZ_NOTICE_WIFI:
        text = mz_ui_text(ui, "正在连接 Wi-Fi", "Wi-Fi connection started");
        color = MZ_COLOR_BLUE_BG;
        ink = MZ_COLOR_BLUE;
        break;
      case MZ_NOTICE_PAIRING:
        text = mz_ui_text(ui, "正在获取小智配对码",
                          "Requesting XiaoZhi pairing code");
        color = MZ_COLOR_BLUE_BG;
        ink = MZ_COLOR_BLUE;
        break;
      case MZ_NOTICE_SETTING:
        text = mz_ui_text(ui, "设置已更新", "Setting updated");
        break;
      case MZ_NOTICE_ERROR:
        text = mz_ui_text(ui, "操作失败或服务未接入",
                          "Operation failed or service unavailable");
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
      mz_ui_base_obj(ui, banner, color);
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

  (void)mz_model_ask(prompt->ui->model, question);
  mz_ui_update_ai(prompt->ui, true);
}

static void mz_ui_save_card_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  int ret = mz_model_save_latest(ui->model);

  if (ret < 0 && ret != -EALREADY)
    {
      if (ui->ai_save_label != NULL)
        {
          lv_label_set_text(ui->ai_save_label,
                            mz_ui_text(ui, "保存失败", "Save failed"));
        }

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

static void mz_ui_anki_reload_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  ui->notice = mz_model_reload_anki(ui->model) < 0 ?
               MZ_NOTICE_ERROR : MZ_NOTICE_SETTING;
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
      lv_obj_set_style_bg_color(
        value, lv_color_hex(mz_ui_color(ui, MZ_COLOR_BLUE_BG)), 0);
    }
}

static enum mz_ai_state_e mz_ui_ai_state(FAR const struct mz_ui_s *ui)
{
  enum mz_ai_state_e state = ui->platform_status.ai_state;

  if (ui->ai_ptt_pressed)
    {
      return ui->ai_ptt_started ? MZ_AI_LISTENING : MZ_AI_STARTING;
    }

  if (ui->model->ai_pending &&
      (state == MZ_AI_UNAVAILABLE || state == MZ_AI_READY))
    {
      return MZ_AI_THINKING;
    }

  if (ui->model->has_latest && ui->model->ai_status < 0 &&
      (state == MZ_AI_UNAVAILABLE || state == MZ_AI_ERROR))
    {
      return MZ_AI_ERROR;
    }

  if (state == MZ_AI_UNAVAILABLE)
    {
      if (ui->platform_status.wifi_state == MZ_WIFI_CONNECTING)
        {
          return MZ_AI_CONNECTING;
        }

      if (ui->platform_status.pair_state == MZ_PAIR_UNPAIRED ||
          ui->platform_status.pair_state == MZ_PAIR_REQUESTING ||
          ui->platform_status.pair_state == MZ_PAIR_CODE_READY)
        {
          return MZ_AI_PAIR_REQUIRED;
        }
    }

  return state;
}

static FAR const char *mz_ui_ai_state_text(FAR struct mz_ui_s *ui,
                                            enum mz_ai_state_e state)
{
  switch (state)
    {
      case MZ_AI_PAIR_REQUIRED:
        return mz_ui_text(ui, "需要完成小智配对", "Pair XiaoZhi first");
      case MZ_AI_CONNECTING:
        return mz_ui_text(ui, "正在连接小智", "Connecting to XiaoZhi");
      case MZ_AI_READY:
        return mz_ui_text(ui, "小智已连接", "XiaoZhi is ready");
      case MZ_AI_STARTING:
        return mz_ui_text(ui, "正在打开麦克风", "Starting microphone");
      case MZ_AI_LISTENING:
        return mz_ui_text(ui, "正在听你说话", "Listening");
      case MZ_AI_THINKING:
        return mz_ui_text(ui, "小智正在思考", "XiaoZhi is thinking");
      case MZ_AI_SPEAKING:
        return mz_ui_text(ui, "小智正在回答", "XiaoZhi is speaking");
      case MZ_AI_ERROR:
        return mz_ui_text(ui, "小智连接异常", "XiaoZhi connection error");
      default:
        return mz_ui_text(ui, "小智服务不可用", "XiaoZhi unavailable");
    }
}

static uint32_t mz_ui_ai_state_color(enum mz_ai_state_e state)
{
  switch (state)
    {
      case MZ_AI_READY:
        return MZ_COLOR_GREEN;
      case MZ_AI_CONNECTING:
      case MZ_AI_STARTING:
      case MZ_AI_THINKING:
        return MZ_COLOR_BLUE;
      case MZ_AI_LISTENING:
      case MZ_AI_ERROR:
        return MZ_COLOR_RED;
      case MZ_AI_SPEAKING:
      case MZ_AI_PAIR_REQUIRED:
        return MZ_COLOR_AMBER;
      default:
        return MZ_COLOR_MUTED;
    }
}

static uint32_t mz_ui_ai_state_background(enum mz_ai_state_e state)
{
  switch (state)
    {
      case MZ_AI_READY:
        return MZ_COLOR_GREEN_BG;
      case MZ_AI_CONNECTING:
      case MZ_AI_STARTING:
      case MZ_AI_THINKING:
        return MZ_COLOR_BLUE_BG;
      case MZ_AI_LISTENING:
      case MZ_AI_ERROR:
        return MZ_COLOR_RED_BG;
      case MZ_AI_SPEAKING:
      case MZ_AI_PAIR_REQUIRED:
        return MZ_COLOR_AMBER_BG;
      default:
        return MZ_COLOR_SURFACE;
    }
}

static FAR const char *mz_ui_ai_activity_text(FAR struct mz_ui_s *ui,
                                               enum mz_ai_state_e state)
{
  switch (state)
    {
      case MZ_AI_STARTING:
        return mz_ui_text(ui, "正在准备录音…", "Preparing microphone...");
      case MZ_AI_LISTENING:
        return ui->ai_ptt_cancelled ?
               mz_ui_text(ui, "松开将取消", "Release to cancel") :
               mz_ui_text(ui, "我在听，请继续说…", "Listening...");
      case MZ_AI_THINKING:
        return mz_ui_text(ui, "小智正在整理答案…", "Thinking...");
      case MZ_AI_SPEAKING:
        return mz_ui_text(ui, "正在播放小智的回答…", "Playing answer...");
      default:
        return mz_ui_ai_state_text(ui, state);
    }
}

static bool mz_ui_ai_ready(FAR const struct mz_ui_s *ui)
{
  return mz_ui_ai_state(ui) == MZ_AI_READY &&
         !ui->model->ai_pending && !ui->ai_ptt_pressed;
}

static void mz_ui_set_disabled(FAR lv_obj_t *obj, bool disabled)
{
  if (obj == NULL)
    {
      return;
    }

  if (disabled)
    {
      lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
  else
    {
      lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
}

static void mz_ui_volume_popup_timeout(FAR lv_timer_t *timer)
{
  FAR struct mz_ui_s *ui = lv_timer_get_user_data(timer);

  if (ui != NULL && ui->volume_popup != NULL)
    {
      lv_obj_add_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN);
    }

  lv_timer_pause(timer);
}

void mz_ui_show_volume_popup(FAR struct mz_ui_s *ui, uint8_t volume)
{
  char text[32];

  if (ui == NULL || ui->root == NULL || ui->diag.active)
    {
      return;
    }

  if (volume > 100)
    {
      volume = 100;
    }

  ui->platform_status.volume = volume;
  if (ui->page == MZ_PAGE_SETTINGS && ui->volume_slider != NULL)
    {
      lv_slider_set_value(ui->volume_slider, volume, LV_ANIM_OFF);
    }

  if (ui->volume_popup == NULL)
    {
      ui->volume_popup = lv_obj_create(ui->root);
      lv_obj_set_size(ui->volume_popup, ui->compact ? 150 : 170,
                      ui->compact ? 58 : 64);
      lv_obj_align(ui->volume_popup, LV_ALIGN_CENTER, 0, -4);
      mz_ui_base_obj(ui, ui->volume_popup, MZ_COLOR_SURFACE);
      lv_obj_set_style_radius(ui->volume_popup, 8, 0);
      lv_obj_set_style_border_color(
        ui->volume_popup,
        lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)), 0);
      lv_obj_set_style_border_width(ui->volume_popup, 1, 0);
      lv_obj_set_style_pad_all(ui->volume_popup, 8, 0);
      lv_obj_set_style_pad_row(ui->volume_popup, 6, 0);
      lv_obj_set_flex_flow(ui->volume_popup, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(ui->volume_popup, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_remove_flag(ui->volume_popup, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(ui->volume_popup, LV_OBJ_FLAG_CLICKABLE);

      ui->volume_popup_label = mz_ui_label(
        ui, ui->volume_popup, "", LV_PCT(100), MZ_COLOR_INK);
      lv_obj_set_style_text_align(ui->volume_popup_label,
                                  LV_TEXT_ALIGN_CENTER, 0);
      ui->volume_popup_bar = lv_bar_create(ui->volume_popup);
      lv_obj_set_size(ui->volume_popup_bar, LV_PCT(100), 10);
      lv_bar_set_range(ui->volume_popup_bar, 0, 100);
      lv_obj_set_style_bg_color(
        ui->volume_popup_bar,
        lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)), LV_PART_MAIN);
      lv_obj_set_style_bg_color(
        ui->volume_popup_bar,
        lv_color_hex(mz_ui_color(ui, MZ_COLOR_GREEN)),
        LV_PART_INDICATOR);
    }

  snprintf(text, sizeof(text), "%s %u%%",
           mz_ui_text(ui, "音量", "Volume"), (unsigned int)volume);
  lv_label_set_text(ui->volume_popup_label, text);
  lv_bar_set_value(ui->volume_popup_bar, volume, LV_ANIM_OFF);
  lv_obj_remove_flag(ui->volume_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ui->volume_popup);

  if (ui->volume_popup_timer == NULL)
    {
      ui->volume_popup_timer = lv_timer_create(
        mz_ui_volume_popup_timeout, MZ_VOLUME_POPUP_MS, ui);
    }

  if (ui->volume_popup_timer != NULL)
    {
      lv_timer_reset(ui->volume_popup_timer);
      lv_timer_resume(ui->volume_popup_timer);
    }
}

int mz_ui_hardware_volume_step(FAR struct mz_ui_s *ui, int delta)
{
  int value;
  int ret;

  if (ui == NULL || delta == 0)
    {
      return -EINVAL;
    }

  if (ui->platform.ops == NULL || ui->platform.ops->set_volume == NULL)
    {
      return -ENOSYS;
    }

  value = (int)ui->platform_status.volume + delta;
  if (value < 0)
    {
      value = 0;
    }
  else if (value > 100)
    {
      value = 100;
    }

  if (value != ui->platform_status.volume)
    {
      ret = ui->platform.ops->set_volume(ui->platform.context,
                                         (uint8_t)value);
      if (ret < 0)
        {
          return ret;
        }
    }

  mz_ui_show_volume_popup(ui, (uint8_t)value);
  return 0;
}

static void mz_ui_ai_scroll_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  FAR lv_obj_t *transcript = lv_event_get_current_target(event);
  lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_SCROLL_BEGIN)
    {
      ui->ai_follow_bottom = false;
    }
  else if (code == LV_EVENT_SCROLL || code == LV_EVENT_SCROLL_END)
    {
      ui->ai_scroll_y = lv_obj_get_scroll_y(transcript);
      if (code == LV_EVENT_SCROLL_END)
        {
          ui->ai_follow_bottom =
            lv_obj_get_scroll_bottom(transcript) <= MZ_AI_SCROLL_THRESHOLD;
        }
    }
}

static void mz_ui_clear_ai_refs(FAR struct mz_ui_s *ui)
{
  unsigned int i;

  ui->ai_status_bar = NULL;
  ui->ai_status_label = NULL;
  ui->ai_transcript = NULL;
  ui->ai_empty = NULL;
  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      ui->ai_prompt_buttons[i] = NULL;
    }

  ui->ai_question_panel = NULL;
  ui->ai_question_label = NULL;
  ui->ai_answer_panel = NULL;
  ui->ai_topic_label = NULL;
  ui->ai_answer_label = NULL;
  ui->ai_save_button = NULL;
  ui->ai_save_label = NULL;
  ui->ai_text_button = NULL;
  ui->ai_bottom_anchor = NULL;
}

static int mz_ui_begin_ptt(FAR struct mz_ui_s *ui)
{
  int ret;

  /* The hardware press entry point validates readiness before setting
   * ai_ptt_pressed.  Rechecking mz_ui_ai_ready() here would reject every
   * valid press because that helper requires ai_ptt_pressed to still be
   * false.
   */

  if (ui->platform.ops == NULL || ui->platform.ops->ptt_begin == NULL)
    {
      return -ENOSYS;
    }

  ret = mz_model_begin_voice(ui->model);
  if (ret < 0)
    {
      mz_model_cancel_ai(ui->model);
      return ret;
    }

  ret = ui->platform.ops->ptt_begin(ui->platform.context);
  if (ret < 0)
    {
      mz_model_cancel_ai(ui->model);
      return ret;
    }

  ui->ai_ptt_started = true;
  ui->platform_status.ai_state = MZ_AI_STARTING;
  ui->platform_status.ai_error = 0;
  ui->platform_status.ai_detail[0] = '\0';
  return 0;
}

static void mz_ui_finish_ptt(FAR struct mz_ui_s *ui, bool cancel)
{
  int ret = 0;

  if (!ui->ai_ptt_pressed)
    {
      return;
    }

  if (ui->ai_ptt_started)
    {
      if (ui->platform.ops == NULL)
        {
          ret = -ENOSYS;
        }
      else if (cancel)
        {
          ret = ui->platform.ops->ptt_cancel == NULL ? -ENOSYS :
                ui->platform.ops->ptt_cancel(ui->platform.context);
        }
      else
        {
          ret = ui->platform.ops->ptt_end == NULL ? -ENOSYS :
                ui->platform.ops->ptt_end(ui->platform.context);
        }
    }

  if (cancel || ret < 0)
    {
      mz_model_cancel_ai(ui->model);
      ui->platform_status.ai_state = ret < 0 ? MZ_AI_ERROR :
                                             MZ_AI_CONNECTING;
      ui->platform_status.ai_error = ret;
      if (ret < 0)
        {
          snprintf(ui->platform_status.ai_detail,
                   sizeof(ui->platform_status.ai_detail),
                   "PTT failed: %d", ret);
        }
      else
        {
          snprintf(ui->platform_status.ai_detail,
                   sizeof(ui->platform_status.ai_detail), "%s",
                   mz_ui_text(ui, "正在取消录音", "Cancelling voice input"));
        }
    }
  else
    {
      ui->platform_status.ai_state = MZ_AI_THINKING;
      ui->platform_status.ai_error = 0;
      ui->platform_status.ai_detail[0] = '\0';
    }

  ui->ai_ptt_pressed = false;
  ui->ai_ptt_started = false;
  ui->ai_ptt_cancelled = false;
  mz_ui_update_ai(ui, !cancel && ret >= 0);
}

int mz_ui_hardware_ptt_press(FAR struct mz_ui_s *ui)
{
  enum mz_ai_state_e state;
  bool interrupting;
  int ret;

  if (ui == NULL || ui->model == NULL)
    {
      return -EINVAL;
    }

  if (ui->ai_ptt_pressed)
    {
      return -EALREADY;
    }

  /* A hardware long press arrives independently of the 100 ms ASK-page
   * refresh cadence.  Refresh once here so a just-started TTS response is
   * recognized as interruptible instead of being rejected as stale READY.
   */

  (void)mz_ui_refresh_platform(ui);

  state = mz_ui_ai_state(ui);
  interrupting = state == MZ_AI_THINKING || state == MZ_AI_SPEAKING;
  if (state != MZ_AI_READY && !interrupting)
    {
      return -EBUSY;
    }

  if (interrupting)
    {
      /* Keep the last completed content visible, but clear the model's
       * in-flight marker so mz_model_begin_voice() can start the new turn.
       * The runtime PTT begin path is responsible for aborting playback or
       * the outstanding cloud request.
       */

      mz_model_cancel_ai(ui->model);
    }

  ui->ai_ptt_pressed = true;
  ui->ai_ptt_started = false;
  ui->ai_ptt_cancelled = false;
  ret = mz_ui_begin_ptt(ui);
  if (ret < 0)
    {
      ui->ai_ptt_pressed = false;
      ui->platform_status.ai_state = MZ_AI_ERROR;
      ui->platform_status.ai_error = ret;
      snprintf(ui->platform_status.ai_detail,
               sizeof(ui->platform_status.ai_detail),
               "PTT unavailable: %d", ret);
    }

  mz_ui_update_ai(ui, true);
  return ret;
}

void mz_ui_hardware_ptt_release(FAR struct mz_ui_s *ui, bool cancel)
{
  if (ui == NULL || !ui->ai_ptt_pressed)
    {
      return;
    }

  ui->ai_ptt_cancelled = cancel;
  mz_ui_finish_ptt(ui, cancel);
}

static void mz_ui_cancel_ai(FAR struct mz_ui_s *ui)
{
  bool active;

  if (ui == NULL || ui->model == NULL)
    {
      return;
    }

  active = ui->ai_ptt_pressed || ui->ai_ptt_started ||
           ui->model->ai_voice_pending;
  if (!active)
    {
      return;
    }

  if (ui->platform.ops != NULL && ui->platform.ops->ptt_cancel != NULL)
    {
      (void)ui->platform.ops->ptt_cancel(ui->platform.context);
    }

  mz_model_cancel_ai(ui->model);
  ui->ai_ptt_pressed = false;
  ui->ai_ptt_started = false;
  ui->ai_ptt_cancelled = false;
  if (ui->platform_status.ai_state == MZ_AI_STARTING ||
      ui->platform_status.ai_state == MZ_AI_LISTENING ||
      ui->platform_status.ai_state == MZ_AI_THINKING ||
      ui->platform_status.ai_state == MZ_AI_SPEAKING)
    {
      ui->platform_status.ai_state = MZ_AI_CONNECTING;
      ui->platform_status.ai_error = 0;
      snprintf(ui->platform_status.ai_detail,
               sizeof(ui->platform_status.ai_detail), "%s",
               mz_ui_text(ui, "正在结束语音会话",
                          "Ending voice session"));
    }
}

static void mz_ui_update_ai(FAR struct mz_ui_s *ui, bool force_follow)
{
  enum mz_ai_state_e state;
  FAR const char *answer;
  FAR const char *topic;
  uint32_t color;
  uint32_t background;
  int32_t old_scroll;
  bool show_conversation;
  bool ready;
  bool follow;
  char status[192];
  unsigned int i;

  if (ui == NULL || ui->ai_transcript == NULL ||
      ui->page != MZ_PAGE_ASK)
    {
      return;
    }

  old_scroll = lv_obj_get_scroll_y(ui->ai_transcript);
  follow = force_follow || ui->ai_follow_bottom;
  state = mz_ui_ai_state(ui);
  color = mz_ui_ai_state_color(state);
  background = mz_ui_ai_state_background(state);
  ready = mz_ui_ai_ready(ui);

  if (ui->platform_status.ai_detail[0] != '\0')
    {
      snprintf(status, sizeof(status), "%s · %s",
               mz_ui_ai_state_text(ui, state),
               ui->platform_status.ai_detail);
    }
  else if (ui->platform_status.ai_error != 0)
    {
      snprintf(status, sizeof(status), "%s (%d)",
               mz_ui_ai_state_text(ui, state),
               ui->platform_status.ai_error);
    }
  else
    {
      snprintf(status, sizeof(status), "%s",
               mz_ui_ai_state_text(ui, state));
    }

  lv_label_set_text(ui->ai_status_label, status);
  lv_obj_set_style_text_color(ui->ai_status_label,
                              lv_color_hex(mz_ui_color(ui, color)), 0);
  lv_obj_set_style_bg_color(ui->ai_status_bar,
                            lv_color_hex(mz_ui_color(ui, background)), 0);

  show_conversation = ui->model->has_latest || ui->model->ai_pending ||
                      ui->ai_ptt_pressed || state == MZ_AI_SPEAKING;
  if (show_conversation)
    {
      lv_obj_add_flag(ui->ai_empty, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(ui->ai_empty, LV_OBJ_FLAG_HIDDEN);
    }

  if (ui->model->has_latest && ui->model->latest.question[0] != '\0')
    {
      lv_label_set_text(ui->ai_question_label,
                        ui->model->latest.question);
      lv_obj_remove_flag(ui->ai_question_panel, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(ui->ai_question_panel, LV_OBJ_FLAG_HIDDEN);
    }

  if (show_conversation)
    {
      topic = ui->model->has_latest && ui->model->latest.topic[0] != '\0' ?
              ui->model->latest.topic : "XiaoZhi";
      answer = ui->model->has_latest &&
               ui->model->latest.answer[0] != '\0' ?
               ui->model->latest.answer :
               mz_ui_ai_activity_text(ui, state);
      lv_label_set_text(ui->ai_topic_label, topic);
      lv_label_set_text(ui->ai_answer_label, answer);
      lv_obj_remove_flag(ui->ai_answer_panel, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(ui->ai_answer_panel, LV_OBJ_FLAG_HIDDEN);
    }

  if (!ui->model->has_latest || ui->model->ai_pending ||
      ui->model->ai_status < 0)
    {
      lv_obj_add_flag(ui->ai_save_button, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(ui->ai_save_button, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(ui->ai_save_label,
                        ui->model->latest_saved ?
                        mz_ui_text(ui, "已保存", "Saved") :
                        mz_ui_text(ui, "保存为知识卡", "Save as card"));
      mz_ui_set_disabled(ui->ai_save_button, ui->model->latest_saved);
    }

  mz_ui_set_disabled(ui->ai_text_button, !ready);
  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      mz_ui_set_disabled(ui->ai_prompt_buttons[i], !ready);
    }

  lv_obj_update_layout(ui->ai_transcript);
  if (follow)
    {
      lv_obj_scroll_to_view(ui->ai_bottom_anchor,
                            force_follow ? LV_ANIM_ON : LV_ANIM_OFF);
    }
  else
    {
      lv_obj_scroll_to_y(ui->ai_transcript, old_scroll, LV_ANIM_OFF);
    }

  ui->ai_scroll_y = lv_obj_get_scroll_y(ui->ai_transcript);
  ui->ai_follow_bottom =
    lv_obj_get_scroll_bottom(ui->ai_transcript) <= MZ_AI_SCROLL_THRESHOLD;
}

static void mz_ui_ask(FAR struct mz_ui_s *ui)
{
  FAR lv_obj_t *role;
  int32_t saved_scroll = ui->ai_scroll_y;
  bool saved_follow = ui->ai_follow_bottom;
  unsigned int i;

  lv_obj_remove_flag(ui->content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(ui->content, 4, 0);
  lv_obj_set_style_pad_row(ui->content, 4, 0);

  ui->ai_status_bar = mz_ui_panel(ui, ui->content, 32);
  lv_obj_set_style_pad_all(ui->ai_status_bar, 3, 0);
  lv_obj_set_style_pad_column(ui->ai_status_bar, 5, 0);
  lv_obj_set_flex_flow(ui->ai_status_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ui->ai_status_bar, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ui->ai_status_label = mz_ui_label(ui, ui->ai_status_bar, "",
                                    0, MZ_COLOR_MUTED);
  lv_obj_set_flex_grow(ui->ai_status_label, 1);
  lv_label_set_long_mode(ui->ai_status_label, LV_LABEL_LONG_DOT);
  ui->ai_text_button = mz_ui_outline_button(
    ui, ui->ai_status_bar, mz_ui_text(ui, "输入", "TYPE"),
    ui->compact ? 46 : 54, MZ_COLOR_BLUE, mz_ui_question_cb, ui);
  lv_obj_set_height(ui->ai_text_button, 24);

  ui->ai_transcript = lv_obj_create(ui->content);
  lv_obj_set_width(ui->ai_transcript, LV_PCT(100));
  lv_obj_set_height(ui->ai_transcript, 0);
  lv_obj_set_flex_grow(ui->ai_transcript, 1);
  mz_ui_base_obj(ui, ui->ai_transcript, MZ_COLOR_SURFACE);
  lv_obj_set_style_radius(ui->ai_transcript, 6, 0);
  lv_obj_set_style_border_color(
    ui->ai_transcript, lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)), 0);
  lv_obj_set_style_border_width(ui->ai_transcript, 1, 0);
  lv_obj_set_style_pad_all(ui->ai_transcript, 6, 0);
  lv_obj_set_style_pad_row(ui->ai_transcript, 6, 0);
  lv_obj_set_flex_flow(ui->ai_transcript, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui->ai_transcript, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(ui->ai_transcript, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_event_cb(ui->ai_transcript, mz_ui_ai_scroll_cb,
                      LV_EVENT_ALL, ui);

  ui->ai_empty = lv_obj_create(ui->ai_transcript);
  lv_obj_remove_style_all(ui->ai_empty);
  lv_obj_set_size(ui->ai_empty, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(ui->ai_empty, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ui->ai_empty, 5, 0);
  lv_obj_remove_flag(ui->ai_empty, LV_OBJ_FLAG_SCROLLABLE);
  mz_ui_label(ui, ui->ai_empty,
              mz_ui_text(ui, "长按机身按键说话，或试试这些问题",
                         "Hold the device talk key or try a question"),
              LV_PCT(100), MZ_COLOR_MUTED);
  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      ui->ai_prompt_buttons[i] =
        mz_ui_outline_button(ui, ui->ai_empty,
                             mz_ai_service_prompt(i, ui->chinese),
                             LV_PCT(100), MZ_COLOR_BLUE,
                             mz_ui_prompt_cb, &ui->prompt_events[i]);
      lv_obj_set_height(ui->ai_prompt_buttons[i], 34);
    }

  ui->ai_question_panel = mz_ui_panel(ui, ui->ai_transcript,
                                      LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(
    ui->ai_question_panel,
    lv_color_hex(mz_ui_color(ui, MZ_COLOR_GREEN_BG)), 0);
  role = mz_ui_label(ui, ui->ai_question_panel,
                     mz_ui_text(ui, "你", "YOU"),
                     LV_PCT(100), MZ_COLOR_GREEN);
  lv_obj_set_style_text_font(role, ui->font_text, 0);
  ui->ai_question_label = mz_ui_label(ui, ui->ai_question_panel, "",
                                      LV_PCT(100), MZ_COLOR_INK);
  lv_obj_add_flag(ui->ai_question_panel, LV_OBJ_FLAG_HIDDEN);

  ui->ai_answer_panel = mz_ui_panel(ui, ui->ai_transcript,
                                    LV_SIZE_CONTENT);
  ui->ai_topic_label = mz_ui_label(ui, ui->ai_answer_panel, "XiaoZhi",
                                   LV_PCT(100), MZ_COLOR_BLUE);
  ui->ai_answer_label = mz_ui_label(ui, ui->ai_answer_panel, "",
                                    LV_PCT(100), MZ_COLOR_MUTED);
  ui->ai_save_button = mz_ui_button(
    ui, ui->ai_answer_panel,
    mz_ui_text(ui, "保存为知识卡", "Save as card"),
    LV_PCT(100), MZ_COLOR_GREEN, mz_ui_save_card_cb, ui);
  ui->ai_save_label = lv_obj_get_child(ui->ai_save_button, 0);
  lv_obj_add_flag(ui->ai_answer_panel, LV_OBJ_FLAG_HIDDEN);

  ui->ai_bottom_anchor = lv_obj_create(ui->ai_transcript);
  lv_obj_remove_style_all(ui->ai_bottom_anchor);
  lv_obj_set_size(ui->ai_bottom_anchor, LV_PCT(100), 1);
  lv_obj_remove_flag(ui->ai_bottom_anchor, LV_OBJ_FLAG_SCROLLABLE);

  ui->ai_follow_bottom = saved_follow;
  mz_ui_update_ai(ui, saved_follow);
  if (!saved_follow && saved_scroll > 0)
    {
      lv_obj_scroll_to_y(ui->ai_transcript, saved_scroll, LV_ANIM_OFF);
      ui->ai_scroll_y = lv_obj_get_scroll_y(ui->ai_transcript);
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
  lv_obj_set_style_bg_color(bar,
                            lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)),
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

static void mz_ui_copy(FAR char *target, size_t size,
                       FAR const char *source)
{
  if (size > 0)
    {
      snprintf(target, size, "%s", source == NULL ? "" : source);
    }
}

static void mz_ui_platform_defaults(FAR struct mz_ui_s *ui)
{
  memset(&ui->platform_status, 0, sizeof(ui->platform_status));
  ui->platform_status.wifi_state = MZ_WIFI_UNAVAILABLE;
  ui->platform_status.pair_state = MZ_PAIR_UNAVAILABLE;
  ui->platform_status.volume = 50;
  ui->platform_status.brightness = 70;
  ui->platform_status.chinese = ui->chinese;
  ui->platform_status.dark_theme = false;
  mz_ui_copy(ui->platform_status.model,
             sizeof(ui->platform_status.model), "Gemini-S1");
  snprintf(ui->platform_status.firmware,
           sizeof(ui->platform_status.firmware), "%s %s", __DATE__,
           __TIME__);
}

static int mz_ui_refresh_platform(FAR struct mz_ui_s *ui)
{
  struct mz_platform_status_s status;
  int ret;

  if (ui->platform.ops == NULL || ui->platform.ops->refresh == NULL)
    {
      return -ENOSYS;
    }

  status = ui->platform_status;
  ret = ui->platform.ops->refresh(ui->platform.context, &status);
  if (ret < 0)
    {
      return ret;
    }

  if (status.volume > 100)
    {
      status.volume = 100;
    }

  if (status.brightness > 100)
    {
      status.brightness = 100;
    }

  if (!ui->chinese_available)
    {
      status.chinese = false;
    }

  ui->platform_status = status;
  ui->chinese = status.chinese;
  ui->dark_theme = status.dark_theme;
  if (ui->wifi_ssid[0] == '\0' && status.wifi_ssid[0] != '\0')
    {
      mz_ui_copy(ui->wifi_ssid, sizeof(ui->wifi_ssid), status.wifi_ssid);
    }

  return 0;
}

static FAR const char *mz_ui_wifi_state(FAR struct mz_ui_s *ui)
{
  switch (ui->platform_status.wifi_state)
    {
      case MZ_WIFI_DISCONNECTED:
        return mz_ui_text(ui, "未连接", "Disconnected");
      case MZ_WIFI_CONNECTING:
        return mz_ui_text(ui, "连接中", "Connecting");
      case MZ_WIFI_CONNECTED:
        return mz_ui_text(ui, "已连接", "Connected");
      case MZ_WIFI_ERROR:
        return mz_ui_text(ui, "连接失败", "Connection error");
      default:
        return mz_ui_text(ui, "服务未接入", "Service unavailable");
    }
}

static uint32_t mz_ui_wifi_color(FAR const struct mz_ui_s *ui)
{
  switch (ui->platform_status.wifi_state)
    {
      case MZ_WIFI_CONNECTED:
        return MZ_COLOR_GREEN;
      case MZ_WIFI_CONNECTING:
        return MZ_COLOR_BLUE;
      case MZ_WIFI_ERROR:
        return MZ_COLOR_RED;
      default:
        return MZ_COLOR_MUTED;
    }
}

static FAR const char *mz_ui_pair_state(FAR struct mz_ui_s *ui)
{
  switch (ui->platform_status.pair_state)
    {
      case MZ_PAIR_UNPAIRED:
        return mz_ui_text(ui, "未配对", "Not paired");
      case MZ_PAIR_REQUESTING:
        return mz_ui_text(ui, "获取配对码中", "Requesting code");
      case MZ_PAIR_CODE_READY:
        return mz_ui_text(ui, "等待配对", "Waiting for pairing");
      case MZ_PAIR_PAIRED:
        return mz_ui_text(ui, "已配对", "Paired");
      case MZ_PAIR_ERROR:
        return mz_ui_text(ui, "配对失败", "Pairing error");
      default:
        return mz_ui_text(ui, "服务未接入", "Service unavailable");
    }
}

static uint32_t mz_ui_pair_color(FAR const struct mz_ui_s *ui)
{
  switch (ui->platform_status.pair_state)
    {
      case MZ_PAIR_PAIRED:
        return MZ_COLOR_GREEN;
      case MZ_PAIR_REQUESTING:
      case MZ_PAIR_CODE_READY:
        return MZ_COLOR_BLUE;
      case MZ_PAIR_ERROR:
        return MZ_COLOR_RED;
      default:
        return MZ_COLOR_MUTED;
    }
}

static void mz_ui_ssid_submit(FAR void *context, FAR const char *value)
{
  FAR struct mz_ui_s *ui = context;

  mz_ui_copy(ui->wifi_ssid, sizeof(ui->wifi_ssid), value);
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_password_submit(FAR void *context, FAR const char *value)
{
  FAR struct mz_ui_s *ui = context;

  mz_ui_copy(ui->wifi_password, sizeof(ui->wifi_password), value);
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static int mz_ui_request_wifi_scan(FAR struct mz_ui_s *ui)
{
  int ret;

  if (ui->platform_status.wifi_scan_state == MZ_WIFI_SCANNING)
    {
      ui->wifi_scan_requested = true;
      return 0;
    }

  if (ui->platform.ops == NULL || ui->platform.ops->wifi_scan == NULL)
    {
      ret = -ENOSYS;
    }
  else
    {
      ret = ui->platform.ops->wifi_scan(ui->platform.context);
    }

  ui->wifi_scan_requested = true;
  ui->platform_status.wifi_network_count = 0;
  ui->platform_status.wifi_scan_error = ret;
  ui->platform_status.wifi_scan_state = ret < 0 ? MZ_WIFI_SCAN_ERROR :
                                                   MZ_WIFI_SCANNING;
  return ret;
}

static void mz_ui_wifi_scan_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  if (mz_ui_request_wifi_scan(ui) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
    }

  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_wifi_network_cb(FAR lv_event_t *event)
{
  FAR struct mz_wifi_event_s *selection = lv_event_get_user_data(event);
  FAR struct mz_ui_s *ui = selection->ui;
  FAR const char *ssid;

  if (selection->index >= ui->platform_status.wifi_network_count)
    {
      return;
    }

  ssid = ui->platform_status.wifi_networks[selection->index].ssid;
  if (strcmp(ui->wifi_ssid, ssid) != 0)
    {
      mz_ui_copy(ui->wifi_ssid, sizeof(ui->wifi_ssid), ssid);
      ui->wifi_password[0] = '\0';
    }

  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_question_submit(FAR void *context,
                                  FAR const char *value)
{
  FAR struct mz_ui_s *ui = context;

  mz_ui_copy(ui->ask_question, sizeof(ui->ask_question), value);
  if (ui->ask_question[0] == '\0')
    {
      return;
    }

  (void)mz_model_ask(ui->model, ui->ask_question);
  mz_ui_update_ai(ui, true);
}

static void mz_ui_open_input(FAR struct mz_ui_s *ui,
                             enum mz_input_mode_e mode,
                             FAR const char *title,
                             FAR const char *placeholder,
                             FAR const char *initial,
                             size_t max_length,
                             mz_input_submit_t submit)
{
  struct mz_input_config_s config;

  memset(&config, 0, sizeof(config));
  config.title = title;
  config.placeholder = placeholder;
  config.initial = initial;
  config.font = ui->font_text;
  config.mode = mode;
  config.max_length = max_length;
  config.pinyin = ui->chinese_available && mode == MZ_INPUT_TEXT;
  config.submit = submit;
  config.context = ui;
  if (mz_input_open(&ui->input, ui->root, &config) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
    }
}

static void mz_ui_ssid_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_ui_open_input(ui, MZ_INPUT_TEXT,
                   mz_ui_text(ui, "输入 Wi-Fi 名称", "Enter Wi-Fi SSID"),
                   "SSID", ui->wifi_ssid, MZ_PLATFORM_SSID_MAX - 1,
                   mz_ui_ssid_submit);
}

static void mz_ui_password_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_ui_open_input(ui, MZ_INPUT_PASSWORD,
                   mz_ui_text(ui, "输入 Wi-Fi 密码",
                              "Enter Wi-Fi password"),
                   mz_ui_text(ui, "密码", "Password"),
                   ui->wifi_password, MZ_PLATFORM_PASSWORD_MAX - 1,
                   mz_ui_password_submit);
}

static void mz_ui_question_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  mz_ui_open_input(ui, MZ_INPUT_TEXT,
                   mz_ui_text(ui, "输入问题", "Enter a question"),
                   mz_ui_text(ui, "想问小智什么？",
                              "What would you like to ask?"),
                   ui->ask_question, MZ_QUESTION_MAX_CHARS,
                   mz_ui_question_submit);
}

static void mz_ui_wifi_connect_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  int ret;

  if (ui->wifi_ssid[0] == '\0')
    {
      mz_ui_ssid_cb(event);
      return;
    }

  if (ui->platform.ops == NULL ||
      ui->platform.ops->wifi_connect == NULL)
    {
      ui->notice = MZ_NOTICE_ERROR;
    }
  else
    {
      ret = ui->platform.ops->wifi_connect(ui->platform.context,
                                           ui->wifi_ssid,
                                           ui->wifi_password);
      if (ret < 0)
        {
          ui->notice = MZ_NOTICE_ERROR;
          ui->platform_status.wifi_state = MZ_WIFI_ERROR;
        }
      else
        {
          ui->notice = MZ_NOTICE_WIFI;
          ui->platform_status.wifi_state = MZ_WIFI_CONNECTING;
        }
    }

  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_volume_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  uint8_t value = (uint8_t)lv_slider_get_value(
    lv_event_get_current_target(event));

  if (ui->platform.ops != NULL && ui->platform.ops->set_volume != NULL &&
      ui->platform.ops->set_volume(ui->platform.context, value) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  ui->platform_status.volume = value;
  mz_ui_show_volume_popup(ui, value);
}

static void mz_ui_brightness_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);
  uint8_t value = (uint8_t)lv_slider_get_value(
    lv_event_get_current_target(event));

  if (ui->platform.ops != NULL &&
      ui->platform.ops->set_brightness != NULL &&
      ui->platform.ops->set_brightness(ui->platform.context, value) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  ui->platform_status.brightness = value;
}

static void mz_ui_language_cb(FAR lv_event_t *event)
{
  FAR struct mz_setting_event_s *setting = lv_event_get_user_data(event);
  FAR struct mz_ui_s *ui = setting->ui;
  bool chinese = setting->value != 0;

  if (chinese && !ui->chinese_available)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  if (ui->platform.ops != NULL &&
      ui->platform.ops->set_language != NULL &&
      ui->platform.ops->set_language(ui->platform.context, chinese) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  ui->chinese = chinese;
  ui->platform_status.chinese = chinese;
  ui->notice = MZ_NOTICE_SETTING;
  mz_ui_build_shell(ui);
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_theme_cb(FAR lv_event_t *event)
{
  FAR struct mz_setting_event_s *setting = lv_event_get_user_data(event);
  FAR struct mz_ui_s *ui = setting->ui;
  bool dark_theme = setting->value != 0;

  if (ui->platform.ops != NULL && ui->platform.ops->set_theme != NULL &&
      ui->platform.ops->set_theme(ui->platform.context, dark_theme) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  ui->dark_theme = dark_theme;
  ui->platform_status.dark_theme = dark_theme;
  ui->notice = MZ_NOTICE_SETTING;
  mz_ui_build_shell(ui);
  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_pair_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  if (ui->platform.ops == NULL ||
      ui->platform.ops->request_pairing == NULL ||
      ui->platform.ops->request_pairing(ui->platform.context) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
      ui->platform_status.pair_state = MZ_PAIR_ERROR;
    }
  else
    {
      ui->notice = MZ_NOTICE_PAIRING;
      ui->platform_status.pair_state = MZ_PAIR_REQUESTING;
      ui->platform_status.pair_code[0] = '\0';
    }

  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static void mz_ui_forget_pair_cb(FAR lv_event_t *event)
{
  FAR struct mz_ui_s *ui = lv_event_get_user_data(event);

  if (ui->platform.ops == NULL ||
      ui->platform.ops->forget_pairing == NULL ||
      ui->platform.ops->forget_pairing(ui->platform.context) < 0)
    {
      ui->notice = MZ_NOTICE_ERROR;
    }
  else
    {
      ui->notice = MZ_NOTICE_SETTING;
      ui->platform_status.pair_state = MZ_PAIR_UNPAIRED;
      ui->platform_status.pair_code[0] = '\0';
    }

  mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
}

static FAR lv_obj_t *mz_ui_choice_button(
  FAR struct mz_ui_s *ui, FAR lv_obj_t *row, FAR const char *text,
  bool selected, lv_event_cb_t callback, FAR void *user_data)
{
  FAR lv_obj_t *button;

  button = selected ?
           mz_ui_button(ui, row, text, 0, MZ_COLOR_GREEN,
                        callback, user_data) :
           mz_ui_outline_button(ui, row, text, 0, MZ_COLOR_GREEN,
                                callback, user_data);
  lv_obj_set_flex_grow(button, 1);
  return button;
}

static void mz_ui_setting_slider(FAR struct mz_ui_s *ui,
                                 FAR lv_obj_t *panel,
                                 FAR const char *label_zh,
                                 FAR const char *label_en,
                                 uint8_t value,
                                 FAR lv_obj_t **target,
                                 lv_event_cb_t callback)
{
  FAR lv_obj_t *row;
  FAR lv_obj_t *label;
  FAR lv_obj_t *slider;
  char text[16];

  row = mz_ui_row(panel, ui->compact ? 34 : 38);
  label = mz_ui_label(ui, row, mz_ui_text(ui, label_zh, label_en),
                      ui->compact ? 48 : 60, MZ_COLOR_INK);
  (void)label;
  slider = lv_slider_create(row);
  lv_obj_set_width(slider, 0);
  lv_obj_set_flex_grow(slider, 1);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider,
                            lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider,
                            lv_color_hex(MZ_COLOR_GREEN),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider,
                            lv_color_hex(MZ_COLOR_GREEN), LV_PART_KNOB);
  lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, ui);
  snprintf(text, sizeof(text), "%u%%", (unsigned int)value);
  mz_ui_label(ui, row, text, ui->compact ? 34 : 40, MZ_COLOR_MUTED);
  *target = slider;
}

static void mz_ui_settings(FAR struct mz_ui_s *ui)
{
  struct mz_stats_s stats;
  bool previous_chinese = ui->chinese;
  bool previous_dark = ui->dark_theme;
  FAR lv_obj_t *panel;
  FAR lv_obj_t *row;
  FAR lv_obj_t *label;
  FAR lv_obj_t *button;
  char status[128];
  char network[96];
  char secret[24];
  unsigned int i;

  mz_model_get_stats(ui->model, &stats);
  mz_ui_refresh_platform(ui);
  if (previous_chinese != ui->chinese || previous_dark != ui->dark_theme)
    {
      mz_ui_build_shell(ui);
      mz_ui_show_page(ui, MZ_PAGE_SETTINGS);
      return;
    }

  if (!ui->wifi_scan_requested)
    {
      (void)mz_ui_request_wifi_scan(ui);
    }

  mz_ui_title(ui, "设置", "Settings", NULL, NULL);

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  row = mz_ui_row(panel, ui->compact ? 24 : 28);
  label = mz_ui_label(ui, row, "Wi-Fi", 0, MZ_COLOR_INK);
  lv_obj_set_flex_grow(label, 1);
  mz_ui_label(ui, row, mz_ui_wifi_state(ui), 0, mz_ui_wifi_color(ui));
  if (ui->platform_status.wifi_ssid[0] != '\0' ||
      ui->platform_status.wifi_ip[0] != '\0')
    {
      snprintf(status, sizeof(status), "%s%s%s",
               ui->platform_status.wifi_ssid,
               ui->platform_status.wifi_ssid[0] != '\0' &&
               ui->platform_status.wifi_ip[0] != '\0' ? "  " : "",
               ui->platform_status.wifi_ip);
      mz_ui_label(ui, panel, status, LV_PCT(100), MZ_COLOR_MUTED);
    }

  row = mz_ui_row(panel, ui->compact ? 34 : 38);
  label = mz_ui_label(ui, row,
                      mz_ui_text(ui, "附近网络", "Nearby networks"),
                      0, MZ_COLOR_INK);
  lv_obj_set_flex_grow(label, 1);
  if (ui->platform_status.wifi_scan_state == MZ_WIFI_SCANNING)
    {
      mz_ui_label(ui, row, mz_ui_text(ui, "扫描中", "Scanning..."),
                  0, MZ_COLOR_BLUE);
    }

  mz_ui_outline_button(ui, row, LV_SYMBOL_REFRESH,
                       ui->compact ? 36 : 40, MZ_COLOR_BLUE,
                       mz_ui_wifi_scan_cb, ui);
  if (ui->platform_status.wifi_scan_state == MZ_WIFI_SCAN_ERROR)
    {
      snprintf(status, sizeof(status), "%s (%d)",
               mz_ui_text(ui, "扫描失败", "Scan failed"),
               ui->platform_status.wifi_scan_error);
      mz_ui_label(ui, panel, status, LV_PCT(100), MZ_COLOR_RED);
    }
  else if (ui->platform_status.wifi_scan_state == MZ_WIFI_SCAN_READY &&
           ui->platform_status.wifi_network_count == 0)
    {
      mz_ui_label(ui, panel,
                  mz_ui_text(ui, "未找到网络", "No networks found"),
                  LV_PCT(100), MZ_COLOR_MUTED);
    }

  for (i = 0; i < ui->platform_status.wifi_network_count; i++)
    {
      FAR const struct mz_wifi_network_s *entry =
        &ui->platform_status.wifi_networks[i];

      snprintf(network, sizeof(network), "%s  %d dBm%s",
               entry->ssid, entry->rssi,
               entry->secured ? "  secured" : "");
      ui->wifi_events[i].ui = ui;
      ui->wifi_events[i].index = i;
      button = mz_ui_outline_button(
        ui, panel, network, LV_PCT(100),
        strcmp(ui->wifi_ssid, entry->ssid) == 0 ? MZ_COLOR_GREEN :
                                                  MZ_COLOR_BLUE,
        mz_ui_wifi_network_cb, &ui->wifi_events[i]);
      label = lv_obj_get_child(button, 0);
      lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    }

  button = mz_ui_outline_button(
    ui, panel,
    ui->wifi_ssid[0] == '\0' ?
    mz_ui_text(ui, "输入 Wi-Fi 名称", "Enter Wi-Fi SSID") :
    ui->wifi_ssid,
    LV_PCT(100), MZ_COLOR_BLUE, mz_ui_ssid_cb, ui);
  lv_obj_set_style_text_align(lv_obj_get_child(button, 0),
                              LV_TEXT_ALIGN_LEFT, 0);
  snprintf(secret, sizeof(secret), "%s",
           ui->wifi_password[0] == '\0' ?
           mz_ui_text(ui, "输入密码", "Enter password") : "********");
  button = mz_ui_outline_button(ui, panel, secret, LV_PCT(100),
                                MZ_COLOR_BLUE, mz_ui_password_cb, ui);
  lv_obj_set_style_text_align(lv_obj_get_child(button, 0),
                              LV_TEXT_ALIGN_LEFT, 0);
  mz_ui_button(ui, panel, mz_ui_text(ui, "连接", "Connect"),
               LV_PCT(100), MZ_COLOR_GREEN, mz_ui_wifi_connect_cb, ui);

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  row = mz_ui_row(panel, ui->compact ? 24 : 28);
  label = mz_ui_label(ui, row,
                      mz_ui_text(ui, "小智官方服务", "XiaoZhi official service"),
                      0, MZ_COLOR_INK);
  lv_obj_set_flex_grow(label, 1);
  mz_ui_label(ui, row, mz_ui_pair_state(ui), 0, mz_ui_pair_color(ui));
  if (ui->platform_status.pair_code[0] != '\0')
    {
      label = mz_ui_label(ui, panel, ui->platform_status.pair_code,
                          LV_PCT(100), MZ_COLOR_BLUE);
      lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      mz_ui_label(ui, panel,
                  mz_ui_text(ui, "请在小智官方页面输入此配对码",
                             "Enter this code on the official XiaoZhi page"),
                  LV_PCT(100), MZ_COLOR_MUTED);
    }

  if (ui->platform_status.pair_state == MZ_PAIR_PAIRED)
    {
      if (ui->platform_status.device_id[0] != '\0')
        {
          snprintf(status, sizeof(status), "%s: %s",
                   mz_ui_text(ui, "设备 ID", "Device ID"),
                   ui->platform_status.device_id);
          mz_ui_label(ui, panel, status, LV_PCT(100), MZ_COLOR_MUTED);
        }

      mz_ui_outline_button(ui, panel,
                           mz_ui_text(ui, "解除配对", "Forget pairing"),
                           LV_PCT(100), MZ_COLOR_RED,
                           mz_ui_forget_pair_cb, ui);
    }
  else
    {
      mz_ui_button(ui, panel,
                   ui->platform_status.pair_state == MZ_PAIR_CODE_READY ?
                   mz_ui_text(ui, "刷新配对码", "Refresh pairing code") :
                   mz_ui_text(ui, "获取配对码", "Get pairing code"),
                   LV_PCT(100), MZ_COLOR_BLUE, mz_ui_pair_cb, ui);
    }

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  mz_ui_label(ui, panel, mz_ui_text(ui, "显示与声音", "Display & sound"),
              LV_PCT(100), MZ_COLOR_INK);
  mz_ui_setting_slider(ui, panel, "音量", "Volume",
                       ui->platform_status.volume, &ui->volume_slider,
                       mz_ui_volume_cb);
  mz_ui_setting_slider(ui, panel, "亮度", "Brightness",
                       ui->platform_status.brightness,
                       &ui->brightness_slider, mz_ui_brightness_cb);

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  mz_ui_label(ui, panel, mz_ui_text(ui, "语言与主题", "Language & theme"),
              LV_PCT(100), MZ_COLOR_INK);
  row = mz_ui_row(panel, ui->compact ? 36 : 40);
  button = mz_ui_choice_button(ui, row, "中文", ui->chinese,
                               mz_ui_language_cb,
                               &ui->language_events[1]);
  if (!ui->chinese_available)
    {
      lv_obj_add_state(button, LV_STATE_DISABLED);
    }

  mz_ui_choice_button(ui, row, "English", !ui->chinese,
                      mz_ui_language_cb, &ui->language_events[0]);
  row = mz_ui_row(panel, ui->compact ? 36 : 40);
  mz_ui_choice_button(ui, row, mz_ui_text(ui, "浅色", "Light"),
                      !ui->dark_theme, mz_ui_theme_cb,
                      &ui->theme_events[0]);
  mz_ui_choice_button(ui, row, mz_ui_text(ui, "深色", "Dark"),
                      ui->dark_theme, mz_ui_theme_cb,
                      &ui->theme_events[1]);

  panel = mz_ui_panel(ui, ui->content, ui->compact ? 86 : 96);
  mz_ui_label(ui, panel, mz_ui_text(ui, "每日目标", "Daily card goal"),
              LV_PCT(100), MZ_COLOR_INK);
  row = mz_ui_row(panel, ui->compact ? 36 : 40);
  mz_ui_goal_button(ui, row, 3);
  mz_ui_goal_button(ui, row, 5);
  mz_ui_goal_button(ui, row, 10);

  panel = mz_ui_panel(ui, ui->content, LV_SIZE_CONTENT);
  mz_ui_label(ui, panel, mz_ui_text(ui, "设备信息", "Device information"),
              LV_PCT(100), MZ_COLOR_INK);
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
  snprintf(status, sizeof(status), "%s  %s",
           ui->platform_status.model[0] == '\0' ? "Gemini-S1" :
                                                  ui->platform_status.model,
           ui->platform_status.firmware);
  mz_ui_label(ui, panel, status, LV_PCT(100), MZ_COLOR_MUTED);
  snprintf(status, sizeof(status), "Anki: %u%s  /data/import",
           (unsigned int)stats.imported_cards,
           stats.imported_truncated ? "+" : "");
  mz_ui_label(ui, panel, status, LV_PCT(100),
              ui->model->anki_status < 0 ? MZ_COLOR_RED : MZ_COLOR_MUTED);
  mz_ui_outline_button(ui, panel,
                       mz_ui_text(ui, "刷新 Anki 卡库",
                                  "Reload Anki cards"),
                       LV_PCT(100), MZ_COLOR_BLUE,
                       mz_ui_anki_reload_cb, ui);

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
  enum mz_page_e previous_page;
  unsigned int i;

  if (ui->content == NULL || page >= MZ_PAGE_COUNT)
    {
      return;
    }

  if (page == MZ_PAGE_ASK && ui->page == MZ_PAGE_ASK &&
      ui->ai_transcript != NULL)
    {
      mz_ui_update_ai(ui, false);
      return;
    }

  previous_page = ui->page;
  if (page == MZ_PAGE_SETTINGS && previous_page != MZ_PAGE_SETTINGS)
    {
      ui->wifi_scan_requested = false;
    }

  if (previous_page == MZ_PAGE_ASK && ui->ai_transcript != NULL)
    {
      ui->ai_scroll_y = lv_obj_get_scroll_y(ui->ai_transcript);
      ui->ai_follow_bottom =
        lv_obj_get_scroll_bottom(ui->ai_transcript) <=
        MZ_AI_SCROLL_THRESHOLD;
    }

  if (previous_page == MZ_PAGE_ASK && page != MZ_PAGE_ASK)
    {
      mz_ui_cancel_ai(ui);
    }

  mz_ui_clear_ai_refs(ui);
  ui->page = page;
  ui->volume_slider = NULL;
  ui->brightness_slider = NULL;
  lv_obj_clean(ui->content);
  lv_obj_add_flag(ui->content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(ui->content, ui->compact ? 7 : 10, 0);
  lv_obj_set_style_pad_row(ui->content, ui->compact ? 6 : 8, 0);
  for (i = 0; i < MZ_PAGE_COUNT; i++)
    {
      lv_obj_set_style_bg_color(ui->nav_buttons[i],
                                lv_color_hex(mz_ui_color(
                                  ui, i == page ? MZ_COLOR_GREEN_BG :
                                                  MZ_COLOR_SURFACE)), 0);
      lv_obj_set_style_text_color(lv_obj_get_child(ui->nav_buttons[i], 0),
                                  lv_color_hex(mz_ui_color(
                                    ui, i == page ? MZ_COLOR_GREEN :
                                                    MZ_COLOR_MUTED)), 0);
    }

  if (page == MZ_PAGE_ASK)
    {
      ui->notice = MZ_NOTICE_NONE;
    }
  else
    {
      mz_ui_notice(ui);
    }

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

  if (page != MZ_PAGE_ASK)
    {
      lv_obj_scroll_to_y(ui->content, 0, LV_ANIM_OFF);
    }
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
  lv_obj_set_style_bg_color(
    button, lv_color_hex(mz_ui_color(ui, MZ_COLOR_SURFACE)), 0);
  lv_label_set_text(label, symbol);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(
    label, lv_color_hex(mz_ui_color(ui, MZ_COLOR_MUTED)), 0);
  lv_obj_center(label);
  ui->nav_events[page].ui = ui;
  ui->nav_events[page].page = page;
  lv_obj_add_event_cb(button, mz_ui_nav_cb, LV_EVENT_CLICKED,
                      &ui->nav_events[page]);
  return button;
}

static void mz_ui_build_shell(FAR struct mz_ui_s *ui)
{
  FAR lv_obj_t *nav;
  int32_t width = lv_display_get_horizontal_resolution(ui->display);
  int32_t height = lv_display_get_vertical_resolution(ui->display);
  int32_t nav_height = ui->compact ? 38 : 44;
  unsigned int i;
  static const FAR char *g_nav_symbols[MZ_PAGE_COUNT] =
  {
    LV_SYMBOL_HOME, "AI", LV_SYMBOL_LIST,
    LV_SYMBOL_CHARGE, LV_SYMBOL_SETTINGS
  };

  if (ui->page == MZ_PAGE_ASK)
    {
      if (ui->ai_transcript != NULL)
        {
          ui->ai_scroll_y = lv_obj_get_scroll_y(ui->ai_transcript);
          ui->ai_follow_bottom =
            lv_obj_get_scroll_bottom(ui->ai_transcript) <=
            MZ_AI_SCROLL_THRESHOLD;
        }

      mz_ui_cancel_ai(ui);
    }

  mz_ui_clear_ai_refs(ui);
  ui->root = lv_screen_active();
  if (ui->volume_popup_timer != NULL)
    {
      lv_timer_pause(ui->volume_popup_timer);
    }

  ui->volume_popup = NULL;
  ui->volume_popup_label = NULL;
  ui->volume_popup_bar = NULL;
  ui->volume_slider = NULL;
  ui->brightness_slider = NULL;
  lv_obj_clean(ui->root);
  mz_ui_base_obj(ui, ui->root, MZ_COLOR_BG);
  lv_obj_remove_flag(ui->root, LV_OBJ_FLAG_SCROLLABLE);

  ui->content = lv_obj_create(ui->root);
  lv_obj_set_pos(ui->content, 0, 0);
  lv_obj_set_size(ui->content, width, height - nav_height);
  mz_ui_base_obj(ui, ui->content, MZ_COLOR_BG);
  lv_obj_set_style_pad_all(ui->content, ui->compact ? 7 : 10, 0);
  lv_obj_set_style_pad_row(ui->content, ui->compact ? 6 : 8, 0);
  lv_obj_set_flex_flow(ui->content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ui->content, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(ui->content, LV_SCROLLBAR_MODE_AUTO);

  nav = lv_obj_create(ui->root);
  lv_obj_set_pos(nav, 0, height - nav_height);
  lv_obj_set_size(nav, width, nav_height);
  mz_ui_base_obj(ui, nav, MZ_COLOR_SURFACE);
  lv_obj_set_style_border_color(
    nav, lv_color_hex(mz_ui_color(ui, MZ_COLOR_LINE)), 0);
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

int mz_ui_init_with_platform(
  FAR struct mz_ui_s *ui, FAR lv_display_t *display,
  FAR struct mz_model_s *model, bool touch_available,
  FAR const struct mz_platform_binding_s *platform,
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
  ui->ai_follow_bottom = true;
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
          ui->chinese_available = true;
        }
    }
#endif

  mz_ui_platform_defaults(ui);
  if (platform != NULL)
    {
      ui->platform = *platform;
      mz_ui_refresh_platform(ui);
    }

  for (i = 0; i < MZ_PROMPT_COUNT; i++)
    {
      ui->prompt_events[i].ui = ui;
      ui->prompt_events[i].index = i;
    }

  for (i = 0; i < 2; i++)
    {
      ui->language_events[i].ui = ui;
      ui->language_events[i].value = i;
      ui->theme_events[i].ui = ui;
      ui->theme_events[i].value = i;
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

int mz_ui_init(FAR struct mz_ui_s *ui, FAR lv_display_t *display,
               FAR struct mz_model_s *model, bool touch_available,
               bool start_diagnostic)
{
  return mz_ui_init_with_platform(ui, display, model, touch_available,
                                  NULL, start_diagnostic);
}

void mz_ui_bind_platform(
  FAR struct mz_ui_s *ui,
  FAR const struct mz_platform_binding_s *platform)
{
  if (ui == NULL)
    {
      return;
    }

  if (platform == NULL)
    {
      memset(&ui->platform, 0, sizeof(ui->platform));
    }
  else
    {
      ui->platform = *platform;
      mz_ui_refresh_platform(ui);
    }

  if (!ui->diag.active && ui->display != NULL)
    {
      mz_ui_build_shell(ui);
      mz_ui_show_page(ui, ui->page);
    }
}

void mz_ui_platform_changed(FAR struct mz_ui_s *ui)
{
  struct mz_platform_status_s previous_status;
  bool previous_chinese;
  bool previous_dark;
  bool shell_changed;
  bool status_changed;
  int32_t scroll_y = 0;

  if (ui == NULL)
    {
      return;
    }

  previous_status = ui->platform_status;
  previous_chinese = ui->chinese;
  previous_dark = ui->dark_theme;
  if (mz_ui_refresh_platform(ui) < 0 || ui->diag.active)
    {
      return;
    }

  status_changed = memcmp(&previous_status, &ui->platform_status,
                          sizeof(previous_status)) != 0;
  if (!status_changed)
    {
      return;
    }

  shell_changed = previous_chinese != ui->chinese ||
                  previous_dark != ui->dark_theme;
  if (!shell_changed && ui->page == MZ_PAGE_ASK)
    {
      mz_ui_update_ai(ui, false);
      return;
    }

  if (!shell_changed && ui->content != NULL)
    {
      scroll_y = lv_obj_get_scroll_y(ui->content);
    }

  if (shell_changed)
    {
      mz_ui_build_shell(ui);
    }

  if (ui->content != NULL)
    {
      mz_ui_show_page(ui, ui->page);
      if (!shell_changed && ui->page == MZ_PAGE_SETTINGS)
        {
          lv_obj_scroll_to_y(ui->content, scroll_y, LV_ANIM_OFF);
        }
    }
}

void mz_ui_model_changed(FAR struct mz_ui_s *ui)
{
  if (ui == NULL || ui->diag.active || ui->content == NULL)
    {
      return;
    }

  if (ui->page == MZ_PAGE_ASK)
    {
      mz_ui_update_ai(ui, false);
      return;
    }

  if (mz_input_is_open(&ui->input))
    {
      return;
    }

  if (ui->page == MZ_PAGE_HOME || ui->page == MZ_PAGE_CARDS ||
      ui->page == MZ_PAGE_STATS)
    {
      mz_ui_show_page(ui, ui->page);
    }
}

void mz_ui_deinit(FAR struct mz_ui_s *ui)
{
  if (ui == NULL)
    {
      return;
    }

  mz_ui_cancel_ai(ui);
  mz_diag_stop(&ui->diag);
  mz_input_close(&ui->input);
  if (ui->volume_popup_timer != NULL)
    {
      lv_timer_delete(ui->volume_popup_timer);
      ui->volume_popup_timer = NULL;
    }

  if (ui->root != NULL)
    {
      lv_obj_clean(ui->root);
    }

  ui->volume_popup = NULL;
  ui->volume_popup_label = NULL;
  ui->volume_popup_bar = NULL;

#if LV_USE_FREETYPE
  if (ui->font_cn != NULL)
    {
      lv_freetype_font_delete(ui->font_cn);
      ui->font_cn = NULL;
    }
#endif
}
