#!/usr/bin/env python3
"""データ品質の判定のテストベクタを iotspec（quality.py・dedup.py）で作る。

使い方（開発用コンテナの iotspec の Python で）:
    /opt/uv/tools/iotspec/bin/python tools/gen_quality_vectors.py > tests/data/quality_vectors.json
    /opt/uv/tools/iotspec/bin/python tools/gen_quality_vectors.py --check tests/data/quality_vectors.json

C++ の tests/unit/test_quality.cpp が同じ入力で同じ結果になることを確かめる（Phase 2 の完了条件）。
"""

from __future__ import annotations

import json
import random
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

from iotspec.clock import format_ts
from iotspec.dedup import SeqDedupWindow, exempt_from_seq_checks
from iotspec.quality import classify_clock_skew, classify_late, is_stale

T0 = datetime(2026, 11, 26, 6, 0, 0, tzinfo=UTC)
PERIODS = [100, 1000, 2000, 10000]


def stale_cases() -> list[dict[str, object]]:
    cases = []
    for period in PERIODS:
        limit = period * 3
        for elapsed in (0, limit - 1, limit, limit + 1, limit * 2):
            now = T0 + timedelta(milliseconds=elapsed)
            cases.append({"period_ms": period, "elapsed_ms": elapsed, "stale": is_stale(T0, period, now)})
    return cases


def skew_cases() -> list[dict[str, object]]:
    cases = []
    for offset in (0, 1, 299_999, 300_000, 300_001, 3_600_000):
        for sign in (1, -1):
            ts = T0 + timedelta(milliseconds=sign * offset)
            cases.append(
                {
                    "ts": format_ts(ts),
                    "received_at": format_ts(T0),
                    "skew": classify_clock_skew(ts, T0) == "skew",
                }
            )
    return cases


def late_cases() -> list[dict[str, object]]:
    cases = []
    for period in PERIODS:
        for age in (-1000, 0, period * 3, period * 3 + 1, 299_999, 300_000, 300_001):
            ts = T0 - timedelta(milliseconds=age)
            cases.append(
                {
                    "ts": format_ts(ts),
                    "received_at": format_ts(T0),
                    "period_ms": period,
                    "late": classify_late(ts, T0, period) == "late",
                }
            )
    return cases


def dedup_cases() -> list[dict[str, object]]:
    rng = random.Random(7)
    window = SeqDedupWindow(capacity=8)  # 容量を小さくして古い記憶を捨てる境界も試す
    ops = []
    for _ in range(400):
        device = rng.choice(["mcu-cpp-01", "mcu-cpp-02"])
        kind = rng.choice(["telemetry", "event"])
        boot = rng.choice(["00000001"] * 9 + ["00000002"])
        seq = rng.randrange(0, 24)
        ops.append(
            {
                "device_id": device,
                "boot_id": boot,
                "kind": kind,
                "seq": seq,
                "duplicate": window.is_duplicate(device, boot, kind, seq),
            }
        )
    return [{"capacity": 8, "ops": ops}]


def exempt_cases() -> list[dict[str, object]]:
    messages = [
        {"schema": "iot.v1.state", "payload": {"status": "offline"}},
        {"schema": "iot.v1.state", "payload": {"status": "online"}},
        {"schema": "iot.v1.telemetry", "payload": {"status": "offline"}},
        {"schema": "iot.v1.state", "payload": "offline"},
        {"schema": "iot.v1.state"},
    ]
    return [{"message": m, "exempt": exempt_from_seq_checks(m)} for m in messages]


def vectors() -> dict[str, object]:
    return {
        "stale": stale_cases(),
        "clock_skew": skew_cases(),
        "late": late_cases(),
        "dedup": dedup_cases(),
        "exempt": exempt_cases(),
    }


def main(argv: list[str]) -> int:
    text = json.dumps(vectors(), ensure_ascii=False, indent=1) + "\n"
    if len(argv) == 3 and argv[1] == "--check":
        if Path(argv[2]).read_text(encoding="utf-8") != text:
            print(f"差分あり: {argv[2]}（tools/gen_quality_vectors.py で作り直す）", file=sys.stderr)
            return 1
        print("テストベクタは iotspec の判定と一致")
        return 0
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
