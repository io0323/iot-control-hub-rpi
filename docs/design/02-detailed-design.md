# 作品03 詳細設計書：IoTデバイス統合管理・監視システム（C++ × Raspberry Pi）

Nov 26, 2026 · @io

要件定義・基本設計書（v0.2.2 改訂版）を実装できる粒度に落とし、Claude Code に渡すフェーズ分割を決める。共通仕様は iot-common-spec v0.2.2 を正とし、タグが出るまでは Phase S でそれを作る。

## 1. 前提と決定事項

2026-11-26 に次の6点を決めた。要件定義・基本設計書と食い違う場合は本書を優先する。

| # | 決定 | 基本設計からの変更 |
| --- | --- | --- |
| D1 | iot-common-spec の v0.2.2 を先にリリースする（Phase S）。現在のタグは v0.2.1（e6280d0）だけで、第11章の CR-1〜5 はリポジトリに未反映 | 作品03 の実装はタグ v0.2.2 が出てから始める |
| D2 | MQTT は自作の `IMqttClient` と libmosquitto の実装 | Paho MQTT C++ を使わない |
| D3 | イベントループは自作（有界キュー＋最小ヒープのタイマー＋IClock）。Asio は使わない | 未決事項を閉じる |
| D4 | core はどのキューにも待たずに入れる（try\_push だけ）。アウトバウンドは UART と MQTT で分ける | 「重要メッセージは送信側を待たせる」は受信スレッド→インバウンドの向きだけに限る。ブローカー停止中の保持と破棄は egress の責任 |
| D5 | request\_id の重複判定は core のメモリだけで行う。起動時に直近1時間の requests と未完了の commands を読み込む | core は実行中に DB を読まない |
| D6 | JSON Schema の検証は draft 2020-12 に対応するライブラリ（第一候補 jsoncons）。Phase 2 の最初に iot-common-spec の samples/valid・invalid 全件で比べて確定する | pboettch/json-schema-validator は draft-07 までなので使わない |

要件定義・基本設計書の第1〜8章に残る socat・WSL・Python 製シミュレータ・Paho の記述は古い。UART の相手は契約テストが作る pty と iotspec-sim の mcu-c-01 代役、作品01 のホスト版である。

ACL は変更不要。deploy/mosquitto/acl で gw-01 は mcu-c-01 の telemetry・event・state・cmd/ack、全デバイスの cmd、app-01 の request/ack を発行できる。

## 2. リポジトリ構成とモジュール

判定のロジックは OS に触れない `gw_core` に集め、入出力はアダプタに押し出す。依存の向きは app → adapters → core の一方向で、core はインターフェース（IClock、IIdGenerator など）だけを知る。

```
iot-control-hub-rpi/
├─ CMakeLists.txt / CMakePresets.json   # debug, release, asan, tsan, ubsan, coverage, aarch64
├─ cmake/IotSpec.cmake                  # iot-common-spec の版（IOTSPEC_REF=v0.2.2）を取得
├─ src/
│  ├─ core/       # gw_core：OS 非依存。macOS でも全テストが通る
│  │  ├─ common/   # Envelope, Ulid, BootId, Clock(IClock), BoundedQueue, TimerService
│  │  ├─ uart/     # Cobs, Crc, FrameCodec, SeqExtender, UartBridge, Profile
│  │  ├─ quality/  # DedupWindow, StaleTracker
│  │  ├─ device/   # DeviceRegistry, LivenessMonitor
│  │  ├─ rules/    # RuleEngine, Rule, Condition, Action（Phase 4）
│  │  ├─ command/  # Arbiter, CommandManager, RequestHandler（Phase 3）
│  │  └─ Core.{h,cpp}  # step(Inbound) と on_timer(now)。スレッドを持たない
│  ├─ adapters/   # gw_adapters：OS・外部ライブラリに触れる
│  │  ├─ serial/   # SerialPort（termios、poll）
│  │  ├─ mqtt/     # IMqttClient, MosquittoClient, FakeMqttClient
│  │  ├─ history/  # HistoryStore（SQLite）
│  │  ├─ platform/ # INotifier, ISysInfoSource, Signals
│  │  └─ admin/    # gwctl 用の UNIX ドメインソケット
│  └─ app/        # main.cpp、各スレッドの組み立て、設定の読み込み
├─ tools/gwctl/
├─ config/        # gateway.yaml, devices.yaml, rules.yaml, requests.yaml の例
├─ deploy/        # systemd ユニット、Dockerfile、docker-compose.yml
├─ tests/
│  ├─ unit/       # GoogleTest。仮想時計
│  ├─ golden/     # uart/golden を読む照合テスト
│  └─ integration/  # pytest：iotspec-sim、契約テスト、障害注入
└─ docs/          # requirements, basic-design, detailed-design, open-questions, phases/
```

- 外部ライブラリ：ncore が使うのは nlohmann/json と yaml-cpp（プロファイル・設定・ルールの読み込み）だけ。libmosquitto、SQLite、スキーマ検証は adapters に閉じる
- ログは spdlog（同梱の fmt）。`std::format`・`std::jthread` は使わない（GCC 12 と AppleClang の差を避ける）
- 名前空間は `gw::core`、`gw::adapters`、`gw::app`

## 3. スレッドとキュー

受信から core への向きだけが待ちを許し、core から先は待たずに入れる（D4）。これでブローカーが止まっても、DB が遅れても、UART の判定と指令は止まらない。

&#91;embedded content: スレッドとキュー · 7スレッド、4キュー\]

UART の送受信は1本の uart-io スレッドが担う（fd と uart-out の起床用パイプを poll）。基本設計の egress は mqtt-tx と uart-io に分かれる。

| キュー | 向き | 容量 | 満杯のとき |
| --- | --- | --- | --- |
| inbound | 受信各スレッド → core | 4096 | telemetry は古い telemetry を追い出す。それ以外は送り手が待つ。mqtt-loop だけは待つのを最大200msとし、超えたら破棄して数える（keepalive を止めないため） |
| uart-out | core → uart-io | 64 | 入らない CMD は CommandManager の送信待ちに残す。HEARTBEAT はその回を飛ばす |
| mqtt-out | core → mqtt-tx | 4096 | 古い telemetry を追い出す。telemetry が無いときは新しい方を捨て、DATA\_DROPPED の対象にする |
| history | core → history | 8192 | 破棄の順は info イベント → ルール発火 → 指令の途中の遷移。指令と要求の最終状態は最後まで残す |

- 容量は gateway.yaml で変えられる。破棄数はキューごと・種別ごとに数え、gw-01 の telemetry と gwctl で公開する
- mqtt-tx は切断中、state をトピックごとの最新の1件だけ保持し、復帰時に再公開する。event・cmd・ack は最大2000件保持し、telemetry は捨てる（数える）
- 終了（SIGTERM）：受信停止 → core が inbound を空にする → history を書き切る → mqtt-tx が gw-01 の offline の state を送り切る → 切断

## 4. core の処理

core はスレッドを持たないクラスで、「入力1件または時刻を受け、状態を更新し、出力を返す」だけを行う。スレッドの組み立ては app の `CoreRunner` が担い、テストは Core を直接呼ぶ。

```cpp
struct Outputs { std::vector<UartTx> uart; std::vector<MqttTx> mqtt; std::vector<HistoryRecord> history; };

class Core {
 public:
  Core(const Config&, IClock&, IIdGenerator&);
  Outputs step(const Inbound& msg);           // 受信・管理要求・接続状態の変化
  Outputs on_timer(Instant now);              // 期限の来たタイマーをすべて処理
  std::optional<Instant> next_deadline() const;
};

// CoreRunner（app）のループ
while (!stopping) {
  auto msg = inbound.pop_until(core.next_deadline());  // 条件変数で待つ
  Outputs out = msg ? core.step(*msg) : core.on_timer(clock.steady_now());
  dispatch(out);                                        // 各キューに try_push
  watchdog.kick_if_healthy();
}
```

### 時刻と ID

- `IClock` は2つの時刻を返す。`steady_now()`（継続時間・タイムアウト用、単調増加）と `system_now()`（エンベロープの ts、UTCミリ秒）。`VirtualClock` は両方を手で進める
- `IIdGenerator` は ULID（msg\_id・cmd\_id）を返す。本番は単調 ULID。テスト用に gen\_golden.py と同じ規則（時刻部＝仮想時刻、乱数部＝通し番号）の `SequentialIdGenerator` を用意する
- 自分の boot\_id は起動ごとに乱数で16進小文8桁。seq は発信元（gw-01、mcu-c-01）と kind ごとに持ち、offline の state では進めない

### TimerService

最小ヒープ。`schedule(at, TimerKey)` と `cancel(TimerKey)` を持ち、取り消しは世代番号で無効化する（ヒープからは消さない）。タイマーの種類は次のとおり。

| タイマー | 周期・期限 | フェーズ |
| --- | --- | --- |
| UART HEARTBEAT 送信 | 1秒 | 1 |
| UART 生存確認（途絶えの判定） | 最後のフレーム受信＋3秒（Q3） | 1 |
| gw-01 の state 再公開・sysmon の取り込み | 必要に応じて・10秒 | 1・6 |
| stale の判定 | 1秒 | 2 |
| ルールの for・cooldown | ルールごと | 4 |
| ACK 待ち・リース更新・UART 送信の毎秒1件 | 2秒・lease\_s/3・1秒 | 3 |
| request\_id の忘却 | 1時間 | 3 |

### 起動の手順

1. 設定4ファイルを読み、検証する。誤りがあれば理由をログに出して終了コード 2 で終わる
2. DB を開き、移行を適用する。未完了の指令を FAILED（restart）で閉じ、直近1時間の requests とデバイス台帳を読んで Core に渡す（D5）
3. 各スレッドを起動し、MQTT の接続と購読が終わったら `READY=1` を送る（接続できなくても3秒で READY にし、再接続は続ける。UART だけでも動けるようにする）

ウォッチドッグは「core のループが WatchdogSec の半分以内に1周した」ときだけ `WATCHDOG=1` を送る。pop\_until の待ちは最大1秒で切り上げる。

## 5. UART

uart-io スレッドはバイト列とフレームの相互変換（COBS・CRC）だけを行い、エンベロープへの変換は core 上の `UartBridge` が行う。基本設計の「uart-rx が変換する」からの変更で、理由は次の3つ。

- STATE 待ち・GET\_STATUS の要求・cmd\_ref の対応表・生存状態は、どれも指令管理と絡む core の状態である
- Python の `UartBridge` と同じ形の1台分の状態を持つクラスにでき、コードを1対1で読み比べられる
- ゴールデン照合をスレッドなしで決定的に行える

### クラス

| クラス | 置き場所 | 責務 | 対応する Python |
| --- | --- | --- | --- |
| `Cobs` / `Crc16` | core/uart | 符号化と検査 | `uart/cobs.py`・`crc.py` |
| `FrameCodec` | core/uart | バイト列 ⇔ `Frame`（variant：State, Telemetry, Event, Heartbeat, Ack, Diag, Cmd）。壊れたものは `Discarded{reason}` | `uart/frames.py` |
| `SeqExtender` | core/uart | 種別ごとの16ビット seq を32ビットへ | `bridge.SeqExtender` |
| `Profile` | core/uart | profiles/\*.yaml の読み込み。状態番号・汎用指令の対応 | `profile.py` |
| `UartBridge` | core/uart | 受信フレーム → `BridgeResult{envelope?, discard_reason?, request_state, dropped_telemetry}`、cmd → `Cmd` フレーム、offline の state | `bridge.UartBridge` |
| `SerialPort` | adapters/serial | open・raw 設定・read・write・再オープン | — |

### 受信の流れ

1. uart-io が `poll` で読んだバイトを 0x00 で区切り、`FrameCodec::decode` する。受信時刻（system\_now）を付けて inbound に入れる
2. core が `UartBridge::receive` を呼ぶ。エンベロープは mqtt-out と DeviceRegistry へ。DIAG は変換せず履歴（uart\_diag）へ
3. `request_state` が立ったら GET\_STATUS を uart-out へ。Phase 1 は毎秒1件の上限だけ守る最小の送信器で送り、再送と未完了4件の制限は Phase 3 の CommandManager に任せる
4. 破棄は理由ごとに数える（crc\_mismatch、awaiting\_state、unknown\_cmd\_ref、unknown\_value、not\_converted など）

### 送信と接続

- HEARTBEAT は core の1秒タイマーで作り、uart-out に入れる。uptime\_ms は自分の起動からの steady 時間
- 書き込みは uart-io が行う。uart-out に入ったら起床用パイプに1バイト書いて poll を起こす
- read が EIO・ENXIO・0バイト（相手が閉じた）を返したら切断とみなし、ポートを閉じて core に `UartLinkDown` を送る。再オープンは指数バックオフ（1・2・4…最大30秒）
- 設定は 115200bps、8N1、raw。速度は gateway.yaml。pty では速度設定の失敗を無視する

### 数値の扱い

温度は Python と同じく `temp_c100 / 100.0` で求める（`* 0.01` は使わない）。JSON の出力は nlohmann/json の最短表現に任せ、比較はパース後の値で行う。対応表（状態・イベントコード・重大度・品質・reset\_reason）は `uart/mapping.py` と同じ内容を `core/uart/Mapping.h` に持ち、第8章の仕組みでずれを検知する。

## 6. MQTT

上位は `IMqttClient` だけを知り、実装は libmosquitto の `MosquittoClient` とテスト用の `FakeMqttClient`（D2）。トピックと QoS・retain の決め方は iotspec の constants と同じ表を `core/common/Topics.h` に持つ。

```cpp
class IMqttClient {
 public:
  struct Will { std::string topic; std::string payload; int qos; bool retain; };
  virtual void connect(const ConnectOptions&, const Will&) = 0;   // 非同期。結果は on_connection
  virtual PublishResult publish(std::string_view topic, std::string_view payload, int qos, bool retain) = 0;
  virtual void subscribe(std::string_view filter, int qos) = 0;
  virtual void on_message(std::function<void(MqttMessage)>) = 0;
  virtual void on_connection(std::function<void(ConnectionEvent)>) = 0;
  virtual void on_published(std::function<void(int mid)>) = 0;   // QoS 1 の PUBACK
  virtual ~IMqttClient() = default;
};
```

### 接続

- client id とユーザー名は gw-01。パスワードは `$CREDENTIALS_DIRECTORY/mqtt_password`（systemd）、無ければ環境変数 `IOTSPEC_MQTT_PASSWORD`（開発・CI）
- LWT は gw-01 の offline の state（retain、QoS 1、seq を進めない）
- 接続ができたら：購読（`iot/v1/{site}/+/telemetry`、event、state、cmd/ack、`app-01/request`）→ gw-01 と UART デバイスの state を再公開 → 保持していた event・cmd・ack を送る
- 切断したら libmosquitto の再接続（`reconnect_delay_set(1, 30, true)`）に任せ、core に `MqttLinkDown` を送る
- 自分が発行した電文は購読で戻ってくるので、device\_id が gw-01 または bridged のデバイスのものは mqtt-loop で捨てる

### 送信制限（mqtt-tx）

- kind ごとのトークンバケット：平均毎秒10件、容量50。バケットは発信元（gw-01、mcu-c-01）× kind ごとに持つ（Q5、publish\_rate チェックと同じ単位）
- QoS 1 の確認待ちは32件（`MOSQ_OPT_SEND_MAXIMUM` と `max_inflight_messages_set`）。超えたら mqtt-tx が PUBACK を待つ
- バケットが空のとき：telemetry は古いものから捨て、それ以外は待つ（mqtt-out があふれたら第3章の方針）

### 受信（mqtt-loop）

コールバックでは JSON のパースとスキーマ検証だけを行い、`Inbound::Mqtt{topic, envelope, received_at, retained}` を inbound に入れる。不正なものは理由ごとに数えて捨て、同じ発信元からのログは10秒に1回に抑える。重複・stale・欠番の判定は core（Phase 2）。

## 7. プラットフォームの抽象化

OS の差は adapters/platform と adapters/serial に閉じる。`#ifdef __linux__` を書いてよいのはこの2か所だけとし、core には置かない。

| 差 | インターフェース | Linux | macOS | テスト |
| --- | --- | --- | --- | --- |
| systemd の通知 | `INotifier`（ready, watchdog, stopping, status） | `$NOTIFY_SOCKET` へ AF\_UNIX データグラムを自作で送る（抽象名前空間の `@` に対応） | 同じコード。変数が無ければ何もしない | 単体：一時ソケットを作って受信内容を確かめる |
| 温度・CPU・メモリ | `ISysInfoSource` | `ProcfsSysInfo(root)`：`{root}/sys/class/thermal/thermal_zone0/temp`、`/proc/stat`、`/proc/meminfo` | `FakeSysInfo`（固定値・設定で変更） | `ProcfsSysInfo` に fixture のディレクトリを渡し、macOS でも実行する |
| ストレージ | 同上 | `statvfs` | `statvfs` | — |
| シグナル | `SignalWatcher` | 全スレッドで SIGTERM・SIGINT・SIGHUP をブロックし、専用スレッドの `sigwait` で受けて inbound へ | 同じ | 結合：SIGHUP・SIGTERM を送る |
| シリアル | `SerialPort` | termios、`cfmakeraw`、`poll` | 同じ。切断時の read の結果が違うので EIO・ENXIO・0 をすべて切断とする | 単体：`openpty` の対で送受信と切断を確かめる |
| ログ | spdlog | 標準エラー出力に1行1イベント（`key=value`、msg\_id・cmd\_id 付き）。journald が拾う | 同じ | — |

役割分担：macOS では単体テストとゴールデン照合まで。結合テスト・契約テスト・障害注入・負荷試験は、常に Linux のコンテナ（docker compose：gw のビルド用イメージ、Mosquitto、iotspec）で行う。

## 8. ゴールデン照合と契約テスト

Python 参照実装とのずれは、同じ版の iot-common-spec を使う3段＋対応表の照合で防ぐ。版は `cmake/IotSpec.cmake` の `IOTSPEC_REF` と CI の環境変数の1か所だけで決め、両者が食い違えば CI を失敗させる。

| 段 | やり方 | 見るもの | 実行場所 |
| --- | --- | --- | --- |
| 1. ゴールデン照合（ctest） | `uart/golden/*.json` を NN 順に1つの UartBridge に流す。仮想時計に `received_at` を入れ、`SequentialIdGenerator` で msg\_id を揃える | 01→03：エンベロープ全体（msg\_id・ts を含む）、`discarded` の理由、`request_state`、`dropped_telemetry_total`。03→01：`input_cmd_payload` と `cmd_ref` から作ったフレームの hex | macOS・Linux・CI |
| 2. 対応表の照合 | `gw-dump-tables`（C++）が状態・コード・重大度・品質・reset\_reason・トピックの QoS/retain・定数（タイムアウト、上限）を JSON で出し、`tools/check_tables.py` が iotspec の constants・mapping と突き合わせる | ゴールデンに現れない値（例：使われていないイベントコード） | Linux・CI |
| 3. 契約テスト | `iotspec-contract --role gw-01 --target-cmd 'build/gw --config ci/gateway.yaml --uart {uart}'`。Mosquitto は deploy/mosquitto の ACL と gen\_passwd.sh | プロセス全体。pty 越しの受信・送信制限・request/ack・bridged\_by 付きの offline・共通チェック | Linux・CI |
| 4. 先読み | 週次ジョブで 1・2 を iot-common-spec の main に対して回す。失敗しても本体の CI は落とさない | 仕様側の変更によるずれ | CI（schedule） |

- 取り込み方：C++ 側は CMake の FetchContent でタグを取得し、`uart/golden`・`profiles`・`schemas`・`samples` を参照する。Python 側は `uv tool install "iotspec @ git+https://github.com/io0323/iot-common-spec@${IOTSPEC_REF}"`
- 契約テストの対象はフェーズごとに `--only` で広げる（第10章）。通らないチェックを外すのではなく、完了条件に含めたものだけを指定する
- ゴールデンの契約チェックは「起動したばかりの対象」が前提。gw は起動直後に UART に HEARTBEAT を送ってよいが、受信前に GET\_STATUS を送ってはいけない
- ローカルでは `make contract`（docker compose で Mosquitto と gw を起動し、コンテナ内で契約テスト）で同じものを動かす

## 9. ビルド・依存・CI

実機で動くことを優先し、コンパイラの下限は GCC 12（Debian Bookworm 系の Raspberry Pi OS）に合わせる。実機の OS がより新しければ後で引き上げる。

| 依存 | 用途 | 入手 |
| --- | --- | --- |
| libmosquitto | MQTT | OS のパッケージ（libmosquitto-dev、Homebrew の mosquitto） |
| SQLite3 | 履歴（Phase 5） | OS のパッケージ |
| nlohmann/json、yaml-cpp、spdlog | JSON・設定・ログ | OS のパッケージ。無ければ FetchContent |
| jsoncons | JSON Schema 検証（Phase 2、D6） | FetchContent（ヘッダのみ） |
| GoogleTest | 単体テスト | FetchContent |
| iotspec（Python） | 契約テスト・代役・対応表の照合 | uv tool install（タグ指定） |

### CI（GitHub Actions）

| ジョブ | 内容 | 導入フェーズ |
| --- | --- | --- |
| build-test | Linux、GCC。ビルド、ctest（単体＋ゴールデン） | 1 |
| lint | clang-format の確認、clang-tidy 警告ゼロ | 1 |
| sanitizers | asan+ubsan と tsan の2構成で ctest | 1 |
| contract | Mosquitto をサービスで起動し、対応表の照合と iotspec-contract（フェーズごとの --only）。JUnit を成果物に残す | 1 |
| macos | AppleClang でビルドと単体テスト | 1 |
| arm64 | arm64 の GitHub ランナーでネイティブにビルド・テスト（NFR-10） | 1 |
| coverage | gcovr。core の行カバレッジ80%以上を条件にする | 2 |
| integration | pytest：iotspec-sim の代役・作品01・02 のホスト版とのシナリオ | 2 |
| fault-injection | ブローカー停止、pty 切断、破損フレーム、SIGKILL、ディスク満杯 | 6 |
| load | 毎秒100〜500件。結果を成果物に残す（手動実行と週次） | 6 |
| spec-drift | 第8章の4段目（週次、失敗しても本体は落とさない） | 1 |

開発用コンテナは `deploy/Dockerfile.dev`（Debian 系、上の依存と uv、iotspec 入り）。CI も同じイメージで動かし、手元と CI の差をなくす。

## 10. フェーズ分割

作品03 は6フェーズ＋共通仕様の Phase S で進める。指令管理（Phase 3）をルールエンジン（Phase 4）より先にし、gw-01 の契約テスト全件を Phase 3 で通す。各フェーズの完了条件には、それまでのフェーズのテストと CI 全ジョブが通ることを含む。

| フェーズ | リポジトリ | 範囲 | 完了条件（すべて CI で確認） |
| --- | --- | --- | --- |
| S | iot-common-spec | v0.2.2 の実体：spec.md に CR-1〜5、DIAG（0x05、15バイト）の復号と not\_converted、EVENT code 1 の reset\_reason、code 9・10、expires\_at を過ぎた指令を UART に送らないこと。gen\_golden に DIAG・BOOT・INTERLOCK のケースを足し、17番を見直す。代役と契約テストも合わせる | pytest 全件、`gen_golden.py --check` に差分なし、CHANGELOG 更新、タグ v0.2.2 |
| 1 | iot-control-hub-rpi | 骨組み（CMake・プリセット・CI・Dockerfile.dev・CLAUDE.md・open-questions）。IClock、ULID、BoundedQueue、TimerService、CoreRunner。UART：SerialPort、FrameCodec、UartBridge、HEARTBEAT 送受信、途絶えでの bridged の offline、GET\_STATUS。MQTT：IMqttClient、MosquittoClient、mqtt-tx の送信制限、gw-01 の state と LWT | 単体テスト（キュー、タイマー、COBS・CRC、Bridge、送信制限）、ゴールデン全件一致、対応表の照合、サニタイザ・clang-tidy・macOS・arm64 が通る。契約テスト `--only golden_conversion --only bridged_offline_state --only schema --only seq_and_boot_id --only publish_rate` が合格 |
| 2 | 同上 | MQTT 取り込みと JSON Schema 検証（D6）、devices.yaml、DeviceRegistry（静的登録と state による自動登録）、LivenessMonitor（online・suspect・offline、DEVICE\_OFFLINE・ONLINE）、データ品質（重複 1,024、stale、bridged 配下の状態不明、CLOCK\_SKEW）、gw-01/state に全デバイスの状態（FR-14） | samples/valid 全件が通り invalid 全件が弾かれる、品質判定の単体テスト（仮想時計）、core カバレッジ80%、結合：mcu-cpp-01 代役と作品02 ホスト版が自動登録され、停止で offline になる |
| 3 | 同上 | Arbiter（優先度と所有者）、CommandManager（ライフサイクル、再送、リース更新、UART の毎秒1件・未完了4件、expires\_at、offline 宛は即 FAILED）、RequestHandler（requests.yaml、request/ack、request\_id の1時間記憶）、COMMAND\_FAILED | 調停の全優先度の組み合わせ、再送とタイムアウトの単体テスト（仮想時計）、契約テスト `--role gw-01` の全チェック合格、結合：app-01 代役の要求が作品01 ホスト版のファンに届き done が返る |
| 4 | 同上 | RuleEngine（YAML、all・any・not、for、ヒステリシス、cooldown、stale での停止、hold と release）、SIGHUP での再読込、SystemModeController（NORMAL・ALERT・EMERGENCY）、アクション4種、RULE\_FIRED・CLEARED、SYSTEM\_MODE\_CHANGED | 「9.9秒では発火しない・10秒で発火・途絶えたら発火しない」など仮想時計の単体テスト、再読込の失敗で旧ルールを維持、結合：構想のイベント／アクション6種のシナリオ |
| 5 | 同上 | HistoryStore（SQLite WAL、移行、uart\_diag を含む全テーブル、100件または200msの一括書き込み、90日の削除、失敗時の1万件保持、DATA\_DROPPED）、起動時の読み込み（D5）、admin ソケットと gwctl（devices、rules、cmd、reload、diag） | 履歴の単体テスト（一時 DB）、再起動をまたいだ request\_id の重複が弾かれる、`gwctl diag mcu-c-01` で DIAG が見え、MQTT には出ていない |
| 6 | 同上 | INotifierと systemd ユニット（Type=notify、ウォッチドッグ、専用ユーザー）、sysmon（ISysInfoSource、HOST\_RESOURCE\_HIGH・NORMAL）、障害注入、負荷試験、README | 障害注入の全項目で基本設計第8章のとおり復旧、systemd 入りコンテナで kill から5秒以内に再起動（NFR-04）、NFR-01・02 の計測値を README に掲載、MVP の受け入れ基準をすべて満たす |

- 各フェーズは main から `phase-N/<名前>` を切り、PR で終える。仕様にないことは docs/open-questions.md に書いて止める
- Claude Code へのプロンプトは docs/phases/phase-N.md に置き、この表の完了条件をそのまま持ち込む

## 11. 未決事項

次の8件を docs/open-questions.md の初期値にする。推奨は仮実装の方針で、決まるまでは Claude Code がそれ以外を独断で選ばない。

| # | 論点 | 推奨 | 決める時期 |
| --- | --- | --- | --- |
| Q1 | DIAG の15バイトの中身と uart\_diag の列 | Phase S で共通仕様に書く。uart\_diag は受信時刻・boot\_id・各項目を列にし、90日保持 | Phase S |
| Q2 | STATE を求めるために gw が自分で送る GET\_STATUS の cmd\_ref と ACK の扱い | 参照実装（gw\_01\_lite）に合わせる：GET\_STATUS は gw が cmd\_id を採番する普通の指令（priority auto）で、毎秒1件の制限を受け、ACK は cmd/ack に変換して発行する。GET\_DIAG だけは内部の操作で、ACK を変換しない（Phase S の #37） | Phase S |
| Q3 | UART の生存確認を HEARTBEAT だけで更新するか、有効なフレームなら何でも更新するか。最初の受信前も数えるか | 参照実装（gw\_01\_lite）は受信したバイトなら何でも更新している。作品03 は「復号できたフレームなら何でも更新」とし、最初の受信までは数えない（boot\_id が無く offline の state を作れないため）。仕様の「HEARTBEAT が3秒途絶えたら」との差は Phase S で spec.md に一文足すか io が決める | Phase 1 の前 |
| Q4 | suspect になる条件 | UART：HEARTBEAT が2秒途絶えたら suspect。MQTT：どのチャネルも stale になったら suspect。suspect ではイベントを出さない | Phase 2 |
| Q5 | 送信制限（毎秒10件・瞬間50件）の単位が「接続×kind」か「発信元×kind」か | 決定：発信元（device\_id）× schema。publish\_rate チェックがこの単位で数えている（容量50・毎秒10件補充のトークンバケットと、1秒窓で50件以下） | Phase 1 |
| Q6 | キューの容量と、切断中に mqtt-tx が保持する件数（2000） | 第3章の値で始め、Phase 6 の負荷試験で見直す | Phase 6 |
| Q7 | JSON Schema の検証が Raspberry Pi で毎秒200件に追いつくか | Phase 2 で PC 上の1件あたりの時間を測る。足りなければ kind ごとの必須項目だけを C++ で確かめる方式に切り替える | Phase 2 |
| Q8 | 実機の Raspberry Pi OS の版（コンパイラの下限） | GCC 12 を下限にしておき、実機フェーズで確定する | 実機フェーズ |
