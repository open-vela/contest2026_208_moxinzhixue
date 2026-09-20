if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST)
  set(HELLO_APP_SRCS
      hello_app_main.c
      moxinzhi_anki.c
      moxinzhi_ai_service.c
      moxinzhi_board_keys.c
      moxinzhi_diag.c
      moxinzhi_input.c
      moxinzhi_instance.c
      moxinzhi_model.c
      moxinzhi_runtime.c
      moxinzhi_storage.c
      moxinzhi_ui.c)
  set(HELLO_APP_DEPENDS lvgl framework_utils)
  set(HELLO_APP_INCLUDE_DIRS
      ${CMAKE_CURRENT_LIST_DIR}/../contest2026_208_anki_importer/include
      ${CMAKE_CURRENT_LIST_DIR}/xiaozhi_client/include)

  if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT)
    list(
      APPEND
      HELLO_APP_SRCS
      xiaozhi_client/src/cloud_client.c
      xiaozhi_client/src/xiaozhi_client.c
      xiaozhi_client/src/xiaozhi_audio.c
      xiaozhi_client/src/xiaozhi_ota.c
      xiaozhi_client/src/xiaozhi_ota_parser.c
      xiaozhi_client/src/xiaozhi_provisioning.c
      xiaozhi_client/src/xiaozhi_tls.c)
    list(APPEND HELLO_APP_DEPENDS curl libopus mbedtls)
  endif()

  if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE)
    list(
      APPEND
      HELLO_APP_SRCS
      platform/gs1_platform.c
      platform/gs1_thread.c
      platform/gs1_storage.c
      platform/gs1_wifi.c
      platform/gs1_audio.c
      platform/gs1_time.c)
  endif()

  if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT AND
     CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE)
    list(APPEND HELLO_APP_SRCS platform/gs1_xiaozhi_audio.c)
  endif()

  nuttx_add_application(
    NAME
    lvgl_screen_test
    PRIORITY
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_PRIORITY}
    STACKSIZE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST_STACKSIZE}
    MODULE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST}
    DEPENDS
    ${HELLO_APP_DEPENDS}
    SRCS
    ${HELLO_APP_SRCS}
    INCLUDE_DIRECTORIES
    ${HELLO_APP_INCLUDE_DIRS})
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT)
  set(XIAOZHI_CHAT_PRIORITY
      ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_PRIORITY})
  set(XIAOZHI_CHAT_STACKSIZE
      ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_CLIENT_STACKSIZE})
  if(NOT XIAOZHI_CHAT_PRIORITY)
    set(XIAOZHI_CHAT_PRIORITY 100)
  endif()
  if(NOT XIAOZHI_CHAT_STACKSIZE)
    set(XIAOZHI_CHAT_STACKSIZE 49152)
  endif()
  set(XIAOZHI_PAIR_PRIORITY
      ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_PRIORITY})
  set(XIAOZHI_PAIR_STACKSIZE
      ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_XIAOZHI_PAIR_STACKSIZE})
  if(NOT XIAOZHI_PAIR_PRIORITY)
    set(XIAOZHI_PAIR_PRIORITY 100)
  endif()
  if(NOT XIAOZHI_PAIR_STACKSIZE)
    set(XIAOZHI_PAIR_STACKSIZE 32768)
  endif()

  nuttx_add_application(
    NAME
    xiaozhi_chat
    PRIORITY
    ${XIAOZHI_CHAT_PRIORITY}
    STACKSIZE
    ${XIAOZHI_CHAT_STACKSIZE}
    MODULE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST}
    DEPENDS
    curl
    libopus
    mbedtls
    SRCS
    xiaozhi_chat_main.c
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/xiaozhi_client/include)

  nuttx_add_application(
    NAME
    xiaozhi_pair
    PRIORITY
    ${XIAOZHI_PAIR_PRIORITY}
    STACKSIZE
    ${XIAOZHI_PAIR_STACKSIZE}
    MODULE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST}
    DEPENDS
    curl
    mbedtls
    SRCS
    xiaozhi_pair_main.c
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/xiaozhi_client/include)
endif()

if(CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE)
  set(GS1_HWCHECK_SRCS gs1_hwcheck_main.c)
  if(NOT CONFIG_LVX_USE_DEMO_CONTEST2026_208_LVGL_SCREEN_TEST)
    list(
      APPEND
      GS1_HWCHECK_SRCS
      platform/gs1_platform.c
      platform/gs1_thread.c
      platform/gs1_storage.c
      platform/gs1_wifi.c
      platform/gs1_audio.c
      platform/gs1_time.c)
  endif()

  nuttx_add_application(
    NAME
    gs1_hwcheck
    PRIORITY
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_PRIORITY}
    STACKSIZE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_STACKSIZE}
    MODULE
    ${CONFIG_LVX_USE_DEMO_CONTEST2026_208_PLATFORM_SERVICE}
    DEPENDS
    framework_utils
    SRCS
    ${GS1_HWCHECK_SRCS})
endif()
