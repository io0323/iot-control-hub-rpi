# iot-control-hub-rpi

作品03（gw-01）：IoT デバイス統合管理・監視システム（C++ × Raspberry Pi）。

- **対応する共通仕様：iot-common-spec v0.2.2**（トピックは `iot/v1/...`）
- 設計：[要件定義・基本設計書](docs/design/01-requirements-basic-design.md)、[詳細設計書](docs/design/02-detailed-design.md)、
  [未決事項](docs/open-questions.md)
- 現在のフェーズ：Phase 1（骨組み・UART 受信と変換・MQTT 発行・契約テスト）

## できること（Phase 1）

- 作品01（mcu-c-01）と UART（115200bps、COBS＋CRC-16）でつながり、TELEMETRY・EVENT・STATE・ACK を
  `mcu-c-01` 名義の telemetry・event・state・cmd/ack に変換して MQTT に発行する（iotspec の参照実装と同じ変換。
  ゴールデン35件で照合）
- 1秒ごとの HEARTBEAT、STATE 待ちでの GET_STATUS（毎秒1件まで）、3秒途絶えたときの `bridged_by` 付き offline の state
- gw-01 の state（online、retain）と LWT（offline）。送信制限（発信元×kind ごと、平均毎秒10件・1秒50件）、QoS 1 の確認待ち32件、
  切断中の保持

## 使い方

```sh
iot-gw --config /etc/iot-gw/gateway.yaml [--uart /dev/ttyAMA0]
```

設定の例は [config/gateway.yaml](config/gateway.yaml)。MQTT のパスワードは `$CREDENTIALS_DIRECTORY/mqtt_password`（systemd）か
環境変数 `IOTSPEC_MQTT_PASSWORD`。設定の誤りは理由を表示して終了コード 2。

## ビルドとテスト

```sh
make test                 # 手元でビルドと ctest（単体＋ゴールデン）
make IN_DOCKER=1 test     # Debian bookworm・GCC 12 のコンテナで
make PRESET=tsan IN_DOCKER=1 test
make tables lint contract # 対応表の照合・clang-format／clang-tidy・契約テスト（コンテナ）
```

依存：libmosquitto、yaml-cpp、nlohmann/json、spdlog（無ければ FetchContent）、GoogleTest（FetchContent）。
一覧は [scripts/deps-apt.txt](scripts/deps-apt.txt) と [Brewfile](Brewfile)。
