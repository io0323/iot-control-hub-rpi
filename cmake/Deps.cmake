# 外部ライブラリ（詳細設計書 第9章）。
# nlohmann/json・yaml-cpp・spdlog は OS のパッケージを優先し、無ければ FetchContent で取得する。
# libmosquitto は OS のパッケージ（pkg-config）。GoogleTest は FetchContent。
# OS のパッケージの一覧は scripts/deps-apt.txt（Linux）と Brewfile（macOS）。
include(FetchContent)

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json: FetchContent")
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
  message(STATUS "yaml-cpp: FetchContent")
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  # yaml-cpp 0.8.0 の cmake_minimum_required は CMake 4 で拒否されるので、取得するときだけ下限を示す
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(yaml-cpp)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()
# yaml-cpp 0.7（Debian bookworm）は yaml-cpp、0.8 は yaml-cpp::yaml-cpp を作る
if(TARGET yaml-cpp::yaml-cpp)
  set(GW_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(GW_YAML_TARGET yaml-cpp)
endif()

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "spdlog: FetchContent")
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.12.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# JSON Schema（draft 2020-12）の検証（D6）。adapters だけが使う。ヘッダのみ
FetchContent_Declare(
  jsoncons
  URL https://github.com/danielaparker/jsoncons/archive/refs/tags/v1.10.0.tar.gz
  URL_HASH SHA256=c829350a2eece662beb378143e2ab1c1cbb07d703687bf168626665a8a7d8bc1
)
set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jsoncons)
# 警告（-Werror）と clang-tidy の対象から外す（CMake 3.25 の FetchContent は SYSTEM を受け付けない）
get_target_property(GW_JSONCONS_INCLUDES jsoncons INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(jsoncons PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GW_JSONCONS_INCLUDES}")

# SQLite（履歴DB、open-questions #49）：amalgamation を版を固定して取り込み、静的にリンクする。
# ターゲット（gw_sqlite3）はサニタイザのフラグが付くよう src/CMakeLists.txt で作る
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
  URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
)
FetchContent_MakeAvailable(sqlite3)
enable_language(C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

find_package(Threads REQUIRED)

if(GW_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
  )
  FetchContent_MakeAvailable(googletest)
endif()
