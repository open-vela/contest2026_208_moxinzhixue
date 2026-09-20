cmake_minimum_required(VERSION 3.16)

project(moxinzhi_anki_importer_host_tests C)

include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(SQLite3 REQUIRED)
find_package(ZLIB REQUIRED)

set(OPENVELA_WORKSPACE "" CACHE PATH
    "Path to a full OpenVela workspace containing apps/ and external/")
if(NOT OPENVELA_WORKSPACE AND DEFINED ENV{OPENVELA_WORKSPACE})
  set(OPENVELA_WORKSPACE "$ENV{OPENVELA_WORKSPACE}")
endif()

if(NOT EXISTS "${OPENVELA_WORKSPACE}/apps/system/zlib/zlib/contrib/minizip/unzip.c"
   OR NOT EXISTS "${OPENVELA_WORKSPACE}/apps/netutils/cjson/cJSON/cJSON.c"
   OR NOT EXISTS "${OPENVELA_WORKSPACE}/external/unqlite/unqlite/unqlite.c")
  message(FATAL_ERROR
          "Set OPENVELA_WORKSPACE to the full OpenVela checkout")
endif()

set(IMPORTER_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(ZLIB_SOURCE_DIR "${OPENVELA_WORKSPACE}/apps/system/zlib/zlib")
set(CJSON_SOURCE_DIR "${OPENVELA_WORKSPACE}/apps/netutils/cjson/cJSON")
set(UNQLITE_SOURCE_DIR "${OPENVELA_WORKSPACE}/external/unqlite/unqlite")

add_library(anki_host_minizip STATIC
            "${ZLIB_SOURCE_DIR}/contrib/minizip/ioapi.c"
            "${ZLIB_SOURCE_DIR}/contrib/minizip/unzip.c")
target_include_directories(anki_host_minizip PUBLIC "${ZLIB_SOURCE_DIR}")
target_compile_definitions(anki_host_minizip PRIVATE
                           _FILE_OFFSET_BITS=64 _LARGEFILE64_SOURCE)
target_link_libraries(anki_host_minizip PUBLIC ZLIB::ZLIB)

add_library(anki_host_cjson STATIC "${CJSON_SOURCE_DIR}/cJSON.c")
target_include_directories(anki_host_cjson PUBLIC "${CJSON_SOURCE_DIR}")
target_link_libraries(anki_host_cjson PUBLIC m)

add_library(anki_host_unqlite STATIC "${UNQLITE_SOURCE_DIR}/unqlite.c")
target_include_directories(anki_host_unqlite PUBLIC "${UNQLITE_SOURCE_DIR}")
target_compile_options(anki_host_unqlite PRIVATE
                       -Wno-unused-function -Wno-misleading-indentation
                       -Wno-shadow -Wno-stringop-overflow)

add_library(moxinzhi_anki_core STATIC
            "${IMPORTER_ROOT}/anki_importer/anki_importer.c"
            "${IMPORTER_ROOT}/anki_importer/anki_sha256.c"
            "${IMPORTER_ROOT}/anki_importer/anki_text.c"
            "${IMPORTER_ROOT}/card_model/card_model.c")
target_compile_features(moxinzhi_anki_core PRIVATE c_std_11)
target_compile_definitions(moxinzhi_anki_core PRIVATE
                           ANKI_HOST_BUILD _GNU_SOURCE)
target_compile_options(moxinzhi_anki_core PRIVATE
                       -Wall -Wextra -Werror -Wpedantic)
target_include_directories(moxinzhi_anki_core
  PUBLIC "${IMPORTER_ROOT}/include"
  PRIVATE "${IMPORTER_ROOT}/anki_importer")
target_link_libraries(moxinzhi_anki_core
  PUBLIC anki_host_minizip anki_host_cjson anki_host_unqlite SQLite::SQLite3)

add_executable(anki_import_host "${IMPORTER_ROOT}/anki_import_main.c")
target_compile_definitions(anki_import_host PRIVATE ANKI_HOST_BUILD _GNU_SOURCE)
target_compile_options(anki_import_host PRIVATE
                       -Wall -Wextra -Werror -Wpedantic)
target_link_libraries(anki_import_host PRIVATE moxinzhi_anki_core)

add_executable(test_anki_importer test_anki_importer.c)
target_compile_definitions(test_anki_importer PRIVATE _GNU_SOURCE)
target_compile_options(test_anki_importer PRIVATE
                       -Wall -Wextra -Werror -Wpedantic)
target_include_directories(test_anki_importer PRIVATE
                           "${IMPORTER_ROOT}/anki_importer")
target_link_libraries(test_anki_importer PRIVATE moxinzhi_anki_core)

set(FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_test(NAME generate_anki_fixtures
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/generate_fixtures.py"
          --output "${FIXTURE_DIR}")
add_test(NAME anki_importer
  COMMAND test_anki_importer "${FIXTURE_DIR}")
set_tests_properties(anki_importer PROPERTIES DEPENDS generate_anki_fixtures)
