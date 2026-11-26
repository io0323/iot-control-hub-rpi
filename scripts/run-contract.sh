#!/bin/sh
# 契約テスト（詳細設計書 第8章の3段目）。make contract が開発用コンテナの中で呼ぶ。
#   scripts/run-contract.sh <build_dir> <junit.xml> <check>...
# 対象（iot-gw）は契約テストが --target-cmd で起動し、{uart} は契約テストの pty に置き換わる。
set -eu
BUILD_DIR=$1
JUNIT=$2
shift 2
ONLY=""
for check in "$@"; do
  ONLY="$ONLY --only $check"
done
# shellcheck disable=SC2086
exec iotspec-contract --role gw-01 $ONLY \
  --target-cmd "$BUILD_DIR/bin/iot-gw --config ci/gateway.yaml --uart {uart}" \
  --junit "$JUNIT"
