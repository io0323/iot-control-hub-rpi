# 作品03 要件定義・基本設計書：IoTデバイス統合管理・監視システム（C++ × Raspberry Pi）

Nov 20, 2026 · @io

**改訂（2026-11-25）：** 共通仕様 v0.2.2（`iot-common-spec` タグ `v0.2.2`）に合わせて改訂した。改訂の要点は第0章にまとめ、第1〜8章と食い違う場合は第0章を優先する。作品01（`io0323/iot-thermal-guard-stm32`）と作品02（`io0323/iot-modular-node-esp32`）はプログラム先行の段階を完了済み。

**改訂（2026-11-26）：** 詳細設計の前に次を決めた。詳細は[作品03 詳細設計書](https://claude.ai/artifact/4ZqPSBz2bdATN1UUAEtRoF)の第1章で、本書と食い違う場合はそちらを優先する。

- iot-common-spec にタグ v0.2.2 はまだ無い（v0.2.1 のみ）。作品03 の前に Phase S で v0.2.2 をリリースする
- ACL：gw-01 が mcu-c-01 名義で発行できる設定は v0.2.1 の deploy/mosquitto/acl にある。共通仕様の変更は不要

  ```
  ```
- MQTT は自作の IMqttClient と libmosquitto（Paho は使わない）。イベントループは自作（Asio は使わない）
- core はどのキューにも待たずに入れる。アウトバウンドは UART と MQTT で分け、ブローカー停止中の保持と破棄は MQTT 送信スレッドの責任とする（第5章の「送信側を待たせる」は受信→core の向きだけ）
- request\_id の重複判定は core のメモリで行い、起動時に直近1時間の requests を読み込む（core は実行中に DB を読まない）
- JSON Schema は draft 2020-12 なので、対応する検証ライブラリ（第一候補 jsoncons）を使う
- UART の変換（UartBridge）は core 上で行い、UART スレッドは COBS・CRC だけを扱う

## 0. 共通仕様 v0.2.2 との対応（改訂の要点）

作品03は「変換・調停・再送」の中心なので、共通仕様の決定の多くがここに集まる。変換の参照実装は iotspec（Python）の `iotspec.uart.bridge` にあり、C++ の実装は `uart/golden/` の期待エンベロープと一致させる。

| 対象 | v0.2.2 での決まり | 影響する章 |
| --- | --- | --- |
| 正とするもの | iot-common-spec v0.2.2 の `docs/spec.md`、`schemas/`、`profiles/`、`uart/golden/`。合否は契約テスト `iotspec-contract --role gw-01` で判定する | 全体 |
| デバイス登録 | `devices.yaml` は iotspec のデバイスプロファイルと同じ形（channels、actuators、states、modes、commands）。mcu-c-01 は同梱の `profiles/mcu-c-01.yaml` を使う。MQTT デバイスは state の channels・actuators で自動登録する | FR-01、第7章 |
| UART の受信と変換 | seq は方向・種別ごと。種別ごとに32ビットへ広げ、対応する kind の seq にそのまま使う。uptime\_ms の減少で再起動と判定し、STATE を受けるまで他のフレームは変換せず破棄（理由ごとに計数）。STATE → state はプロファイルで補い `bridged_by: gw-01` を付ける。TELEMETRY → telemetry（`device_state` 付き）。EVENT → event（`detail.from_state`・`to_state`。BOOT は `detail.reset_reason` だけ）。code 9・10 は INTERLOCK\_ACTIVE／RELEASED。DIAG は MQTT に変換せず（理由 not\_converted）、履歴と `gwctl` で使う | FR-02、第5章 |
| UART の生存監視 | HEARTBEAT を1秒ごとに送る。相手の HEARTBEAT が3秒途絶えたら offline と判定し、mcu-c-01 名義の offline の state を retain（`bridged_by` 付き）で発行する | FR-04、第8章 |
| 作品01への指令 | 汎用 cmd をプロファイルの commands で UART CMD に変換する。cmd\_ref（u16）と cmd\_id の対応表を持つ。ACK を 2秒待ち、同じ cmd\_ref で最大3回再送。**毎秒1件（再送を含み、再送を優先）・未完了4件まで**。expires\_at を過ぎた指令は UART に送らず rejected（expired）で終える。ttl\_s は set\_actuator のとき lease\_s（1〜600）、それ以外は0 | FR-09、第6章 |
| 指令全般 | cmd に priority・lease\_s（省略時60秒、上限600秒、set\_actuator のみ）・expires\_at・origin。ack は accepted／done／rejected／failed と limited・applied・reason。リースの更新は lease\_s の3分の1ごとに新しい cmd\_id で送る | 第6章 |
| 要求 | `request/ack` トピックで accepted（cmd\_id）／rejected（reason）／done／failed を返す。request\_id を1時間記憶し、重複には現在の状態を返す | FR-10、第6章 |
| データ品質 | 重複は（device\_id、boot\_id、kind、seq）で直近1,024個。stale は period\_ms の3倍超。offline の state（LWT を含む）は重複・欠損・遅延の判定から除外。`bridged_by` のデバイスが offline なら配下は状態不明 | 第6章 |
| イベント | 1回の遷移につき EVENT は1件（CR-5）。gw-01 は DEVICE\_OFFLINE／ONLINE、SYSTEM\_MODE\_CHANGED、RULE\_FIRED／CLEARED、COMMAND\_FAILED、CLOCK\_SKEW、DATA\_DROPPED、HOST\_RESOURCE\_HIGH／NORMAL を発行する | FR-08、第8章 |
| MQTT | ユーザー名は gw-01。cmd と request/ack を発行できるのは gw-01 だけ。送信量は kind ごとに平均毎秒10件・瞬間50件、QoS 1 の確認待ちは32件まで | NFR-07 |
| 契約テスト | `iotspec-contract --role gw-01`：ゴールデンとの照合（起動したばかりの対象が前提）、送信制限、request/ack、`bridged_by` 付きの offline の state、共通チェック | 第8章 |

特に注意する点：作品03は mcu-c-01 名義で telemetry・event・state を発行するが、ACL は「各デバイスは自分の device\_id のトピックだけ」を原則とする。gw-01 に配下デバイス名義の発行を許す設定が iot-common-spec の `deploy/mosquitto/` にあるかを、実装の最初に確認する。無ければ共通仕様の変更案として切り出す。

## 1. 概要とスコープ

複数のマイコンデバイスを登録・監視し、受け取ったイベントをルールで判定して他のデバイスへ指令を出す、IoTシステムの中核サービスをC++で開発する。Raspberry Pi上でsystemdの常駐サービスとして動かし、システム全体でアクチュエーターへの指令を出せる唯一の存在とする。

ポートフォリオとして示すことは3つ。マルチスレッドのイベント駆動サービスを安全に設計できること。通信断・異常終了からの復旧を設計とテストで示せること。Linuxの運用（systemd、journald、権限）まで含めて作れること。

### 前提

- プログラム先行で開発する。Linux PC（WSL可）で実行し、UARTはsocatの仮想シリアル、マイコンは共通仕様に沿ったデバイスシミュレータで代替する。Raspberry Piでの実行は実機フェーズで行う
- C++20、CMake。主なライブラリ：Eclipse Paho MQTT C++（自作MQTTアダプタ経由）、nlohmann/json、yaml-cpp、SQLite3、spdlog、GoogleTest
- 接続先：作品01（UART）、作品02（MQTT）、作品04（MQTTのrequest）。通信は共通仕様 v0.2.2 に従い、自身のデバイスIDは `gw-01`
- 役割の境界：時系列データの保存・分析・画面は作品04の責任。本作品はイベント・指令・要求の履歴（監査ログ）だけを保存する

### 対象外（本版）

- Web画面、REST API（作品04の責任）
- TCP直接接続のトランスポート、Prometheus形式のメトリクス公開、Yoctoによるイメージ化（拡張で対応）
- 複数台のRaspberry Piによる冗長化

## 2. 機能要件

| ID | 機能 | 要件 | 優先度 |
| --- | --- | --- | --- |
| FR-01 | デバイス登録 | 設定ファイルの静的登録と、stateトピックによる自動登録の両方に対応する。各デバイスはID、接続方式、持っている機器（チャネル）、状態を持つ | Must |
| FR-02 | UART取り込み | 作品01のUARTフレームを復号・検証し、受信時刻を付けて共通エンベロープに変換する。変換したデータは `mcu-c-01` 名義でMQTTにも転送し、作品04から見えるようにする | Must |
| FR-03 | MQTT取り込み | telemetry・event・state・cmd/ack・requestを購読し、スキーマで検証する。不正なメッセージは破棄して記録する | Must |
| FR-04 | 生存監視 | UARTはHEARTBEAT 3秒途絶、MQTTはLWTまたはstateのofflineで切断と判定する。状態は online／suspect／offline。変化時にeventを発行する | Must |
| FR-05 | 再接続 | UARTの抽き差しやMQTTブローカー停止から、指数バックオフ（1〜30秒）で自動復旧する | Must |
| FR-06 | 自己監視 | CPU温度・CPU使用率・メモリ・ストレージを10秒ごとに取得し、`gw-01` のtelemetryとして送る。しきい値超過はルールの入力になる | Must |
| FR-07 | ルールエンジン | YAMLで定義したルール（条件・継続時間・ヒステリシス・クールダウン・優先度・アクション）を評価する。SIGHUPで再読込し、検証に失敗したら旧ルールを維持する | Must |
| FR-08 | アクション | デバイスへの指令、システムモードの変更、通知イベントの発行、再接続の試行を実行できる | Must |
| FR-09 | 指令管理 | 指令ごとに発行→送信→受理→完了／失敗を追跡する。ACKが2秒以内に来なければ同じcmd\_idで最大3回再送する。継続する制御はリース期限前に更新する | Must |
| FR-10 | 要求の受付 | 作品04の要求（手動操作、スケジュール実行）を許可リストと優先度で判定し、指令に変換する。結果を要求元に返す | Must |
| FR-11 | 調停 | アクチュエーターごとに現在の「所有者」を管理し、優先度（安全ルール＞手動要求＞自動ルール＞スケジュール）で競合を解決する | Must |
| FR-12 | システムモード | NORMAL／ALERT／EMERGENCYの3段階。EMERGENCYではスケジュールと自動ルールの一部を停止し、冷却を最大にする | Must |
| FR-13 | 履歴 | イベント・指令の状態変化・要求・デバイス状態変化・ルール発火をSQLiteに保存する。保持期間90日 | Must |
| FR-14 | 状態公開 | `gw-01/state`（retain）に稼働状態、システムモード、全デバイスの状態を公開する | Must |
| FR-15 | 管理コマンド | `gwctl` でデバイス一覧、ルール一覧、指令の手動発行、ルール再読込を行える（UNIXドメインソケット経由） | Should |

## 3. 非機能要件と制約

| ID | 分類 | 要件 | 確認方法 |
| --- | --- | --- | --- |
| NFR-01 | 応答性 | イベント受信から指令送信まで、99パーセンタイルで100ms以内（毎秒100メッセージの負荷時） | 負荷生成ツールで計測し結果をREADMEに載せる |
| NFR-02 | 処理量 | 毎秒200メッセージを1時間処理してもキューが満杯にならない | 負荷試験 |
| NFR-03 | 資源 | 常駐メモリ（RSS）64MB以内、通常負荷でCPU使用率10%以内（Raspberry Pi 5想定） | 負荷試験、実機フェーズで実測 |
| NFR-04 | 復旧性 | 異常終了から5秒以内に再起動し、直前に確定した履歴を失わない | プロセスkill試験 |
| NFR-05 | スレッド安全 | ThreadSanitizer、AddressSanitizer、UBSanで全テストが警告なし | CIで各サニタイザのビルドを実行 |
| NFR-06 | 起動 | 起動からメッセージ処理開始まで3秒以内。設定の誤りは起動時に検出して理由をログに出して終了する | 結合テスト |
| NFR-07 | セキュリティ | root以外の専用ユーザーで実行（UART用にdialoutグループのみ）。MQTTはユーザー認証とACLで、作品04はcmdを発行できないようにする | MosquittoのACL設定と試験 |
| NFR-08 | 可観測性 | journaldへ構造化ログを出す。各メッセージと指令にmsg\_id／cmd\_idを付けて追跡できるようにする | ログで経路を辿るテスト |
| NFR-09 | 品質 | coreライブラリの行カバレッジ80%以上。clang-tidyで警告ゼロ | CI（gcovr） |
| NFR-10 | 移植性 | x86\_64とaarch64の両方でビルドできる。Raspberry Pi固有の処理はシステム監視の入力元だけに限る | CIでaarch64向けクロスビルド |

制約：開発段階では実機を使わない。NFR-03のRaspberry Pi上での実測は実機フェーズで行い、それまではPCでの計測値を参考値として扱う。

## 4. アーキテクチャ

受信するスレッドは種類ごとに分け、すべてを一つの受信キューに集める。判定・調停・指令管理はcoreスレッドが順に処理し、送信と履歴の書き込みは別スレッドに任せる。

&#91;embedded content: スレッドとキューの構成\]

この形にすると、DBの書き込みが遅れても判定は止まらず、ブローカーが落ちてもUART経由の制御は続く。coreの処理は「キューから1件取り出し、状態を更新し、結果をキューに入れる」だけなので、スレッドなしでも単体テストできる。

## 5. 主要コンポーネントとスレッド設計

判定と状態の更新は「core」スレッド1本だけが行う。受信・送信・永続化は別スレッドで、スレッド間は境界付きキューだけでつなぐ。デバイス状態やルールの状態はcoreスレッドが単独で持つため、ロックが不要になり、デッドロックと競合を設計で排除できる。

### スレッド

| スレッド | 役割 | 入力 | 出力 |
| --- | --- | --- | --- |
| `uart-rx` | シリアル読み取り（poll）、フレーム復号、共通エンベロープへの変換 | シリアルデバイス | インバウンドキュー |
| `mqtt`（Pahoのコールバック） | 購読メッセージの検証と変換、接続状態の通知 | ブローカー | インバウンドキュー |
| `core` | デバイス管理、ルール評価、調停、指令管理、システムモード。タイマーもここで処理 | インバウンドキュー、タイマー | アウトバウンドキュー、履歴キュー |
| `egress` | MQTT発行とUART送信 | アウトバウンドキュー | ブローカー、シリアル |
| `history` | SQLiteへのバッチ書き込み（100件または200msごと）、期限切れデータの削除 | 履歴キュー | DBファイル |
| `sysmon` | CPU温度・使用率・メモリ・ストレージの取得（10秒周期） | `/sys`、`/proc`、statvfs | インバウンドキュー |
| `admin` | `gwctl` 用のUNIXドメインソケット | ソケット | インバウンドキュー（管理要求として） |

キューは容量固定。満杯時の方針は種類ごとに決める：telemetryは古いものから捨てる、event・cmd/ack・requestは捨てずに送信側を待たせる。破棄数はメトリクスとして公開する。終了時は「受信停止 → coreがキューを空にする → 履歴を書き切る → stateをofflineにして切断」の順で止める（SIGTERM）。

### 主要クラス

| クラス | 責務 |
| --- | --- |
| `ITransport`（`UartTransport`, `MqttTransport`） | 接続・再接続・送受信。上位には共通エンベロープだけを渡す |
| `FrameCodec` | UARTフレームのCOBS／CRC処理と共通エンベロープへの相互変換 |
| `EnvelopeValidator` | 共通仕様のJSON Schemaによる検証 |
| `DeviceRegistry` | デバイスとそのチャネル、生存状態、最新値の保持 |
| `LivenessMonitor` | ハートビートとLWTから生存状態を判定 |
| `RuleEngine` / `Rule` / `Condition` / `Action` | ルールの保持と評価（第6章） |
| `Arbiter` | アクチュエーターごとの所有者と優先度の管理 |
| `CommandManager` | 指令のライフサイクル、再送、リース更新 |
| `RequestHandler` | 作品04からの要求の認可と指令への変換 |
| `SystemModeController` | NORMAL／ALERT／EMERGENCYの遷移とその影響範囲の適用 |
| `HistoryStore` | SQLiteへの書き込みと保持期間の管理 |
| `TimerService` | coreスレッド上のタイマー（継続時間判定、ACK待ち、リース更新）。テストでは仮想時計に差し替える |

時刻はすべて `IClock` 経由で取得する。継続時間やタイムアウトのテストを、実時間を待たずに決定的に実行するためである。

## 6. ルールエンジン・指令管理・調停

### ルール定義

ルールはYAMLで書き、コードを変更せずに追加・調整できる。構想に挙げたイベント／アクションはすべてこの形で書ける。

```yaml
rules:
  - id: cool-on-high-temp
    priority: auto            # safety | manual | auto | schedule
    when:
      any:
        - { device: mcu-c-01,   channel: temp_board, op: ">=", value: 45.0 }
        - { device: mcu-cpp-01, channel: temp_air,   op: ">=", value: 30.0 }
    for: 10s                  # 条件が10秒続いたら発火
    clear_when_below: 2.0     # 解除はしきい値から2.0下で（ヒステリシス）
    then:
      - { action: command, device: mcu-cpp-01, op: set_actuator,
          args: { id: fan_main, level: 800 }, lease: 60s, hold: true }
    cooldown: 30s

  - id: emergency-on-sustained-heat
    priority: safety
    when: { device: mcu-c-01, channel: temp_board, op: ">=", value: 55.0 }
    for: 60s
    then:
      - { action: set_system_mode, mode: EMERGENCY }
      - { action: notify, level: critical, message: "高温状態が60秒継続" }

  - id: reconnect-on-offline
    priority: auto
    when: { device: "*", status: offline }
    then:
      - { action: reconnect }
      - { action: notify, level: warning }
```

- 条件は `all`／`any`／`not` で組み合わせられる。参照できるのは各チャネルの最新値、デバイスの生存状態、システムモード、時刻帯
- 評価は「値が更新されたチャネルを参照するルールだけ」を対象にする（チャネル→ルールの索引を起動時に作る）。qualityがgoodでない値では発火しない
- ルールの状態は idle → pending（条件成立、継続時間待ち）→ active（発火済み）→ cooldown → idle。`hold: true` の指令はactiveの間リースを更新し続け、解除時に `release` を送る
- 再読込時は新しいファイル全体を検証し、成功した場合だけ入れ替える。activeのルールは解除処理をしてから入れ替える

### 時間条件と値の鮮度

「値が届いたときだけ評価する」だけでは、継続時間（`for`）や値の途絶を扱えない。そのため、値の更新に加えてタイマーでも評価する。

1. 条件が成立したら pending にし、`for` の経過時刻にタイマーを登録する。タイマーが鳴ったら、その時点の最新値で条件を評価し直し、成立していれば発火する
2. pending 中に条件が不成立になったらタイマーを取り消して idle に戻す
3. 参照する値が stale（最終更新から `period_ms` の3倍超、共通仕様の第5章）になったら、ルールは「不明」とする。pending なら idle に戻し、active なら新しい指令を出さずリースの更新も止める。デバイスは期限切れで自律制御に戻る
4. stale の判定は1秒周期のタイマーで行う。値の途絶に気づくのは最大で「周期×3＋1秒」後

タイマーはすべてcoreスレッドの `TimerService` で処理し、テストでは仮想時計を進めて「9.9秒では発火しない」「10秒で発火する」「途中で値が途絶えたら発火しない」を確かめる。

### 調停（アクチュエーターの所有者管理）

`Arbiter` はアクチュエーターごとに「今どの経路が制御しているか」を保持する。新しい要求は、優先度が現所有者以上なら受け入れて所有者を入れ替え、低ければ理由を付けて拒否する。所有者が解放すると、待ちの中で最も優先度の高い要求が引き継ぐ。

| 優先度 | 発生元 | 例 |
| --- | --- | --- |
| 1. safety | 安全ルール、EMERGENCYモード | 高温持続で全ファン最大 |
| 2. manual | 作品04の手動操作、`gwctl` | 利用者が照明を点灯 |
| 3. auto | 通常のルール | 照度低下で照明を点灯 |
| 4. schedule | 作品04のスケジュール実行 | 毎朝7時に給水 |

マイコン側の安全インターロック（作品01・02）はこの調停よりさらに上位にあり、本作品からは解除できない。制限された場合はackの `limited` で知り、履歴に残す。

### 指令のライフサイクル

| 状態 | 意味 | 次の状態への条件 |
| --- | --- | --- |
| ISSUED | 調停を通過しcmd\_idを採番 | アウトバウンドキューに入ったらSENT |
| SENT | 送信済み、ACK待ち | acceptedでACCEPTED、rejectedでREJECTED、2秒無応答で再送（最大3回）、上限でTIMED\_OUT |
| ACCEPTED | デバイスが受理 | doneでDONE、failedでFAILED |
| DONE ／ REJECTED ／ FAILED ／ TIMED\_OUT | 終端状態 | 履歴に確定し、要求元（ルール／作品04）に結果を返す |

デバイスがofflineのときは送信せず、その場でFAILED（理由：offline）とする。復帰後に古い指令をまとめて送ることはしない。必要な制御はルールが現在の値で判定し直す。

重複排除：作品04は応答がなければ同じ `request_id` で要求を再送する。`RequestHandler` は `request_id` を1時間記憶し、重複した要求には新しい指令を作らず現在の状態を `request/ack` で返す。履歴の `requests` テーブルを参照するので、再起動をまたいだ重複も弾ける。デバイスへの再送は常に同じ `cmd_id`（UARTは同じ `cmd_ref`）を使い、デバイス側の重複排除に任せる。

送信の制限：作品01が覚えておける指令番号は16件なので、`CommandManager` は作品01への指令を毎秒1件まで、未完了は同時4件までに制限する。超えた分は送信待ちにし、同じ対象への古い指令は新しい指令で置き換える（共通仕様 v0.2 第4章）。リースの更新は lease\_s の3分の1ごとに行う。

### システムモード

| モード | 入る条件（例） | 影響 |
| --- | --- | --- |
| NORMAL | 起動時、復旧条件を満たしたとき | 制限なし |
| ALERT | WARNING相当のイベント、デバイスoffline | スケジュール実行を保留、通知を強化 |
| EMERGENCY | 安全ルールの発火 | 冷却を最大、スケジュールとautoルールを停止。解除は条件回復＋作品04または `gwctl` での承認 |

## 7. データ設計

### 履歴DB（SQLite、WALモード）

書き込むのは `history` スレッドだけ。`synchronous=NORMAL` とし、異常終了でも確定済みトランザクションは失わない。時刻はすべてUTCのミリ秒。

| テーブル | 主な列 | 用途 |
| --- | --- | --- |
| `devices` | device\_id（PK）, transport, channels（JSON）, first\_seen, last\_seen, status | デバイス台帳。再起動後の初期状態 |
| `device_status_log` | id, device\_id, from\_status, to\_status, reason, ts | 生存状態の変化 |
| `events` | msg\_id（PK）, device\_id, code, severity, payload（JSON）, ts | 受信・発行したイベント |
| `rule_firings` | id, rule\_id, state\_from, state\_to, trigger（JSON）, ts | ルールの発火と解除 |
| `commands` | cmd\_id（PK）, device\_id, op, args（JSON）, origin, priority, state, retries, created\_ts, updated\_ts | 指令とその最終状態 |
| `command_transitions` | id, cmd\_id, from\_state, to\_state, detail, ts | 指令の状態遷移（監査用） |
| `requests` | request\_id（PK）, source, kind, body（JSON）, result, reason, cmd\_id, ts | 作品04からの要求とその結果 |
| `system_mode_log` | id, from\_mode, to\_mode, reason, ts | システムモードの変化 |

保持期間90日を過ぎた行は毎日3時に削除する。スキーマは `PRAGMA user_version` で版管理し、起動時に移行スクリプトを順に適用する。

### 設定ファイル

| ファイル | 内容 | 再読込 |
| --- | --- | --- |
| `/etc/iot-gw/gateway.yaml` | site名、MQTT接続先と認証情報の参照先、UARTデバイスパスと速度、キュー容量、タイムアウト、DBパス | 再起動が必要 |
| `/etc/iot-gw/devices.yaml` | 静的に登録するデバイスと、汎用指令から各デバイス固有の指令への対応表 | SIGHUP |
| `/etc/iot-gw/rules.yaml` | ルール（第6章） | SIGHUP |
| `/etc/iot-gw/requests.yaml` | 作品04に許可する操作の一覧 | SIGHUP |

対応表の例：汎用指令 `set_actuator {id: fan_main, level: 800}` を、作品01に対してはUARTの `SET_FAN_OVERRIDE(duty=800‰, 有効秒数=lease)` に変換する。ルールは汎用指令だけで書けばよく、デバイスごとの違いを意識しない。パスワードは設定ファイルに書かず、systemdの資格情報機能（`LoadCredential=`）で渡す。

### 共通仕様への変更提案（v0.2に反映済み）

本作品の設計で共通仕様 v0.1 に足りないと分かった点。実装で吸収せず、`iot-common-spec` に変更として反映する。

- [ ] `request/ack` トピックを追加する（03 → 04、QoS 1）。要求の受理／拒否と理由、対応するcmd\_id、最終結果を返す
- [ ] cmdの共通項目に `lease_s` と `priority` を加える。`release` 操作を定義する
- [ ] cmd/ackの結果値に `limited`（デバイス側の安全制限で丸めた）を加える
- [ ] UARTのHEARTBEATを双方向とし、上位側は1秒周期で送る（作品01の上位断検知に使う）
- [ ] stateペイロードに機器（チャネル）一覧を含める形式を決める

## 8. 障害対策・運用・テスト方針・未決事項

### 障害と対応

| 障害 | 検知 | 対応 |
| --- | --- | --- |
| MQTTブローカー停止 | 接続断のコールバック | 指数バックオフで再接続。その間もUART経由の判定と指令は継続。復帰時にstateを再公開し、購読をやり直す |
| UARTの抽き差し | readエラー、デバイスファイル消失 | ポートを閉じて周期的に再オープン。mcu-c-01をofflineにしてevent発行 |
| デバイス無応答 | ハートビート途絶、ACKタイムアウト | suspect → offline。そのデバイス宛の新規指令は即FAILED |
| 不正・破損メッセージ | スキーマ検証、CRC | 破棄してカウンタ加算。同一送信元から大量ならログを抑制 |
| キュー満杯 | 投入失敗 | telemetryは破棄、重要メッセージは待機。破棄数を公開しALERT候補とする |
| DB書き込み失敗 | SQLiteのエラー、ディスク残量不足 | メモリ上に一時保持（上限あり）して再試行。制御は止めない。ALERTへ |
| プロセスのハングアップ | systemdウォッチドッグの通知漏れ | coreスレッドが健全なときだけ `WATCHDOG=1` を送る。途絶えたらsystemdが再起動 |
| 異常終了 | — | `Restart=on-failure` で再起動。起動時に未完了だった指令はFAILED（理由：restart）で閉じる。マイコン側はリース切れで自律制御に戻る |

履歴の書き込み失敗時にメモリ上で保持するのは最大1万件。上限を超えたら、重要度の低いものから破棄する（infoイベント → ルール発火 → 指令の途中の状態遷移の順。指令と要求の最終状態は最後まで残す）。破棄した件数は `DATA_DROPPED` イベントで知らせ、復旧後に「この期間の履歴は欠けている」ことを `system_mode_log` に記録する。

### systemdユニット（概要）

```ini
[Unit]
Description=IoT Gateway
After=network-online.target mosquitto.service
Wants=network-online.target

[Service]
Type=notify
User=iotgw
SupplementaryGroups=dialout
ExecStart=/usr/local/bin/iot-gw --config /etc/iot-gw/gateway.yaml
ExecReload=/bin/kill -HUP $MAINPID
WatchdogSec=10s
Restart=on-failure
RestartSec=2s
LoadCredential=mqtt_password:/etc/iot-gw/credentials/mqtt_password
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/var/lib/iot-gw
ProtectHome=yes

[Install]
WantedBy=multi-user.target
```

### テスト方針

| レベル | 対象 | 手段 | 主な観点 |
| --- | --- | --- | --- |
| 単体 | coreライブラリの各クラス | GoogleTest、仮想時計 | ルールの継続時間・ヒステリシス・クールダウン、調停の全優先度組み合わせ、指令ライフサイクルと再送、UARTコーデック |
| 結合 | プロセス全体 | DockerのMosquitto、socatの仮想シリアル、Python製デバイスシミュレータ、pytestでシナリオ実行 | 構想のイベント／アクション6種をすべてシナリオ化 |
| 障害注入 | プロセス全体 | ブローカー停止、socat切断、破損フレーム投入、SIGKILL、ディスク満杯 | 第8章の表どおりに復旧すること。結果を表でREADMEに載せる |
| 負荷 | プロセス全体 | 負荷生成ツール（毎秒100〜500メッセージ） | NFR-01〜03 |
| 契約 | 送受信メッセージ | `iot-common-spec` のJSON Schemaとサンプル | 作品01・02・04との解釈一致 |

### 受け入れ基準（MVP）

- [ ] FR-01〜14がテスト仕様書に紐付き、結合テストで合格している
- [ ] 障害注入の全項目で復旧を確認し、結果を公開している
- [ ] サニタイザとカバレッジ（NFR-05・09）、aarch64ビルド（NFR-10）をCIで確認できる

* [ ] `iotspec-contract --role gw-01`（iot-common-spec v0.2.2）の全チェックが合格し、CI（Mosquitto をサービスで起動）でも実行されている
* [ ] `uart/golden/` の全件を順に通したとき、C++ の変換結果が期待エンベロープと一致する（msg\_id・ts は固定の生成器と仮想時計で揃える）
* [ ] DIAG を受けたら MQTT には流さず、履歴に残して `gwctl diag mcu-c-01` で表示できる

### 未決事項

- [ ] MQTTクライアントをPaho C++とlibmosquittoのどちらにするか（自作MQTTアダプタの対応状況で決める）
- [ ] イベントループを自作のキュー＋タイマーとするか、Asioを使うか。学習効果を重視するなら自作、実績を重視するならAsio
- [ ] 共通仕様 v0.2 の変更提案（第7章）をどの時点で確定させるか。作品01・02のMVPに影響するので、できれば作品01の着手前

* [ ] （改訂で追加）gw-01 が mcu-c-01 名義で発行できる ACL が iot-common-spec にあるか。無ければ共通仕様 v0.2.x の変更案にする
* [ ] （改訂で追加）DIAG を履歴 DB のどのテーブルに持つか（推奨：`uart_diag` を新設し、受信時刻・boot\_id・15バイトの各項目を列にする。保持90日）
* [ ] （改訂で追加）変換を C++ で書き直すので、iotspec の Python 参照実装との差をどう検知し続けるか（推奨：ゴールデン照合を CI に置き、iot-common-spec の版を上げるたびに実行する）
