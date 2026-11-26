"""開発用コンテナに入った iotspec が、作品03 の前提とする共通仕様の検査を持つことを確かめる。

uv は版番号が同じだと前に作ったパッケージを使い回すことがあるので、イメージを作るときに確かめる
（deploy/Dockerfile.dev）。v0.2.3 #48：SYSTEM_MODE_CHANGED の detail.from・to が必須。
"""

from __future__ import annotations

import sys

from iotspec.schema import validate


def event(code: str, detail: dict[str, object]) -> dict[str, object]:
    return {
        "schema": "iot.v1.event",
        "msg_id": "01JD3R0A5K7M2N4P6R8T0V2X4Y",
        "device_id": "gw-01",
        "seq": 0,
        "boot_id": "0badcafe",
        "ts": "2026-11-26T06:00:00.000Z",
        "ts_source": "device",
        "payload": {"code": code, "severity": "info", "detail": detail},
    }


def main() -> int:
    problems = []
    if not validate(event("SYSTEM_MODE_CHANGED", {"to": "NORMAL"})):
        problems.append("SYSTEM_MODE_CHANGED の detail.from を検査しない（#48）")
    if validate(event("SYSTEM_MODE_CHANGED", {"from": "EMERGENCY", "to": "NORMAL", "reason": "x"})):
        problems.append("正しい SYSTEM_MODE_CHANGED を拒否する")
    if not validate(event("RULE_FIRED", {})):
        problems.append("RULE_FIRED の detail.rule_id を検査しない（#47）")
    for p in problems:
        print(f"iotspec の版が古い: {p}", file=sys.stderr)
    if not problems:
        print("iotspec：#47・#48 の検査あり")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
