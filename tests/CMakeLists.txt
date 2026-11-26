include(GoogleTest)

add_library(gw_test_support INTERFACE)
target_compile_definitions(gw_test_support INTERFACE
  GW_IOTSPEC_DIR="${IOTSPEC_SOURCE_DIR}"
  GW_PROFILES_DIR="${IOTSPEC_SOURCE_DIR}/profiles"
)

add_executable(gw_unit_tests
  unit/test_bounded_queue.cpp
  unit/test_clock_ids.cpp
  unit/test_cobs_crc.cpp
  unit/test_config.cpp
  unit/test_core.cpp
  unit/test_frames.cpp
  unit/test_mqtt_tx.cpp
  unit/test_profile.cpp
  unit/test_serial_port.cpp
  unit/test_timer_service.cpp
  unit/test_token_bucket.cpp
  unit/test_uart_bridge.cpp
  unit/test_uart_io.cpp
)
target_link_libraries(gw_unit_tests PRIVATE gw_app gw_test_support gw_options GTest::gtest_main)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(gw_unit_tests PRIVATE util)  # openpty
endif()
gtest_discover_tests(gw_unit_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS unit)

# ゴールデン照合（詳細設計書 第8章の1段目）：ゴールデンのファイルごとに1件
add_executable(gw_golden_tests golden/test_golden.cpp)
target_link_libraries(gw_golden_tests PRIVATE gw_core gw_test_support gw_options GTest::gtest_main)
gtest_discover_tests(gw_golden_tests DISCOVERY_TIMEOUT 60 PROPERTIES LABELS golden)
