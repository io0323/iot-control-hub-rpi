# 作品03 フェーズ 4：ルールエンジン・システムモード・再読込

## Phase 3 への回答（先に片付ける）

PR #3 は io がマージする。マージを確かめてから `phase-3/command-request` をローカルとリモートで削除し、main から `phase-4/rules-mode` を切る。

open-questions の更新：
- #20・#22・#23・#24・#25・#26：推奨案で「決定（2026-11-26）」
- #21：A で「決定」。置き換えは同じ所有者（同じ優先度・同じ発生元）の指令どうしだけ。基本設計 第6章の「同じ対象への古い指令は新しい指令で置き換える」はこの意味に読む、と書き足す
- #27：iot-common-spec v0.2.3 のタグがあるので、このフェーズの最初に片付ける（下の「作るもの 1」）

## はじめに読むもの

- 要件定義・基本設計書：FR-07・08・12、第6章「ルール定義」「時間条件と値の鮮度」「システムモード」、第8章
- 詳細設計書：第3章、第4章（TimerService の表）、第10章の Phase 4 の行
- iot-common-spec v0.2.3：docs/spec.md 第4章（要求、優先度、#46）、第5章（stale）、第6章（RULE_FIRED・CLEARED、SYSTEM_MODE_CHANGED、#47）、profiles/gw-01.yaml、CHANGELOG の v0.2.3
- 作品03 の open-questions #11・#21・#23・#24・#26

読み終えたら「作るもの・作らないもの・不明点」を短く報告してから始める。

## 決まっていること

- ルールの形は基本設計 第6章の YAML。ただしヒステリシスは `hysteresis: <値>` と書き、解除の向きは op から決める（`>=`・`>` なら しきい値−値を下回ったら解除、`<=`・`<` なら しきい値＋値を上回ったら解除）。`clear_when_below` は使わない
- 条件は all・any・not、参照できるのはチャネルの最新値、デバイスの生存状態、システムモード、時刻帯（`time: {from: "HH:MM", to: "HH:MM"}`、gateway.yaml の timezone で判定。既定 Asia/Tokyo）
- 評価は「値が更新されたチャネルを参照するルールだけ」（索引を起動時と再読込時に作る）に加え、タイマーでも行う。quality が good でない値では発火しない
- ルールの状態は idle → pending → active → cooldown → idle。pending 中に不成立なら idle。参照する値が stale になったら「不明」：pending は idle に戻し、active は新しい指令を出さずリースの更新を止める
- hold: true の command は active の間、CommandManager のリース更新を使う（#23 の仕組み）。解除時に release を送る
- 手動の release で所有権を外されたルール（#26）は、そのルールが一度 idle に戻って再び発火するまで、指令もリース更新もしない
- アクションは command・set_system_mode・notify・reconnect。notify は RULE_FIRED の重大度と detail.message になる（v0.2.3 #47）。reconnect は作品03 が持つ接続（UART のポート、自分の MQTT 接続）をバックオフを待たずに開き直す。MQTT のデバイスには何もしない（ログに残す）
- システムモード：
  - NORMAL → ALERT：warning 以上のイベント、デバイスの offline（どちらもルールで書く。SystemModeController はモードの遷移と影響の適用だけを持つ）
  - EMERGENCY：safety のルールの set_system_mode で入る。「冷却を最大」は SystemModeController が直接指令を出すのではなく、safety のルールの command として rules.yaml に書く（config の例に入れる）
  - EMERGENCY の影響：schedule の request は rejected（`emergency_mode`）、auto のルールは新しい指令を出さずリース更新を止める
  - ALERT の影響：通知の強化（SYSTEM_MODE_CHANGED の重大度 warning）だけ。基本設計の「スケジュール実行を保留」はしない（保留した要求を後で実行すると、古い操作が遅れて動くため）。open-questions に決定として書く
  - EMERGENCY の解除：条件の回復（EMERGENCY に入れたルールがすべて idle）＋承認。承認は request の set_mode（target gw-01、mode NORMAL、v0.2.3 #46）。gwctl の承認は Phase 5
  - モードの変化で SYSTEM_MODE_CHANGED（NORMAL は info、ALERT は warning、EMERGENCY は critical。detail は v0.2.3 #48 の `{from, to, reason}`）を1件出し、gw-01 の state の mode を更新して発行し直す（#11）
- 再読込（SIGHUP）：rules.yaml・devices.yaml・requests.yaml の3つを読み、すべて検証に通ったときだけ入れ替える。1つでも失敗したら全部旧いままにし、理由をログに出す。入れ替え前に active のルールの解除処理（release）をする

## Phase 4 で作るもの

1. **v0.2.3 の取り込み（#27）**：IOTSPEC_REF を v0.2.3 にし、品質の件数を gw-01 の telemetry で60秒ごとに発行する（ログの event=stats は残す）。ゴールデン・対応表の照合・契約テストが引き続き通ること。iotspec は版番号が同じだと uv が前に作ったパッケージを使い回すことがある。手元と開発用コンテナでは `uv tool install --reinstall --no-cache` で入れ直し、CI で uv や iotspec をキャッシュしているならキーにタグのコミット（`git ls-remote` の結果）を含める。入った iotspec に SYSTEM_MODE_CHANGED の検査（#48）があることを確かめてから先に進む
2. **RuleEngine・Rule・Condition・Action**（core/rules）と rules.yaml の読み込み・検証
3. **SystemModeController**（core）と、#46 の承認の request の処理（RequestHandler の拡張）
4. **SIGHUP の再読込**（app と core）
5. **config の例**：基本設計 第6章の3つのルール（冷却、高温継続で EMERGENCY と全ファン最大、offline で再接続と通知）を rules.yaml の例にする
6. **テスト**
   - 単体（仮想時計）：9.9秒では発火しない・10秒で発火する・途中で値が途絶えたら発火しない、ヒステリシスの両方向、cooldown、stale での停止とリース更新の停止、hold と release、手動 release 後に再発火まで黙ること、all・any・not と時刻帯、索引（関係ないチャネルの更新で評価しない）、モードの全遷移と影響、承認の accepted・rejected（unsafe・not_permitted）
   - 再読込：成功・1ファイルの失敗で全部旧いまま・active のルールの解除
   - 結合（pytest）：構想のイベント／アクション6種のシナリオ（基本設計 第8章のテスト方針）。少なくとも、高温で冷却の指令、高温継続で EMERGENCY と schedule の拒否、承認での NORMAL への復帰、offline での再接続と通知、SIGHUP での再読込、stale での停止

## Phase 4 で作らないもの

履歴・gwctl（Phase 5）、systemd・sysmon・HOST_RESOURCE・障害注入・負荷試験（Phase 6）。

## 完了条件（すべて満たしてから PR）

1. Linux（GCC 12、x86_64・aarch64）と macOS で警告ゼロ、ctest 全件合格
2. asan+ubsan・tsan の指摘ゼロ、clang-tidy の警告ゼロ、clang-format の差分なし
3. core の行カバレッジ 80% 以上
4. 契約テスト `--role gw-01` の全チェックが v0.2.3 で合格
5. これまでの結合テストと Phase 4 の結合テストが手元と CI で合格
6. CI の全ジョブが合格

PR の説明には、完了条件ごとの結果、open-questions に足した項目、Phase 5 への申し送りを書く。マージはしない。

## 止まって相談すること

- ルールの YAML で、上の決まりで書けない構想のルールが出てきたとき
- 共通仕様 v0.2.3 の #46・#47 と、実装の都合が合わないとき
- 再読込で、実行中の指令やリースの扱いが上の決まりで決まらないとき
- 共通仕様の変更が要ると分かったとき

いずれも open-questions に「論点・選択肢・推奨案」を書き、その件に関わる作業を止めて報告する。
