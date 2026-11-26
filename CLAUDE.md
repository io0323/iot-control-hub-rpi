# iot-control-hub-rpi（作品03：gw-01）

IoT ポートフォリオの作品03（C++ × Raspberry Pi、ゲートウェイ）。作品01（UART）・作品02（MQTT）のデータを集め、
ルールで判断して指令を出し、作品04の要求を調停する。共通仕様は iot-common-spec（タグ v0.2.2）。

## 正とするもの（食い違ったら上を優先する）
1. iot-common-spec v0.2.2 の `docs/spec.md`・`schemas/`・`profiles/`・`uart/golden/` と、参照実装
   `iotspec.uart.bridge`。合否は契約テスト `iotspec-contract --role gw-01` で決まる
2. `docs/design/02-detailed-design.md`（詳細設計書）
3. `docs/design/01-requirements-basic-design.md` の第0章
4. `docs/design/01-requirements-basic-design.md` の第1〜8章（socat・WSL・Paho・Python 製シミュレータの記述は古い）

## 判断の約束
- 仕様・設計書に書かれていないことを独断で決めない。`docs/open-questions.md` に「論点・選択肢・推奨案」を書き、
  その件に関わる作業を止めて報告する（キューの容量・タイムアウトなどの値も同じ）
- 共通仕様の変更が要るときは、作品03で吸収せず iot-common-spec への変更案にする
- C++ の UartBridge は bridge.py と同じ単位でクラスを分け、対応する Python の名前をコメントに書く

## 技術スタック
- C++20、CMake ≥ 3.25 ＋ Ninja、コンパイラの下限は GCC 12（AppleClang も通す）。`std::format`・`std::jthread` は使わない
- core（`src/core`、gw_core）：nlohmann/json・yaml-cpp だけ。OS に触れない。`#ifdef __linux__` は書かない
- adapters（`src/adapters`）：libmosquitto（自作の IMqttClient 経由）、termios、シグナル。app（`src/app`）：スレッドの組み立て
- ログは spdlog（stderr、1行1イベントの key=value）。テストは GoogleTest
- 依存パッケージの一覧は `scripts/deps-apt.txt`（Linux）と `Brewfile`（macOS）の1か所。`deploy/Dockerfile.dev` と CI が使う
- iot-common-spec の版は `cmake/IotSpec.cmake` の `IOTSPEC_REF` だけで決める（CI の環境変数と食い違うと失敗）

## コマンド
- `make build` / `make test` / `make golden`：手元（macOS・Linux）でビルド・ctest（`PRESET=asan-ubsan`・`tsan` なども）
- `make IN_DOCKER=1 test`：開発用コンテナ（Debian bookworm・GCC 12）で同じことをする（CI はこれ）
- `make tables`：`gw-dump-tables` の出力を `tools/check_tables.py` で iotspec と突き合わせる（コンテナ）
- `make lint`：clang-format の差分なし・clang-tidy の警告ゼロ（コンテナ、clang 15）。`make format` で整形
- `make contract`：Mosquitto（iot-common-spec の deploy/mosquitto）と iot-gw を起動して契約テスト（コンテナ）
- macOS で Xcode のライセンスが未同意なら `DEVELOPER_DIR=/Library/Developer/CommandLineTools` を付ける

## 規約
- 時刻を読む処理は IClock を注入する（steady は継続時間、system はエンベロープの ts）。テストは VirtualClock
- 数値・コード値・上限は `src/core/common/Constants.h`・`Topics.h`・`core/uart/Mapping.h` に集める
- core はどのキューにも try_push だけで入れる（D4）。受信スレッド → core の向きだけが待ってよい
- 各フェーズは main から `phase-N/<名前>` を切り、完了条件のコマンドと CI 全ジョブが通ってから PR にする。マージは io が行う
- フェーズのプロンプトは `docs/prompts/phase-N.md` に写しを置く
