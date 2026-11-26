#!/bin/sh
# CI の環境変数 IOTSPEC_REF と cmake/IotSpec.cmake の IOTSPEC_REF が同じかを確かめる（詳細設計書 第8章）
set -eu
CMAKE_REF=$(sed -n 's/^set(IOTSPEC_REF "\([^"]*\)".*/\1/p' cmake/IotSpec.cmake)
if [ "${IOTSPEC_REF:-}" != "$CMAKE_REF" ]; then
  echo "IOTSPEC_REF が食い違う: CI=${IOTSPEC_REF:-（未設定）} cmake/IotSpec.cmake=$CMAKE_REF" >&2
  exit 1
fi
echo "IOTSPEC_REF=$CMAKE_REF"
