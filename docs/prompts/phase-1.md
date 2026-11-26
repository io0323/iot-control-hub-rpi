# 作品03 フェーズ 1：骨組み・UART 受信と変換・MQTT 発行・契約テスト

## 始める前に

- io0323/iot-control-hub-rpi は空。main に README.md（1行）だけの初回コミットを作って push し、main から `phase-1/skeleton` を切る
- 作業ディレクトリに io が置いた `01-requirements-basic-design.md`・`02-detailed-design.md` を `docs/design/` に移す。これが設計の正。食い違いは 02（詳細設計書）→ 01 の第0章 → 01 の第1〜8章 の順に優先する
- iot-common-spec にタグ v0.2.2 があることを確かめる。無ければ止まって報告する。参照用に作業ディレクトリの外へ v0.2.2 で読み取り専用に clone する

## はじめに読むもの

- 詳細設計書：第1・2・3・4・5・6・7・8・9章と、第10章の Phase 1 の行、第11章
- 要件定義・基本設計書：第0章、NFR-05・06・08・09・10
- iot-common-spec v0.2.2：`docs/spec.md` 第2・3・7・8章、`docs/contract-tests.md`、`src/iotspec/uart/`（cobs・crc・frames・mapping・bridge）、`constants.py`、`ids.py`、`envelope.py`、`profile.py`、`sim/devices/gw_01_lite.py`、`contract/roles/gw_01.py`・`common.py`、`tools/gen_golden.py`（msg_id の規則と仮想時計）、`uart/golden/README.md`、`profiles/`、`schemas/state.schema.json`、`deploy/mosquitto/`

読み終えたら「作るもの・作らないもの・不明点」を短く報告してから始める。

## 決まっていること

- 詳細設計書第1章の D1〜D6。MQTT は libmosquitto（自作の IMqttClient 経由）、イベントループは自作、core は try_push だけ
- UART の変換は core 上の UartBridge。bridge.py（v0.2.2）の振る舞いとゴールデンが正。C++ のクラスは bridge.py と同じ単位で分け、対応する Python の関数名をコメントに書く
- open-questions の初期値は詳細設計書第11章の Q1〜Q8。Q1・Q2・Q5 は決定、Q3 は「復号できたフレームなら何でも生存確認を更新。最初の受信までは数えない」で決定。番号は #1〜#8 に振り直す
- expires_at の検査は最初の送信の前だけ（iot-common-spec #43）。Phase 1 の GET_STATUS には expires_at を付けない
- C++20、`std::format`・`std::jthread` は使わない。コンパイラの下限は GCC 12

## Phase 1 で作るもの

1. **骨組み**
   - 詳細設計書第2章のディレクトリ。CMakeLists.txt、CMakePresets.json（debug、release、asan-ubsan、tsan、coverage）、`cmake/IotSpec.cmake`（FetchContent、`IOTSPEC_REF` 既定 v0.2.2）
   - .clang-format、.clang-tidy、`-Wall -Wextra -Wpedantic -Werror`
   - 依存パッケージの一覧を1か所に置き（例：`scripts/deps-apt.txt`、`Brewfile`）、`deploy/Dockerfile.dev` と CI が同じ一覧を使う
   - `docker-compose.yml`（Mosquitto は iot-common-spec v0.2.2 の deploy/mosquitto を使う。開発用コンテナ）、`Makefile`（build、test、golden、tables、contract、lint）
   - `CLAUDE.md`（正とする書類の順、技術スタック、コマンド、規約、「仕様にないことは open-questions に書いて止める」、「共通仕様の変更が要るときは作品03 で吸収せず変更案にする」）、`docs/open-questions.md`、`docs/prompts/phase-1.md`（このプロンプトの写し）、最小の README
2. **core/common**：IClock（steady_now・system_now）と SystemClock・VirtualClock、ULID と IIdGenerator（単調 ULID、gen_golden.py と同じ規則の SequentialIdGenerator）、boot_id、Envelope（JSON 化）、発信元×kind の seq、BoundedQueue（push・try_push・追い出し付き push・pop_until・close）、TimerService、トピックと QoS・retain の表、定数
3. **core/uart**：Cobs、Crc16（CCITT-FALSE）、FrameCodec（DIAG を含む全種別。CMD は符号化）、SeqExtender、Profile（yaml-cpp。iot-common-spec の profiles/mcu-c-01.yaml）、Mapping、UartBridge（bridge.py の全機能：STATE 待ち、再起動の検知、破棄理由の計数、cmd_ref の対応表、GET_DIAG の内部フレーム、offline の state、bridged_by）
4. **Core（Phase 1 の範囲）**
   - UART の受信フレーム → UartBridge → mqtt-out。DIAG は今は破棄理由 not_converted で数えるだけ（履歴は Phase 5）
   - request_state → GET_STATUS。cmd_id を採番する普通の指令（priority auto）として、毎秒1件の上限を守る最小の送信器で送る。再送と未完了4件は Phase 3。ACK は cmd/ack として発行される
   - HEARTBEAT を 1秒ごとに送る。受信前に GET_STATUS を送らない（契約テストのゴールデン照合の前提）
   - 最後のフレーム受信から3秒、または UART の切断で、mcu-c-01 の offline の state（bridged_by gw-01、retain、seq を進めない）を発行。次の STATE で online に戻る。DEVICE_OFFLINE・ONLINE の event は Phase 2
   - gw-01 自身の state（online、retain。プロファイル gw-01.yaml）と LWT（offline、retain）
5. **adapters**：SerialPort（termios・poll・再オープンの指数バックオフ）、IMqttClient・MosquittoClient・FakeMqttClient、SignalWatcher（SIGTERM・SIGINT で正常終了。SIGHUP は受けてログだけ）、gateway.yaml の読み込みと検証（誤りは終了コード 2）
6. **app**：`iot-gw --config <path> [--uart <path>]`。uart-io、mqtt-loop（libmosquitto のスレッド）、mqtt-tx（発信元×kind のトークンバケット：平均毎秒10件・容量50、QoS 1 の確認待ち32件、切断中の保持）、core の各スレッド。詳細設計書第3・4章の起動と終了の順序。パスワードは `$CREDENTIALS_DIRECTORY/mqtt_password`、無ければ `IOTSPEC_MQTT_PASSWORD`。MQTT の購読は Phase 2
7. **テスト**
   - 単体（GoogleTest）：COBS・CRC、全フレームの往復と壊れた入力、SeqExtender の一周、UartBridge（STATE 待ち・再起動・DIAG・BOOT・INTERLOCK・offline）、BoundedQueue（追い出し・close・満杯）、TimerService、トークンバケット（仮想時計）、ULID、SerialPort（openpty の対で送受信と切断）、Core（仮想時計で HEARTBEAT の周期、3秒での offline、GET_STATUS の毎秒1件）
   - ゴールデン（ctest）：詳細設計書第8章の1段目。v0.2.2 の35件を NN 順に1つの UartBridge に流す。テストはゴールデンのファイルごとに1件とし、失敗時に番号と項目が分かるようにする
   - 対応表の照合：`gw-dump-tables` と `tools/check_tables.py`（第8章の2段目）
8. **CI**：詳細設計書第9章の「導入フェーズ 1」のジョブ（build-test、lint、sanitizers、contract、macos、arm64、spec-drift）。`push`（main）と `pull_request` で走らせる。contract は JUnit XML を成果物に残す

## Phase 1 で作らないもの

MQTT の購読・検証、DeviceRegistry・LivenessMonitor の event、データ品質（Phase 2）、CommandManager の再送・未完了4件・リース、Arbiter、RequestHandler（Phase 3）、ルールとシステムモード（Phase 4）、SQLite・gwctl（Phase 5）、systemd・sysmon・障害注入・負荷試験（Phase 6）。

## 完了条件（すべて満たしてから PR）

1. Linux（GCC）と macOS（AppleClang）で警告ゼロでビルドし、ctest が全件合格
2. ゴールデン35件すべてが一致（01→03 は msg_id・ts を含むエンベロープ全体、破棄理由、request_state、dropped_telemetry_total、diag。03→01 は hex）
3. `tools/check_tables.py` が差分なし
4. asan+ubsan と tsan で ctest の指摘ゼロ
5. clang-tidy の警告ゼロ、clang-format の差分なし
6. 契約テスト `iotspec-contract --role gw-01 --only golden_conversion --only bridged_offline_state --only schema --only seq_and_boot_id --only publish_rate --target-cmd '<iot-gw の起動コマンド> --uart {uart}'` が `make contract` と CI で合格
7. arm64・macos を含む CI の全ジョブが合格

PR の説明には、完了条件ごとの結果、契約テストの JUnit の要約、open-questions に足した項目、Phase 2 への申し送りを書く。マージはしない。

## 止まって相談すること

- bridge.py・ゴールデン・契約テストと、設計書の記述が食い違うとき
- 契約テストを通すために、設計書にない振る舞いが要るとき
- libmosquitto・yaml-cpp などの版の違いで、macOS・arm64・GCC 12 のどれかで同じコードが通らないとき
- キューの容量やタイムアウトなど、設計書にない値を決める必要が出たとき
- 共通仕様の変更が要ると分かったとき

いずれも open-questions に「論点・選択肢・推奨案」を書き、その件に関わる作業を止めて報告する。
