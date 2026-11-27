cmake_minimum_required(VERSION 3.25)

project(iot-control-hub-rpi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GW_BUILD_TESTS "単体テストとゴールデン照合を作る" ON)
option(GW_WERROR "警告をエラーにする" ON)
set(GW_SANITIZE "" CACHE STRING "サニタイザ：address,undefined または thread")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(IotSpec)
include(Deps)
include(Warnings)

# 実行ファイルは bin/、プロファイルとスキーマは share/iot-gw/ に置く（インストールと同じ配置）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(GW_SHARE_DIR ${CMAKE_BINARY_DIR}/share/iot-gw)
file(MAKE_DIRECTORY ${GW_SHARE_DIR})
file(COPY ${IOTSPEC_SOURCE_DIR}/profiles ${IOTSPEC_SOURCE_DIR}/schemas DESTINATION ${GW_SHARE_DIR})

add_subdirectory(src)
add_subdirectory(tools/gw_dump_tables)
add_subdirectory(tools/gwctl)

if(GW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(TARGETS iot-gw gw-dump-tables gwctl RUNTIME DESTINATION bin)
install(DIRECTORY ${IOTSPEC_SOURCE_DIR}/profiles ${IOTSPEC_SOURCE_DIR}/schemas DESTINATION share/iot-gw)
