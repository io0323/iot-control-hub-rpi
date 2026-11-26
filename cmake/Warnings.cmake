# 自分のターゲットだけに付ける警告とサニタイザ（外部ライブラリには付けない）。
add_library(gw_options INTERFACE)
target_compile_options(gw_options INTERFACE -Wall -Wextra -Wpedantic)
if(GW_WERROR)
  target_compile_options(gw_options INTERFACE -Werror)
endif()

if(GW_SANITIZE)
  message(STATUS "サニタイザ: ${GW_SANITIZE}")
  # 外部ライブラリを含めて全体に付ける（ASan・TSan は混在させると誤検知・見逃しになる）
  add_compile_options(-fsanitize=${GW_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${GW_SANITIZE})
endif()
