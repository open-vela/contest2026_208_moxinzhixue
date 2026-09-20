cmake_minimum_required(VERSION 3.16)
project(xiaozhi_client_host_tests C)

include(FetchContent)

FetchContent_Declare(
  cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.12.tar.gz
  URL_HASH SHA256=760687665ab41a5cff9c40b1053c19572bcdaadef1194e5cba1b5e6f824686e7)
FetchContent_MakeAvailable(cjson)

add_executable(
  test_xiaozhi_client
  test_xiaozhi_client.c
  ../src/xiaozhi_client.c)

add_executable(
  test_xiaozhi_provisioning
  test_xiaozhi_provisioning.c
  ../src/xiaozhi_ota_parser.c
  ../src/xiaozhi_provisioning.c)

add_executable(
  test_xiaozhi_tls
  test_xiaozhi_tls.c
  fake_curl/fake_curl.c
  ../src/xiaozhi_tls.c)

target_include_directories(
  test_xiaozhi_client
  PRIVATE
  ../include
  ${cjson_SOURCE_DIR})

target_include_directories(
  test_xiaozhi_provisioning
  PRIVATE
  ../include
  ${cjson_SOURCE_DIR})

target_include_directories(
  test_xiaozhi_tls
  PRIVATE
  fake_curl
  ../include)

target_link_libraries(test_xiaozhi_client PRIVATE cjson)
target_link_libraries(test_xiaozhi_provisioning PRIVATE cjson)

if(MSVC)
  target_compile_options(test_xiaozhi_client PRIVATE /W4)
  target_compile_options(test_xiaozhi_provisioning PRIVATE /W4)
  target_compile_options(test_xiaozhi_tls PRIVATE /W4)
else()
  target_compile_options(test_xiaozhi_client PRIVATE -Wall -Wextra -Werror)
  target_compile_options(
    test_xiaozhi_provisioning PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
  target_compile_options(
    test_xiaozhi_tls PRIVATE -Wall -Wextra -Werror -D_GNU_SOURCE)
endif()

enable_testing()
add_test(NAME xiaozhi_client_state_machine COMMAND test_xiaozhi_client)
add_test(NAME xiaozhi_provisioning_flow COMMAND test_xiaozhi_provisioning)
add_test(NAME xiaozhi_tls_bootstrap COMMAND test_xiaozhi_tls)
