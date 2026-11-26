# 未決の論点（open questions）

設計書（docs/design/）と共通仕様（iot-common-spec v0.2.2）に書かれていない、または食い違いがあり、
実装で判断が必要になった点を記録する。#1〜#8 は詳細設計書 第11章の Q1〜Q8。

状態：`未決`（判断待ち）／`仮実装`（推奨案で進めている）／`決定`

| # | 論点 | 選択肢 | 推奨 | 状態 |
|---|---|---|---|---|
| 1 | DIAG の15バイトの中身と uart_diag の列（Q1） | — | Phase S で共通仕様に書く。uart_diag は受信時刻・boot_id・各項目を列にし、90日保持 | 決定：iot-common-spec v0.2.2 #37（DIAG 0x05、15バイト）。Phase 1 は not_converted で数えるだけ。uart_diag は Phase 5 |
| 2 | STATE を求めるために gw が自分で送る GET_STATUS の cmd_ref と ACK の扱い（Q2） | — | gw_01_lite に合わせる | 決定：GET_STATUS は gw が cmd_id を採番する普通の指令（priority auto）で、毎秒1件の制限を受け、ACK は cmd/ack に変換して発行する。GET_DIAG だけは内部の操作で ACK を変換しない（v0.2.2 #37） |
| 3 | UART の生存確認を HEARTBEAT だけで更新するか、有効なフレームなら何でも更新するか。最初の受信前も数えるか（Q3） | — | 復号できたフレームなら何でも更新、最初の受信までは数えない | 決定（2026-11-26）：復号できたフレームなら何でも生存確認を更新する（壊れたフレームは更新しない）。最初の受信までは数えない（boot_id が無く offline の state を作れないため）。Core::on_uart |
| 4 | suspect になる条件（Q4） | — | UART：HEARTBEAT が2秒途絶えたら suspect。MQTT：どのチャネルも stale になったら suspect。suspect ではイベントを出さない | 決定（2026-11-26、io）：推奨案。UART は最後のフレーム受信から2秒で suspect、3秒で offline。MQTT のデバイスは state のどのチャネルも stale になったら suspect、LWT または state の offline で offline。suspect ではイベントを出さない（LivenessMonitor） |
| 5 | 送信制限（毎秒10件・瞬間50件）の単位（Q5） | — | 発信元×schema | 決定：発信元（device_id）× kind。実装は iotspec.sim.ratelimit と同じ「容量50・毎秒10件補充のトークンバケット」と「直近1秒の送信が50件未満」の両方（バケットだけだと1秒の窓で最大59件になり publish_rate の判定を超えるため。TokenBucket） |
| 6 | キューの容量と、切断中に mqtt-tx が保持する件数（Q6） | — | 第3章の値で始め、Phase 6 の負荷試験で見直す | 仮実装：inbound 4096、uart-out 64、mqtt-out 4096、保持 2000（gateway.yaml の queues で変えられる） |
| 7 | JSON Schema の検証が Raspberry Pi で毎秒200件に追いつくか（Q7） | — | Phase 2 で測る | 計測（Phase 2）：jsoncons 1.10.0 で1件あたり約 23µs（telemetry、GCC 12 -O2、aarch64 のコンテナ。毎秒200件で CPU 約0.5%）。実行中の平均はログ `event=stats` の validate_avg_us。Raspberry Pi での実測は実機フェーズ |
| 8 | 実機の Raspberry Pi OS の版（コンパイラの下限）（Q8） | — | GCC 12 を下限にしておく | 未決（実機フェーズ）。CI は Debian bookworm（GCC 12）で x86_64・aarch64 |
| 9 | bridge.py の `_emit` は変換結果を JSON Schema で検証して schema_invalid で捨てるが、core はスキーマ検証ライブラリを持たない（D6 で adapters に閉じる） | A：UART の値から起こりうる違反だけを core で確かめる／B：core から adapters の検証を呼ぶ | A。UART 由来の電文で Schema に違反しうるのは「fan_pm が1000超（level の maximum）」と「rejected・failed で reason が null」だけ（ほかの値は対応表とプロファイルで先に弾かれる）。Phase 2 で jsoncons が入ったら、UART 由来の電文も samples と同じ検証にかけて差がないことをテストで確かめる | 決定（2026-11-26、io）：A。Phase 2 で、ゴールデン35件から作った UART 由来の電文と、その値を UART の取りうる範囲で変えた電文をスキーマで検証し、`schema_violation` の判定と一致することをテストで確かめた（tests/unit/test_schema.cpp） |
| 10 | MQTT の keepalive（設計書に値がない） | 60・30・10秒 | 60秒（libmosquitto の一般的な既定。gw-01 の LWT は約90秒後） | 決定（2026-11-26、io）：60秒。gateway.yaml の mqtt.keepalive_s |
| 11 | gw-01 自身の state の項目と発行の時機（設計書は「プロファイル gw-01.yaml」とだけ書く） | — | mode は起動時のシステムモード `NORMAL`（基本設計 第6章）、fw_version は CMake のプロジェクトの版、channels・actuators は gw-01.yaml（空）。MQTT につながるたびに新しい seq で発行する（再接続で LWT の offline が retain されていても online に戻すため）。LWT と終了時の offline は seq を進めない（v0.2.1 #29） | 決定（2026-11-26、io）：推奨案。mode は Phase 4 で SystemModeController の値に置き換える。FR-14 は #13 |
| 12 | 起動直後、MQTT の最初の接続より前に UART から telemetry が届くと、第3章どおり mqtt-tx が捨てる（state は最新1件、event・ack は保持） | A：そのまま／B：初回接続まで（最大3秒）UART を開かない | A。契約テストの golden_conversion は起動と同時にフレームを送るが、手元とコンテナでは接続が数 ms で終わり合格している。CI で落ちるようなら B を相談する | 決定（2026-11-26、io）：C。最初の MQTT 接続が成立するまでは、mqtt-tx は telemetry も含めて保持する（上限は切断中の保持と同じ2000件、超えたら古い telemetry から捨てて数える）。2回目以降の切断は第3章どおり telemetry を捨てる。理由：起動直後の取りこぼしは golden_conversion を不安定にし、起動時の遅れは障害ではないため（MqttTxWorker） |
| 13 | FR-14「gw-01/state に全デバイスの状態を公開する」と、共通仕様の state の形 | — | — | 決定（2026-11-26、io）：gw-01 の state には自分の稼働状態と mode（システムモード）だけを入れ、全デバイスの状態は入れない。state にデバイス一覧の項目は無く、足すと共通仕様の変更になる。作品04 は各デバイスの retain の state、bridged_by の規則、DEVICE_OFFLINE／ONLINE のイベントで同じ情報を得られる。基本設計 FR-14 の記述とはこの点で異なる |
| 14 | devices.yaml の書き方（設計書は「iotspec のデバイスプロファイルと同じ形」とだけ書く） | A：`devices:` の各要素を `{profile: <device_id>}`（同梱のプロファイル）か、プロファイルと同じ形のインラインにする／B：プロファイルの一覧のパスだけを書く | A。mcu-c-01 は同梱の profiles/mcu-c-01.yaml をそのまま使え、同梱に無いデバイスもその場で書ける。gateway.yaml の `devices_file`（既定は同じディレクトリの devices.yaml）で指す。gateway.yaml の uart.device_id は transport: uart のデバイスとして devices.yaml に必要 | 仮実装（adapters/config/DevicesConfig） |
| 15 | 破棄・重複・欠番・stale の件数を gw-01 の telemetry に載せる（第5章）には、件数の単位（unit の列挙に無い）とチャネル名が要る | A：共通仕様 v0.2.3 の変更案にして、決まるまでは数えてログに出す／B：先に iot-common-spec の PR を出し、タグが出てから載せる／C：telemetry には載せない | A | 決定（2026-11-26、io）：A。Phase 2 は件数を数え、60秒ごとと終了時にログ `event=stats` に出す。変更案は docs/design/03-common-spec-cr-v0.2.3.md（unit に count、gw-01 のチャネル8つ） |
| 16 | 一度も観測していないデバイスが最初に online になったとき、DEVICE_ONLINE を出すか（FR-04 は「変化時に event」とだけ書く） | A：出さない（offline を観測した後の online だけ）／B：出す | A。gw_01_lite と同じ。起動のたびに全デバイスの DEVICE_ONLINE が出ると、作品04 の通知が「復旧」で埋まる。最初に観測したのが retain の offline（LWT）なら、次の online で出す | 仮実装（LivenessMonitor::set_status） |
| 17 | CLOCK_SKEW の detail の項目（第6章は決めていない） | — | `{device_id, ts}`（ts は置き換える前の元の値。第5章「元の ts も記録」） | 仮実装（Core::on_mqtt） |
| 18 | stale の起点：state で登録したが一度も値を受けていないチャネル | A：stale に数えない（suspect の根拠にしない）／B：state を受けた時刻から数える | A。値の途絶ではなく、まだ値が無い状態なので | 仮実装（LivenessMonitor::stale_check）。遅延到着（第5章、ts が周期の3倍超・5分以内で過去）は最新値を更新しない |
| 19 | state で登録される前のデバイスから届いた telemetry | A：値を持たず数えるだけ／B：telemetry からも登録する | A。FR-01 の自動登録は state による（チャネルの単位と周期は state にしかない）。retain の state は購読時に届くので、ふつうは先に登録される | 仮実装（Core::apply_message、統計の unregistered） |
