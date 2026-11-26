"""Phase 2 の結合テスト：MQTT 取り込み・自動登録・生存監視・重複と欠番。

- iotspec-sim の mcu-cpp-01 代役が state で自動登録され、止める（LWT）と offline になり DEVICE_OFFLINE が出る
- duplicate_and_gap シナリオで、gw が数えた重複・欠番が observer の見た電文から数えたものと一致する
"""

from __future__ import annotations

import asyncio
from collections import defaultdict
from datetime import UTC, datetime
from typing import Any

import pytest
import yaml

from iotspec.clock import format_ts
from iotspec.dedup import exempt_from_seq_checks
from iotspec.sim.mqtt import BrokerAddress, run_over_mqtt
from iotspec.sim.scenario import SCENARIO_DIR, parse_scenario
from iotspec.sim.simulation import Simulation

from .conftest import Gateway, Observed, device_events, observing

pytestmark = pytest.mark.asyncio

SIGNALS = {
    "temp_air": {"kind": "constant", "value": 22.0, "noise": 0.1},
    "humidity": {"kind": "constant", "value": 50.0},
    "pressure": {"kind": "constant", "value": 1012.0},
    "lux": {"kind": "constant", "value": 8000.0},
    "soil": {"kind": "soil", "start": 40.0, "decay_per_hour": 1.0, "refill_per_pump_s": 0.2},
}


async def test_standin_is_registered_and_goes_offline_on_lwt(
    broker: BrokerAddress, gateway: Gateway
) -> None:
    scenario = parse_scenario(
        {
            "name": "phase2_standin",
            "start": format_ts(datetime.now(UTC)),
            "speed": 1,
            "duration_s": 60,
            "devices": [{"device_id": "mcu-cpp-01", "type": "mcu_cpp_01", "signals": SIGNALS}],
        }
    )
    kill = asyncio.Event()
    async with observing(broker) as seen:
        runner = asyncio.create_task(run_over_mqtt(Simulation(scenario), broker, kill=kill))
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_registered device=mcu-cpp-01"
        ), gateway.log()
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=unseen to=online"
        ), gateway.log()
        await asyncio.sleep(3)
        kill.set()  # DISCONNECT を送らずに切る：ブローカーが LWT（offline の state）を発行する
        await runner
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=online to=offline"
        ), gateway.log()
        await asyncio.sleep(0.5)
    offline = device_events(seen, "DEVICE_OFFLINE", "mcu-cpp-01")
    assert len(offline) == 1, [o.message for o in seen if o.message and o.message["device_id"] == "gw-01"]
    assert offline[0]["payload"]["severity"] == "warning"
    stats = gateway.stop()
    assert stats["device_mcu-cpp-01"] == "offline"
    assert int(stats["devices_registered"]) >= 1
    assert "rejected_schema_invalid" not in stats  # 代役の電文はすべてスキーマに適合する


def _expected_dups_and_gaps(seen: list[Observed]) -> tuple[int, int]:
    """observer が見た mcu-cpp-01 の電文から、重複と欠番を数える（iotspec の判定と同じ考え方）。"""
    remembered: dict[tuple[str, str], set[int]] = defaultdict(set)
    last: dict[tuple[str, str], tuple[str, int]] = {}
    dups = gaps = 0
    for o in seen:
        m: dict[str, Any] | None = o.message
        if o.retain or m is None or m.get("device_id") != "mcu-cpp-01" or exempt_from_seq_checks(m):
            continue
        key = (m["schema"], m["boot_id"])
        if m["seq"] in remembered[key]:
            dups += 1
            continue
        remembered[key].add(m["seq"])
        previous = last.get((m["schema"], "seq"))
        if previous is not None and previous[0] == m["boot_id"] and m["seq"] > previous[1] + 1:
            gaps += m["seq"] - previous[1] - 1
        last[(m["schema"], "seq")] = (m["boot_id"], max(m["seq"], previous[1] if previous else -1))
    return dups, gaps


async def test_duplicate_and_gap_are_counted(broker: BrokerAddress, gateway: Gateway) -> None:
    # 開始時刻をいまにし、30倍速（600秒 → 20秒）で流す。faults の時刻は読み込むときに開始時刻から
    # 決まるので、YAML の start を書き換えてから読み込む（読み込んだ後に start だけ変えると faults が過去になる）
    data = yaml.safe_load((SCENARIO_DIR / "duplicate_and_gap.yaml").read_text(encoding="utf-8"))
    data["start"] = format_ts(datetime.now(UTC))
    data["speed"] = 30
    scenario = parse_scenario(data)
    async with observing(broker) as seen:
        await run_over_mqtt(Simulation(scenario), broker)
    stats = gateway.stop()
    dups, gaps = _expected_dups_and_gaps(seen)
    assert dups >= 3  # telemetry の重複2回と state の重複1回（シナリオの faults）
    assert gaps >= 3  # telemetry の seq を3つ飛ばす（＋送信途絶の間の欠番）
    assert int(stats["duplicates"]) == dups, stats
    assert int(stats["gaps"]) == gaps, stats
