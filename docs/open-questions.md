# 未決の論点（open questions）

設計書（docs/design/）と共通仕様（iot-common-spec v0.2.2）に書かれていない、または食い違いがあり、
実装で判断が必要になった点を記録する。#1〜#8 は詳細設計書 第11章の Q1〜Q8。

状態：`未決`（判断待ち）／`仮実装`（推奨案で進めている）／`決定`

| # | 論点 | 選択肢 | 推奨 | 状態 |
|---|---|---|---|---|
| 1 | DIAG の15バイトの中身と uart_diag の列（Q1） | — | Phase S で共通仕様に書く。uart_diag は受信時刻・boot_id・各項目を列にし、90日保持 | 決定：iot-common-spec v0.2.2 #37（DIAG 0x05、15バイト）。Phase 1 は not_converted で数えるだけ。uart_diag は Phase 5 |
| 2 | STATE を求めるために gw が自分で送る GET_STATUS の cmd_ref と ACK の扱い（Q2） | — | gw_01_lite に合わせる | 決定：GET_STATUS は gw が cmd_id を採番する普通の指令（priority auto）で、毎秒1件の制限を受け、ACK は cmd/ack に変換して発行する。GET_DIAG だけは内部の操作で ACK を変換しない（v0.2.2 #37） |
| 3 | UART の生存確認を HEARTBEAT だけで更新するか、有効なフレームなら何でも更新するか。最初の受信前も数えるか（Q3） | — | 復号できたフレームなら何でも更新、最初の受信までは数えない | 決定（2026-11-26）：復号できたフレームなら何でも生存確認を更新する（壊れたフレームは更新しない）。最初の受信までは数えない（boot_id が無く offline の state を作れないため）。Core::on_uart |
| 4 | suspect になる条件（Q4） | — | UART：HEARTBEAT が2秒途絶えたら suspect。MQTT：どのチャネルも stale になったら suspect。suspect ではイベントを出さない | 未決（Phase 2） |
| 5 | 送信制限（毎秒10件・瞬間50件）の単位（Q5） | — | 発信元×schema | 決定：発信元（device_id）× kind。実装は iotspec.sim.ratelimit と同じ「容量50・毎秒10件補充のトークンバケット」と「直近1秒の送信が50件未満」の両方（バケットだけだと1秒の窓で最大59件になり publish_rate の判定を超えるため。TokenBucket） |
| 6 | キューの容量と、切断中に mqtt-tx が保持する件数（Q6） | — | 第3章の値で始め、Phase 6 の負荷試験で見直す | 仮実装：inbound 4096、uart-out 64、mqtt-out 4096、保持 2000（gateway.yaml の queues で変えられる） |
| 7 | JSON Schema の検証が Raspberry Pi で毎秒200件に追いつくか（Q7） | — | Phase 2 で測る | 未決（Phase 2） |
| 8 | 実機の Raspberry Pi OS の版（コンパイラの下限）（Q8） | — | GCC 12 を下限にしておく | 未決（実機フェーズ）。CI は Debian bookworm（GCC 12）で x86_64・aarch64 |
| 9 | bridge.py の `_emit` は変換結果を JSON Schema で検証して schema_invalid で捨てるが、core はスキーマ検証ライブラリを持たない（D6 で adapters に閉じる） | A：UART の値から起こりうる違反だけを core で確かめる／B：core から adapters の検証を呼ぶ | A。UART 由来の電文で Schema に違反しうるのは「fan_pm が1000超（level の maximum）」と「rejected・failed で reason が null」だけ（ほかの値は対応表とプロファイルで先に弾かれる）。Phase 2 で jsoncons が入ったら、UART 由来の電文も samples と同じ検証にかけて差がないことをテストで確かめる | 仮実装（UartBridge の schema_violation） |
| 10 | MQTT の keepalive（設計書に値がない） | 60・30・10秒 | 60秒（libmosquitto の一般的な既定。gw-01 の LWT は約90秒後） | 決定（2026-11-26、io）：60秒。gateway.yaml の mqtt.keepalive_s |
| 11 | gw-01 自身の state の項目と発行の時機（設計書は「プロファイル gw-01.yaml」とだけ書く） | — | mode は起動時のシステムモード `NORMAL`（基本設計 第6章）、fw_version は CMake のプロジェクトの版、channels・actuators は gw-01.yaml（空）。MQTT につながるたびに新しい seq で発行する（再接続で LWT の offline が retain されていても online に戻すため）。LWT と終了時の offline は seq を進めない（v0.2.1 #29） | 仮実装（Core::gw_state_envelope）。FR-14（全デバイスの状態）は Phase 2 |
| 12 | 起動直後、MQTT の最初の接続より前に UART から telemetry が届くと、第3章どおり mqtt-tx が捨てる（state は最新1件、event・ack は保持） | A：そのまま／B：初回接続まで（最大3秒）UART を開かない | A。契約テストの golden_conversion は起動と同時にフレームを送るが、手元とコンテナでは接続が数 ms で終わり合格している。CI で落ちるようなら B を相談する | 仮実装（A） |
