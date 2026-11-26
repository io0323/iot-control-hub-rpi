# iot-common-spec（共通仕様の実体）を取得する。
#
# 版はここの IOTSPEC_REF だけで決める（詳細設計書 第8章）。CI の環境変数 IOTSPEC_REF と
# 食い違えば CI が失敗する（scripts/check-iotspec-ref.sh）。
# 手元のチェックアウトを使うときは -DFETCHCONTENT_SOURCE_DIR_IOTSPEC=<path>。
include(FetchContent)

set(IOTSPEC_REF "v0.2.2" CACHE STRING "iot-common-spec のタグ（またはブランチ）")

FetchContent_Declare(
  iotspec
  GIT_REPOSITORY https://github.com/io0323/iot-common-spec.git
  GIT_TAG ${IOTSPEC_REF}
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(iotspec)

set(IOTSPEC_SOURCE_DIR ${iotspec_SOURCE_DIR})
foreach(dir uart/golden profiles schemas samples)
  if(NOT EXISTS ${IOTSPEC_SOURCE_DIR}/${dir})
    message(FATAL_ERROR "iot-common-spec（${IOTSPEC_REF}）に ${dir} がない: ${IOTSPEC_SOURCE_DIR}")
  endif()
endforeach()
message(STATUS "iot-common-spec ${IOTSPEC_REF}: ${IOTSPEC_SOURCE_DIR}")
