# ##############################################################################
# SPDX-License-Identifier: Apache-2.0
# ##############################################################################

if(CONFIG_MOXINZHI_ANKI_IMPORTER)
  set(ANKI_SQLITE_DIR ${CMAKE_BINARY_DIR}/apps/external/sqlite/build)

  set(ANKI_SRCS
      anki_import_main.c
      anki_importer/anki_importer.c
      anki_importer/anki_sha256.c
      anki_importer/anki_text.c
      card_model/card_model.c)

  nuttx_add_application(
    NAME
    anki_import
    PRIORITY
    ${CONFIG_MOXINZHI_ANKI_IMPORTER_PRIORITY}
    STACKSIZE
    ${CONFIG_MOXINZHI_ANKI_IMPORTER_STACKSIZE}
    MODULE
    ${CONFIG_MOXINZHI_ANKI_IMPORTER}
    SRCS
    ${ANKI_SRCS}
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/anki_importer
    ${NUTTX_APPS_DIR}/system/zlib/zlib
    ${ANKI_SQLITE_DIR}
    ${NUTTX_APPS_DIR}/../external/unqlite/unqlite
    DEPENDS
    zlib
    unqlite
    sqlite)
endif()
