#!/usr/bin/env python3
"""gw-dump-tables の出力を iotspec の constants・mapping と突き合わせる（詳細設計書 第8章の2段目）。

使い方:
    build/debug/bin/gw-dump-tables > tables.json
    uv run --with "iotspec @ git+https://github.com/io0323/iot-common-spec@v0.2.2" \\
        python tools/check_tables.py tables.json

差分があれば項目ごとに表示して終了コード 1。
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from iotspec import constants
from iotspec.uart import frames, mapping


def _table(t: mapping.ValueTable) -> dict[str, Any]:
    return {str(k): t.to_common(k) for k in t.uart_values()}


def expected() -> dict[str, Any]:
    return {
        "mapping": {
            "QUALITY": _table(mapping.QUALITY),
            "STATUS": _table(mapping.STATUS),
            "REASON": _table(mapping.REASON),
            "EVENT_CODE": _table(mapping.EVENT_CODE),
            "SEVERITY": _table(mapping.SEVERITY),
            "RESET_REASON": _table(mapping.RESET_REASON),
        },
        "kinds": {
            kind: {
                "schema": constants.SCHEMA_NAMES[kind],
                "qos": constants.QOS_BY_KIND[kind],
                "retain": kind in constants.RETAIN_KINDS,
            }
            for kind in constants.KINDS
        },
        "topic_format": constants.TOPIC_FORMAT,
        "uart_op_names": {str(k): v for k, v in constants.UART_OP_NAMES.items()},
        "cmd_arg_names": {str(k): list(v) for k, v in frames.CMD_ARG_NAMES.items()},
    }


def compare(actual: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    want = expected()
    for key, value in want.items():
        if actual.get(key) != value:
            problems.append(f"{key}: C++ {json.dumps(actual.get(key), ensure_ascii=False)} "
                            f"≠ iotspec {json.dumps(value, ensure_ascii=False)}")
    for name, value in actual.get("constants", {}).items():
        if not hasattr(constants, name):
            problems.append(f"constants.{name}: iotspec にない")
        elif getattr(constants, name) != value:
            problems.append(f"constants.{name}: C++ {value!r} ≠ iotspec {getattr(constants, name)!r}")
    # iotspec に新しく増えた UART・送信量の定数を見落とさない
    for name in dir(constants):
        if name.startswith(("UART_", "CRC16_", "PUBLISH_RATE_", "LEASE_")) and name not in actual.get(
            "constants", {}
        ):
            if isinstance(getattr(constants, name), int | str):
                problems.append(f"constants.{name}: C++ にない")
    return problems


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    actual = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    problems = compare(actual)
    for p in problems:
        print(f"差分: {p}")
    if not problems:
        print(f"対応表と定数は iotspec と一致（定数 {len(actual.get('constants', {}))} 件）")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
