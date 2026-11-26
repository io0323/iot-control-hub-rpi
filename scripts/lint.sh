#!/bin/sh
# clang-format の差分なし・clang-tidy の警告ゼロ（NFR-09）。make lint が開発用コンテナの中で呼ぶ。
set -eu
BUILD_DIR=${1:-build/linux-debug}
FILES=$(git -c safe.directory='*' ls-files --cached --others --exclude-standard '*.cpp' '*.h')
echo "clang-format: $(echo "$FILES" | wc -l) ファイル"
# shellcheck disable=SC2086
clang-format --dry-run --Werror $FILES
SOURCES=$(git -c safe.directory='*' ls-files --cached --others --exclude-standard 'src/*.cpp' 'tools/*.cpp' 'tests/*.cpp')
echo "clang-tidy: $(echo "$SOURCES" | wc -l) ファイル"
# shellcheck disable=SC2086
run-clang-tidy -quiet -p "$BUILD_DIR" -j "$(nproc)" $SOURCES
