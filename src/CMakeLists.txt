# gw_core：OS 非依存（nlohmann/json と yaml-cpp だけを使う）
add_library(gw_core STATIC
  core/common/Clock.cpp
  core/common/Envelope.cpp
  core/common/Ids.cpp
  core/common/TimerService.cpp
  core/uart/Cobs.cpp
  core/uart/FrameCodec.cpp
  core/uart/Mapping.cpp
  core/uart/Profile.cpp
  core/uart/SeqExtender.cpp
  core/uart/UartBridge.cpp
  core/Core.cpp
)
target_include_directories(gw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gw_core PUBLIC nlohmann_json::nlohmann_json ${GW_YAML_TARGET} PRIVATE gw_options)

# gw_adapters：OS・外部ライブラリに触れる
add_library(gw_adapters STATIC
  adapters/config/GatewayConfig.cpp
  adapters/mqtt/FakeMqttClient.cpp
  adapters/mqtt/MosquittoClient.cpp
  adapters/platform/SignalWatcher.cpp
  adapters/serial/SerialPort.cpp
)
target_include_directories(gw_adapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gw_adapters PUBLIC gw_core ${GW_YAML_TARGET} PkgConfig::MOSQUITTO Threads::Threads
  PRIVATE gw_options spdlog::spdlog)

# gw_app：スレッドの組み立て（main 以外はテストから使う）
configure_file(app/gw_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/gw_version.h @ONLY)
add_library(gw_app STATIC
  app/CoreRunner.cpp
  app/MqttTxWorker.cpp
  app/UartIo.cpp
)
target_include_directories(gw_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(gw_app PUBLIC gw_adapters spdlog::spdlog PRIVATE gw_options)

add_executable(iot-gw app/main.cpp)
target_link_libraries(iot-gw PRIVATE gw_app gw_options)
