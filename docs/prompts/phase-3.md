# 作品03 フェーズ 3：調停・指令管理・要求

## Phase 2 への回答（先に片付ける）

PR #2 は io がマージする。マージを確かめてから `phase-2/ingest-liveness` をローカルとリモートで削除し、main から `phase-3/command-request` を切る。

open-questions の更新：
- #14・#16・#17・#18・#19：推奨案（A）で「決定（2026-11-26）」
- #15：変更案を承認。自己監視（FR-06）のチャネルも同じ v0.2.3 に含めて iot-common-spec のフェーズ 8 で扱う、と書き足す

## はじめに読むもの

- 詳細設計書：第3章、第4章（TimerService の表、起動の手順）、第10章の Phase 3 の行
- 要件定義・基本設計書：第0章、FR-08〜11、第6章「調停」「指令のライフサイクル」「重複排除」「送信の制限」、第8章の障害と対応の表
- iot-common-spec（cmake/IotSpec.cmake の版）：docs/spec.md 第3章（cmd・cmd/ack・request・request/ack）、第4章（指令と要求、IDの種類、重複排除の記憶、送信量の上限、リース、優先度）、第7章（CMD・ACK・#40・#43）、schemas/、src/iotspec/lease.py、sim/devices/gw_01_lite.py・app_01.py・mcu_cpp_01.py、sim/scenarios/request_flow.yaml、contract/roles/gw_01.py（uart_cmd_limits・request_ack）

読み終えたら「作るもの・作らないもの・不明点」を短く報告してから始める。

## 決まっていること

- 優先度は safety ＞ manual ＞ auto ＞ schedule。同じ優先度では新しい方が勝つ。マイコン側のインターロックはさらに上位で、limited で知る
- 指令の状態は ISSUED → SENT → ACCEPTED → DONE、ほかに REJECTED・FAILED・TIMED_OUT。ACK を 2秒待ち、同じ cmd_id（UART は同じ cmd_ref）で最大3回再送する
- 作品01 への指令は毎秒1件（再送を含み、再送を優先）・未完了4件まで。超えた分は送信待ちにし、同じ対象への古い指令は新しい指令で置き換える。Phase 1 の GET_STATUS の送信器はこの仕組みに置き換える
- expires_at は最初の送信の前だけ検査する（v0.2.2 #43）。過ぎていたら UART に送らず REJECTED（expired）で終え、mcu-c-01 名義の cmd/ack は作らない（#40）
- lease_s は省略時60秒・上限600秒で set_actuator のみ。リースの更新は lease_s の3分の1ごとに新しい cmd_id で送る
- offline のデバイス宛の指令は送らずにその場で FAILED（offline）。復帰後に古い指令をまとめて送らない
- request は requests.yaml の許可リストと優先度で判定する。request_id を1時間記憶し、重複には新しい指令を作らず現在の状態を request/ack で返す（D5。core のメモリだけ。再起動をまたぐ分は Phase 5）
- request/ack は accepted（cmd_id）／rejected（reason）／done／failed。終端は指令の終端に対応させる
- COMMAND_FAILED は FAILED・TIMED_OUT のとき1件発行する

## Phase 3 で作るもの

1. **Arbiter**（core/command）：アクチュエーターごとの所有者と優先度、解放時の引き継ぎ、拒否の理由
2. **CommandManager**（core/command）：上のライフサイクル、再送、リース更新と release、UART と MQTT の送信経路、送信待ちと置き換え、cmd/ack の受け取り（MQTT の cmd/ack と UART の ACK の両方）
3. **RequestHandler**（core/command）：requests.yaml（許可する target・op・actuator と既定の優先度）、request の検証、指令への変換、request/ack の発行、request_id の記憶
4. **設定**：requests.yaml の読み込みと検証（誤りは終了コード 2）。SIGHUP での再読込は Phase 4 でルールと一緒に扱う
5. **iot-common-spec v0.2.3**：タグがあれば IOTSPEC_REF を v0.2.3 にし、品質の件数を gw-01 の telemetry で60秒ごとに発行する（ログの event=stats は残す）。タグが無ければこの項目は飛ばし、PR に書く
6. **テスト**
   - 単体（仮想時計）：調停の全優先度の組み合わせ（16通り）と同じ優先度の後勝ち、解放と引き継ぎ、ライフサイクルの全遷移、2秒の境界と3回の再送、TIMED_OUT、毎秒1件・未完了4件と再送の優先、置き換え、expires_at、offline の即 FAILED、リース更新の周期と release、request の重複
   - 結合（pytest）：app-01 の代役の要求が作品01 のホスト版（または mcu-c-01 の代役）のファンに届き done が返る。mcu-cpp-01 の代役への set_actuator と release。request_flow シナリオの拒否・無応答
   - 契約テスト：`--role gw-01` の全チェック（--only なし）

## Phase 3 で作らないもの

ルールエンジン・システムモード・SIGHUP（Phase 4）、履歴と再起動をまたぐ重複排除・gwctl（Phase 5）、systemd・sysmon・障害注入（Phase 6）。

## 完了条件（すべて満たしてから PR）

1. Linux（GCC 12、x86_64・aarch64）と macOS で警告ゼロ、ctest 全件合格
2. asan+ubsan・tsan の指摘ゼロ、clang-tidy の警告ゼロ、clang-format の差分なし
3. core の行カバレッジ 80% 以上
4. 契約テスト `iotspec-contract --role gw-01` の全チェックが `make contract` と CI で合格
5. Phase 2 までの結合テストと、Phase 3 の結合テストが手元と CI で合格
6. CI の全ジョブが合格

PR の説明には、完了条件ごとの結果、open-questions に足した項目、Phase 4 への申し送りを書く。マージはしない。

## 止まって相談すること

- 調停・ライフサイクル・送信制限の規則が、設計書と共通仕様と参照実装（gw_01_lite）で食い違うとき
- requests.yaml の形や、request の拒否理由で共通仕様にない値が要るとき
- 契約テストを通すために設計書にない振る舞いが要るとき
- 共通仕様の変更が要ると分かったとき

いずれも open-questions に「論点・選択肢・推奨案」を書き、その件に関わる作業を止めて報告する。
