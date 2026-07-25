/****************************************************************************
 * Contest 2026 team 208 - MoXin Smart Learning P0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <lvgl/lvgl.h>

#include "moxinzhi_model.h"
#include "moxinzhi_ui.h"

#define MZ_LOOP_MAX_MS 20

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
#  define MZ_INPUT_DEVPATH \
     CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_INPUT_DEVPATH
#endif

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

struct mz_options_s
{
  uint32_t timeout_ms;
  bool diagnostic;
};

static void mz_usage(FAR const char *program)
{
  printf("Usage: %s [-d|--diagnostic] [-t seconds]\n", program);
}

static int mz_parse_options(int argc, FAR char *argv[],
                            FAR struct mz_options_s *options)
{
  FAR char *endptr;
  unsigned long seconds;
  int i;

  memset(options, 0, sizeof(*options));
  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
          mz_usage(argv[0]);
          return 1;
        }

      if (strcmp(argv[i], "-d") == 0 ||
          strcmp(argv[i], "--diagnostic") == 0)
        {
          options->diagnostic = true;
          continue;
        }

      if (strcmp(argv[i], "-t") != 0 || i + 1 >= argc)
        {
          fprintf(stderr, "lvgl_screen_test: invalid argument: %s\n",
                  argv[i]);
          mz_usage(argv[0]);
          return -EINVAL;
        }

      errno = 0;
      seconds = strtoul(argv[++i], &endptr, 10);
      if (errno != 0 || endptr == argv[i] || *endptr != '\0' ||
          seconds == 0 || seconds > UINT32_MAX / 1000)
        {
          fprintf(stderr, "lvgl_screen_test: invalid timeout: %s\n",
                  argv[i]);
          return -EINVAL;
        }

      options->timeout_ms = (uint32_t)seconds * 1000;
    }

  return 0;
}

int main(int argc, FAR char *argv[])
{
  struct mz_options_s options;
  struct mz_model_s model;
  struct mz_ui_s ui;
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  uint32_t start_ms;
  uint32_t elapsed_ms;
  uint32_t idle_ms;
  bool touch_available = false;
  int ret;

  ret = mz_parse_options(argc, argv, &options);
  if (ret != 0)
    {
      return ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  if (lv_is_initialized())
    {
      fprintf(stderr, "lvgl_screen_test: LVGL is already initialized\n");
      return EXIT_FAILURE;
    }

#ifdef NEED_BOARDINIT
  boardctl(BOARDIOC_INIT, 0);
#endif

  memset(&result, 0, sizeof(result));
  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/lcd0";

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  info.input_path = MZ_INPUT_DEVPATH;
#endif

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "lvgl_screen_test: failed to open /dev/lcd0\n");
      lv_nuttx_deinit(&result);
      lv_deinit();
      return EXIT_FAILURE;
    }

#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  touch_available = result.indev != NULL;
  if (!touch_available)
    {
      fprintf(stderr, "lvgl_screen_test: failed to open %s\n",
              MZ_INPUT_DEVPATH);
    }
#endif

  ret = mz_model_init(&model);
  if (ret < 0)
    {
      fprintf(stderr, "lvgl_screen_test: KVDB load failed: %d\n", ret);
    }

  ret = mz_ui_init(&ui, result.disp, &model, touch_available,
                   options.diagnostic);
  if (ret < 0)
    {
      fprintf(stderr, "lvgl_screen_test: UI creation failed: %d\n", ret);
      lv_nuttx_deinit(&result);
      lv_deinit();
      return EXIT_FAILURE;
    }

  printf("lvgl_screen_test: MoXin UI on /dev/lcd0");
#ifdef CONFIG_LV_USE_NUTTX_TOUCHSCREEN
  if (touch_available)
    {
      printf(" with %s", MZ_INPUT_DEVPATH);
    }
  else
    {
      printf(" without touchscreen");
    }
#endif
  printf(options.diagnostic ? " [diagnostic]\n" : " [product]\n");

  start_ms = lv_tick_get();
  for (;;)
    {
      idle_ms = lv_timer_handler();
      if (idle_ms == 0)
        {
          idle_ms = 1;
        }
      else if (idle_ms > MZ_LOOP_MAX_MS)
        {
          idle_ms = MZ_LOOP_MAX_MS;
        }

      if (options.timeout_ms != 0)
        {
          elapsed_ms = (uint32_t)(lv_tick_get() - start_ms);
          if (elapsed_ms >= options.timeout_ms)
            {
              break;
            }
        }

      usleep(idle_ms * 1000);
    }

  mz_ui_deinit(&ui);
  lv_nuttx_deinit(&result);
  lv_deinit();
  printf("lvgl_screen_test: complete\n");
  return EXIT_SUCCESS;
}
