# 作品03 フェーズ 5：履歴・起動時の復元・gwctl

## Phase 4 への回答（先に片付ける）

PR #4 は io がマージする。マージを確かめてから `phase-4/rules-mode` をローカルとリモートで削除し、main から `phase-5/history-gwctl` を切る。

open-questions の更新：
- #29：A で「決定（2026-11-27）」
- #30〜#38・#40：推奨案で「決定」
- #39：**変更して決定**。再読込で active のルールをすべて解除すると、hold の指令（例：冷却ファン）がいったん release され、for の時間（例：10秒）が過ぎるまで冷却が止まる。そこで、id と定義（when・for・hysteresis・then・priority・cooldown）が再読込の前後で同じルールは、状態（pending の経過・active・cooldown・hold の指令とリース）をそのまま引き継ぎ、解除しない。解除するのは、消えたルールと定義が変わったルールだけ（active なら RULE_CLEARED と hold の release）。そのあと全ルールを評価する。EMERGENCY と実行中の指令の扱いは今のまま。単体テストを足し、このフェーズの最初のコミットにする

手元の Docker の容量：`iot-gw-dev:v0.2.2` と、どのコンテナにも使われていない dangling のイメージ（`docker image prune` で消えるもの）は削除してよい。ほかのプロジェクトのイメージ・ボリュームには触らない。空けたら Phase 4 の結合テストを手元で流し、結果を PR #5 に書く。それでも足りなければ止まって報告する。

## はじめに読むもの

- 要件定義・基本設計書：FR-13・15、NFR-04、第7章「履歴DB」、第8章（DB書き込み失敗の行と、その下の1万件の段落）
- 詳細設計書：第3章（history のキュー）、第4章「起動の手順」、第7章、第10章の Phase 5 の行
- iot-common-spec v0.2.3：spec.md 第4章（重複排除の記憶）、第6章（DATA_DROPPED）、#37（DIAG の15バイト）、#46〜#48
- 作品03 の open-questions #1（uart_diag）・#36・#39
- PR #4 の説明の「Phase 5 への申し送り」

読み終えたら「作るもの・作らないもの・不明点」を短く報告してから始める。

## 決まっていること

- SQLite は WAL、synchronous=NORMAL。書くのは history スレッドだけ。core は実行中に DB を読まない（D5）
- テーブルは基本設計 第7章の8つと uart_diag（受信時刻・boot_id・#37 の各項目を列にする）。スキーマは `PRAGMA user_version` で版を管理し、起動時に移行を順に適用する。時刻はすべて UTC のミリ秒
- 保存するもの：受信・発行したイベント、デバイスの状態の変化、ルールの発火と解除、指令とその状態遷移、要求とその結果、システムモードの変化、DIAG。telemetry は保存しない（作品04 の責任）
- 書き込みは100件または200msごとの一括。保持は90日で、gateway.yaml の timezone の3時に削除する
- 書き込みに失敗したら、メモリに最大1万件を保持して再試行する。超えたら info のイベント → ルールの発火 → 指令の途中の状態遷移 の順に捨てる。指令と要求の最終状態は最後まで残す。捨てた件数は DATA_DROPPED で知らせ、復旧後に「この期間の履歴は欠けている」ことを system_mode_log に記録する。制御は止めない
- 起動時の復元（DB を開いたあと、core を動かす前）：未完了の指令を FAILED（restart）で閉じる。直近1時間の requests を重複判定の記憶に入れる。デバイス台帳を初期状態にする（状態は unknown から始める）
- 再起動をまたぐシステムモード：最後のモードが EMERGENCY なら EMERGENCY で起動し、承認を待つ（安全側）。ALERT と NORMAL は NORMAL で起動し、ルールの評価で決め直す
- gwctl：UNIX ドメインソケット（gateway.yaml の admin.socket、既定 `/run/iot-gw/gwctl.sock`、権限 0660）。1行1つの JSON の要求と応答。コマンドは devices、rules、cmd（優先度 manual、origin gwctl。調停と指令管理は request と同じ経路）、reload（SIGHUP と同じ）、approve（EMERGENCY の解除。reason は `approved_by_gwctl`）、diag <device>（GET_DIAG を送り、届いた DIAG を履歴から表示する）、stats（品質の件数とキューの破棄数）
- gwctl の要求は inbound に管理要求として入り、core が処理する。応答は admin スレッドがソケットに返す

## Phase 5 で作るもの

1. #39 の直し（上）
2. **HistoryStore**（adapters/history）：上の決まりどおり。移行、一括書き込み、保持期間の削除、失敗時の保持と捨てる順、DATA_DROPPED
3. **起動時の復元**（app と core）：上の決まりどおり。詳細設計書 第4章の起動の手順に合わせる
4. **admin スレッドと gwctl**（adapters/admin、tools/gwctl）：上のコマンド。gwctl は C++ の小さなコマンドで、人が読む表と `--json` を出す
5. **テスト**
   - 単体：移行（空の DB と古い版から）、一括書き込みの境界（100件・200ms）、保持期間の削除、失敗時の保持と捨てる順（1万件の境界）、復元（未完了の指令、1時間の境界の requests、EMERGENCY の引き継ぎ）、gwctl の要求の解析と応答、#39
   - 結合（pytest）：再起動をまたいで同じ request_id の要求が重複として扱われる。`gwctl diag mcu-c-01` で mcu-c-01 の代役の DIAG が見え、MQTT には出ていない。`gwctl approve` で EMERGENCY から NORMAL に戻り、reason が approved_by_gwctl。`gwctl cmd` の手動の指令が auto のルールに勝つ。EMERGENCY のまま再起動すると EMERGENCY で起動する

## Phase 5 で作らないもの

systemd のユニットと通知・ウォッチドッグ、sysmon と HOST_RESOURCE、プロセス kill とディスク満杯の障害注入、負荷試験（Phase 6）。

## 完了条件（すべて満たしてから PR）

1. Linux（GCC 12、x86_64・aarch64）と macOS で警告ゼロ、ctest 全件合格
2. asan+ubsan・tsan の指摘ゼロ、clang-tidy の警告ゼロ、clang-format の差分なし
3. core の行カバレッジ 80% 以上
4. 契約テスト `--role gw-01` の全チェック合格
5. これまでの結合テストと Phase 5 の結合テストが、手元と CI で合格
6. CI の全ジョブが合格

PR の説明には、完了条件ごとの結果、open-questions に足した項目、Phase 6 への申し送りを書く。マージはしない。

## 止まって相談すること

- 基本設計 第7章のテーブルの列で、記録したいものが入らないとき
- 1万件を超えたときの捨て方で、上の順序だけでは決まらないとき
- gwctl のコマンドで、上にない操作が要ると分かったとき
- 共通仕様の変更が要ると分かったとき

いずれも open-questions に「論点・選択肢・推奨案」を書き、その件に関わる作業を止めて報告する。
