# 共通仕様 v0.2.3 変更案：作品03（gw-01）の品質の件数を telemetry で公開する

2026-11-26 · 作品03 Phase 2 から（open-questions #15）

## 問題

共通仕様 第5章は「破棄や置き換えの件数は、各作品とも状態として公開する（03は `gw-01` の telemetry、04は `/health`）」と定める。しかし v0.2.2 の telemetry では、件数を載せられない。

- `readings[].unit` は列挙（degC・pct_rh・pct・level_pm・hPa・lux・bytes・rpm）で、件数に合う単位がない
- gw-01 のチャネル名が決まっていない。`profiles/gw-01.yaml` の channels は空
- payload に独自の項目を足すと、共通仕様にない形になる（作品04 が読めない）

作品03 は Phase 2 で件数を数えている。決まるまでは、60秒ごとと終了時のログ（`event=stats`）に出している。

## 提案（推奨）

1. **単位に `count` を足す**（common.schema.json の `unit` の列挙）。意味は「起動からの累積件数（boot_id ごとに0から）」で、整数とする
2. **gw-01 のチャネルを決める**（`profiles/gw-01.yaml` の channels、`period_ms: 60000`）

   | チャネル | 数えるもの |
   |---|---|
   | `mqtt_rejected` | スキーマ違反・JSON として読めない・未知の schema で捨てた電文 |
   | `mqtt_duplicates` | 重複（第5章）で捨てた電文 |
   | `mqtt_gaps` | 欠損（同じ boot_id 内の seq の飛び）の数 |
   | `mqtt_clock_skew` | 時刻ずれで ts を置き換えた電文 |
   | `mqtt_late` | 遅延到着で最新値を更新しなかった値 |
   | `stale_transitions` | チャネルが stale になった回数 |
   | `uart_discarded` | UART で捨てたフレーム（CRC・COBS・STATE 待ち・変換できない値など。not_converted は含めない） |
   | `queue_dropped` | キューやブローカー停止中の保持で捨てた電文（第3章） |

3. gw-01 は、上の値を telemetry で60秒ごとに発行する（`quality: good`）。理由ごとの内訳は gwctl（Phase 5）とログで見る

## 選ばなかった案

- **payload に `counters` などの独自項目を足す**：Schema は通るが、共通仕様にない形を作品04 に読ませることになる
- **gw-01 の state に入れる**：state は retain の生存状態で、頻繁に変わる件数には向かない。FR-14 と同じ理由で、state に独自の項目を足さない
- **単位を `pct` などで代用する**：意味が違う

## 影響

| 作品 | 変更 |
|---|---|
| iot-common-spec | common.schema.json の `unit` に `count`、profiles/gw-01.yaml の channels、spec.md 第2章（単位）と第5章、サンプル、契約テスト（gw-01 の telemetry の Schema 適合は共通チェックで確かめられる） |
| 作品03 | telemetry の発行を足す。値はすでに数えている |
| 作品04 | gw-01 の telemetry を、ほかのデバイスと同じく取り込んで表示する（単位 `count` を知る） |
| 作品01・02 | なし |

トピックの版（v1）は変えない（単位の列挙に値を1つ足すだけ）。
