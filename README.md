# iot-control-hub-rpi

作品03（gw-01）：IoT デバイス統合管理・監視システム（C++ × Raspberry Pi）。

- **対応する共通仕様：iot-common-spec v0.2.3**（トピックは `iot/v1/...`）
- 設計：[要件定義・基本設計書](docs/design/01-requirements-basic-design.md)、[詳細設計書](docs/design/02-detailed-design.md)、
  [未決事項](docs/open-questions.md)
- 現在のフェーズ：Phase 4（ルールエンジン・システムモード・再読込）

## できること（Phase 1）

- 作品01（mcu-c-01）と UART（115200bps、COBS＋CRC-16）でつながり、TELEMETRY・EVENT・STATE・ACK を
  `mcu-c-01` 名義の telemetry・event・state・cmd/ack に変換して MQTT に発行する（iotspec の参照実装と同じ変換。
  ゴールデン35件で照合）
- 1秒ごとの HEARTBEAT、STATE 待ちでの GET_STATUS（毎秒1件まで）、3秒途絶えたときの `bridged_by` 付き offline の state
- gw-01 の state（online、retain）と LWT（offline）。送信制限（発信元×kind ごと、平均毎秒10件・1秒50件）、QoS 1 の確認待ち32件、
  切断中の保持

## できること（Phase 2）

- MQTT の telemetry・event・state・cmd/ack を購読し、JSON Schema（draft 2020-12、jsoncons）で検証する。不正なものは理由ごとに数えて捨てる
- デバイス台帳：devices.yaml の静的登録と、state の channels・actuators による自動登録
- 生存監視：UART は2秒で suspect・3秒で offline、MQTT は LWT・state の offline とチャネルの stale。online ⇔ offline で
  gw-01 の DEVICE_OFFLINE／DEVICE_ONLINE
- データ品質：重複（直近1,024個）、欠番、stale、時刻ずれ（CLOCK_SKEW は同一デバイス1時間に1回）、遅延到着、bridged 配下の状態不明
- 件数は60秒ごとと終了時にログ `event=stats` に出す。Phase 4 から gw-01 の telemetry（unit `count`、60秒ごと。
  iot-common-spec v0.2.3 #45）でも公開する

## できること（Phase 3）

- 作品04（app-01）の request を requests.yaml の許可リストで判定し、指令（cmd）に変えて request/ack を返す
  （accepted → done／failed、または rejected）。request_id を1時間覚え、重複には現在の状態を返す
- 調停：アクチュエーターごとの所有者（safety ＞ manual ＞ auto ＞ schedule、同じ優先度は後勝ち）。奪われた所有者は
  リースの期限まで待ち、解放・期限切れで引き継ぐ
- 指令管理：ISSUED → SENT → ACCEPTED → DONE ／ REJECTED・FAILED・TIMED_OUT。ACK を2秒待ち、同じ cmd_id（UART は
  同じ cmd_ref）で最大3回再送。作品01 へは毎秒1件（再送を優先）・未完了4件まで。expires_at を過ぎた指令は送らない。
  offline のデバイス宛はその場で FAILED。FAILED・TIMED_OUT で COMMAND_FAILED
- 契約テスト `iotspec-contract --role gw-01` の全チェックに合格

## できること（Phase 4）

- ルールエンジン（rules.yaml）：条件は all・any・not と、チャネルの最新値・デバイスの生存状態・システムモード・時刻帯
  （gateway.yaml の timezone）。for（継続時間）、hysteresis、cooldown。状態は idle → pending → active → cooldown
- 評価は値が更新されたチャネルを参照するルールだけ。加えて for・cooldown の期限と1秒ごとの時刻帯・stale の変化。
  stale・quality が good でない値は「不明」で、発火せず、active なら新しい指令もリースの更新も止める
- アクション：command（調停して指令。hold は active の間リースを更新し、解除で release）、set_system_mode、
  notify（RULE_FIRED の重大度と message）、reconnect（作品03 の UART・MQTT の接続をバックオフを待たずに開き直す）
- システムモード NORMAL・ALERT・EMERGENCY：SYSTEM_MODE_CHANGED と gw-01 の state の mode。EMERGENCY では
  schedule の request を拒否し（emergency_mode）、auto のルールの指令を止める。解除は条件の回復＋承認
  （request の set_mode NORMAL、v0.2.3 #46）。ALERT は ALERT に入れたルールがすべて解除されたら NORMAL に戻る
- SIGHUP：rules.yaml・devices.yaml・requests.yaml を読み直し、すべて検証に通ったときだけ入れ替える

## 使い方

```sh
iot-gw --config /etc/iot-gw/gateway.yaml [--uart /dev/ttyAMA0]
```

設定の例は [config/gateway.yaml](config/gateway.yaml)・[config/devices.yaml](config/devices.yaml)・
[config/requests.yaml](config/requests.yaml)・[config/rules.yaml](config/rules.yaml)（基本設計 第6章の3つのルール）。MQTT のパスワードは `$CREDENTIALS_DIRECTORY/mqtt_password`（systemd）か
環境変数 `IOTSPEC_MQTT_PASSWORD`。設定の誤りは理由を表示して終了コード 2。

## ビルドとテスト

```sh
make test                 # 手元でビルドと ctest（単体＋ゴールデン）
make IN_DOCKER=1 test     # Debian bookworm・GCC 12 のコンテナで
make PRESET=tsan IN_DOCKER=1 test
make tables lint contract # 対応表の照合・clang-format／clang-tidy・契約テスト（コンテナ）
make integration coverage # 結合テスト（代役・作品02 のホスト版）・core の行カバレッジ（コンテナ）
```

依存：libmosquitto、yaml-cpp、nlohmann/json、spdlog（無ければ FetchContent）、GoogleTest（FetchContent）。
一覧は [scripts/deps-apt.txt](scripts/deps-apt.txt) と [Brewfile](Brewfile)。
