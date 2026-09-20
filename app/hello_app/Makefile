############################################################################
# Contest 2026 team 208 MoXin Smart Learning - Makefile
############################################################################

include $(APPDIR)/Make.defs

PROGNAME  =
PRIORITY  =
STACKSIZE =
MODULE    =
MAINSRC   =
CSRCS     =

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST),)
PROGNAME  += lvgl_screen_test
PRIORITY  += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_PRIORITY)
STACKSIZE += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_STACKSIZE)
MODULE    += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST)
MAINSRC   += hello_app_main.c
CSRCS     += moxinzhi_anki.c moxinzhi_ai_service.c moxinzhi_diag.c
CSRCS     += moxinzhi_board_keys.c
CSRCS     += moxinzhi_input.c moxinzhi_instance.c moxinzhi_model.c
CSRCS     += moxinzhi_runtime.c moxinzhi_storage.c moxinzhi_ui.c
CFLAGS    += ${INCDIR_PREFIX}$(APPDIR)/packages/demos/contest2026_208_anki_importer/include
CFLAGS    += ${INCDIR_PREFIX}$(CURDIR)/xiaozhi_client/include

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT),)
XIAOZHI_CHAT_PRIORITY = $(if \
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_PRIORITY),\
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_PRIORITY),100)
XIAOZHI_CHAT_STACKSIZE = $(if \
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_STACKSIZE),\
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_STACKSIZE),49152)
XIAOZHI_PAIR_PRIORITY = $(if \
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_PRIORITY),\
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_PRIORITY),100)
XIAOZHI_PAIR_STACKSIZE = $(if \
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_STACKSIZE),\
  $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_STACKSIZE),32768)
PROGNAME  += xiaozhi_chat
PRIORITY  += $(XIAOZHI_CHAT_PRIORITY)
STACKSIZE += $(XIAOZHI_CHAT_STACKSIZE)
MODULE    += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST)
MAINSRC   += xiaozhi_chat_main.c
PROGNAME  += xiaozhi_pair
PRIORITY  += $(XIAOZHI_PAIR_PRIORITY)
STACKSIZE += $(XIAOZHI_PAIR_STACKSIZE)
MODULE    += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST)
MAINSRC   += xiaozhi_pair_main.c
CSRCS     += xiaozhi_client/src/cloud_client.c
CSRCS     += xiaozhi_client/src/xiaozhi_client.c
CSRCS     += xiaozhi_client/src/xiaozhi_audio.c
CSRCS     += xiaozhi_client/src/xiaozhi_ota.c
CSRCS     += xiaozhi_client/src/xiaozhi_ota_parser.c
CSRCS     += xiaozhi_client/src/xiaozhi_provisioning.c
CSRCS     += xiaozhi_client/src/xiaozhi_tls.c
CFLAGS    += ${INCDIR_PREFIX}$(CURDIR)/xiaozhi_client/include
ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE),)
CSRCS     += platform/gs1_xiaozhi_audio.c
endif
endif
endif

ifneq ($(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE),)
PROGNAME  += gs1_hwcheck
PRIORITY  += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_PRIORITY)
STACKSIZE += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_STACKSIZE)
MODULE    += $(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE)
MAINSRC   += gs1_hwcheck_main.c
CSRCS     += platform/gs1_platform.c platform/gs1_thread.c
CSRCS     += platform/gs1_storage.c platform/gs1_wifi.c
CSRCS     += platform/gs1_audio.c platform/gs1_time.c
endif

include $(APPDIR)/Application.mk
