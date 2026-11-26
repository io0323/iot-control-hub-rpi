"""Phase 3 の結合テスト：要求 → 調停 → 指令 → 結果。

- app-01 の代役の要求が、UART の mcu-c-01 の代役（iotspec-sim）のファンに届き done が返る
- mcu-cpp-01 の代役への set_actuator と release が done になる
- request_flow シナリオ（app-01 の要求）で、許可されていない操作は not_permitted で拒否され、
  応答しないデバイスへの指令は TIMED_OUT になり failed（target_offline）と COMMAND_FAILED が出る
"""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import time
from collections.abc import Iterator
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import pytest
import yaml

from iotspec.clock import SystemClock, format_ts
from iotspec.ids import new_ulid
from iotspec.sim.mqtt import BrokerAddress, client_for, run_over_mqtt
from iotspec.sim.scenario import SCENARIO_DIR, parse_scenario
from iotspec.sim.simulation import Simulation

from .conftest import GatewayFactory, Observed, clear_retained, observing
from .test_ingest_liveness import SIGNALS

pytestmark = pytest.mark.asyncio

_SIM_START_WAIT_S = 15.0
_FINISH_WAIT_S = 30.0


def _payloads(seen: list[Observed], schema: str, device_id: str | None = None) -> list[dict[str, Any]]:
    return [
        o.message["payload"]
        for o in seen
        if o.message is not None
        and o.message.get("schema") == schema
        and (device_id is None or o.message.get("device_id") == device_id)
    ]


def _request_acks(seen: list[Observed]) -> dict[str, list[dict[str, Any]]]:
    """request_id → gw-01 の request/ack（届いた順）。"""
    result: dict[str, list[dict[str, Any]]] = {}
    for p in _payloads(seen, "iot.v1.request_ack", "gw-01"):
        result.setdefault(p["request_id"], []).append(p)
    return result


def _requests(seen: list[Observed]) -> list[dict[str, Any]]:
    """app-01 が送った request（再送を除く）。"""
    unique: dict[str, dict[str, Any]] = {}
    for p in _payloads(seen, "iot.v1.request", "app-01"):
        unique.setdefault(p["request_id"], p)
    return list(unique.values())


async def _wait(predicate: Any, timeout: float = _FINISH_WAIT_S) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        await asyncio.sleep(0.2)
    return False


def _final(acks: list[dict[str, Any]]) -> str | None:
    finals = [a["status"] for a in acks if a["status"] != "accepted"]
    return finals[-1] if finals else None


def _assert_accepted_then(acks: list[dict[str, Any]], final: str) -> str:
    """accepted（cmd_id 付き）→ final の順で、cmd_id が同じこと。cmd_id を返す。"""
    statuses = [a["status"] for a in acks]
    assert statuses[0] == "accepted", statuses
    assert final in statuses, statuses
    cmd_ids = {a.get("cmd_id") for a in acks}
    assert len(cmd_ids) == 1 and None not in cmd_ids, acks
    return str(acks[0]["cmd_id"])


@pytest.fixture(autouse=True)
def _clear_retained_states(broker: BrokerAddress) -> Iterator[None]:
    yield
    asyncio.run(clear_retained(broker, ("mcu-cpp-01", "app-01", "mcu-c-01")))


@pytest.fixture
def mcu_c_sim(tmp_path: Path) -> Iterator[tuple[str, Path]]:
    """mcu-c-01 の代役と app-01 の代役を iotspec-sim で別プロセスに起動し、代役が作った pty の
    パス（iot-gw が開く側）を返す。app-01 の要求は起動から8秒・14秒（gw の接続を待つ）。"""
    scenario = {
        "name": "phase3_uart_request",
        "start": format_ts(datetime.now(UTC)),
        "speed": 1,
        "duration_s": 40,
        "devices": [
            {
                "device_id": "mcu-c-01",
                "type": "mcu_c_01",
                "signals": {"temp_board": {"kind": "constant", "value": 30.0}},
            },
            {
                "device_id": "app-01",
                "type": "app_01",
                "requests": [
                    {
                        "at_s": 8,
                        "kind": "manual",
                        "target": "mcu-c-01",
                        "op": "set_actuator",
                        "args": {"id": "fan_main", "level": 700},
                        "lease_s": 30,
                    },
                    {
                        "at_s": 14,
                        "kind": "manual",
                        "target": "mcu-c-01",
                        "op": "release",
                        "args": {"id": "fan_main"},
                    },
                ],
            },
        ],
    }
    path = tmp_path / "phase3_uart_request.yaml"
    path.write_text(yaml.safe_dump(scenario, allow_unicode=True), encoding="utf-8")
    log_path = tmp_path / "sim.log"
    env = {**os.environ, "PYTHONUNBUFFERED": "1"}
    process = subprocess.Popen(
        [
            sys.executable,
            "-c",
            "import sys; from iotspec.sim.cli import main; sys.exit(main(sys.argv[1:]))",
            "run",
            str(path),
        ],
        stdout=log_path.open("w", encoding="utf-8"),
        stderr=subprocess.STDOUT,
        env=env,
    )
    try:
        deadline = time.monotonic() + _SIM_START_WAIT_S
        peer: str | None = None
        while time.monotonic() < deadline and peer is None:
            for line in log_path.read_text(encoding="utf-8").splitlines():
                if "相手は " in line and " を開く" in line:
                    peer = line.split("相手は ", 1)[1].split(" を開く", 1)[0]
            time.sleep(0.05)
        assert peer is not None, f"代役が pty を作らない:\n{log_path.read_text(encoding='utf-8')}"
        yield peer, log_path
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


async def test_app_request_reaches_mcu_c_01_fan_and_returns_done(
    broker: BrokerAddress,
    gateway_factory: GatewayFactory,
    mcu_c_sim: tuple[str, Path],
) -> None:
    peer, sim_log = mcu_c_sim
    async with observing(broker) as seen:
        gateway = await asyncio.to_thread(gateway_factory, peer)

        def both_final() -> bool:
            acks = _request_acks(seen)
            sent = _requests(seen)
            return len(sent) == 2 and all(_final(acks.get(r["request_id"], [])) for r in sent)

        assert await _wait(both_final), (
            f"要求の最終結果が来ない\n{_request_acks(seen)}\n{gateway.log()}\n"
            f"{sim_log.read_text(encoding='utf-8')}"
        )
    stats = gateway.stop()
    acks = _request_acks(seen)
    sent = {r["op"]: r for r in _requests(seen)}
    set_cmd = _assert_accepted_then(acks[sent["set_actuator"]["request_id"]], "done")
    release_cmd = _assert_accepted_then(acks[sent["release"]["request_id"]], "done")
    # mcu-c-01 名義の cmd/ack（UART の ACK を変換したもの）が同じ cmd_id で届いている
    cmd_acks = {p["cmd_id"]: p for p in _payloads(seen, "iot.v1.cmd_ack", "mcu-c-01")}
    assert cmd_acks[set_cmd]["status"] == "done"
    assert cmd_acks[set_cmd]["applied"] == {"level": 700}
    assert cmd_acks[release_cmd]["status"] == "done"
    # UART のデバイスへは MQTT の cmd を出さない
    assert not _payloads(seen, "iot.v1.cmd")
    assert int(stats["cmd_done"]) >= 2
    assert int(stats["command_failed_events"]) == 0


async def test_set_actuator_and_release_on_mcu_cpp_01(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart")
    scenario = parse_scenario(
        {
            "name": "phase3_mqtt_request",
            "start": format_ts(datetime.now(UTC)),
            "speed": 1,
            "duration_s": 16,
            "devices": [
                {"device_id": "mcu-cpp-01", "type": "mcu_cpp_01", "signals": SIGNALS},
                {
                    "device_id": "app-01",
                    "type": "app_01",
                    "requests": [
                        {
                            "at_s": 4,
                            "kind": "manual",
                            "target": "mcu-cpp-01",
                            "op": "set_actuator",
                            "args": {"id": "led_grow", "level": 800},
                            "lease_s": 20,
                        },
                        {
                            "at_s": 9,
                            "kind": "manual",
                            "target": "mcu-cpp-01",
                            "op": "release",
                            "args": {"id": "led_grow"},
                        },
                    ],
                },
            ],
        }
    )
    async with observing(broker) as seen:
        await run_over_mqtt(Simulation(scenario), broker)
    stats = gateway.stop()
    acks = _request_acks(seen)
    sent = {r["op"]: r for r in _requests(seen)}
    set_cmd = _assert_accepted_then(acks[sent["set_actuator"]["request_id"]], "done")
    release_cmd = _assert_accepted_then(acks[sent["release"]["request_id"]], "done")
    cmds = {p["cmd_id"]: p for p in _payloads(seen, "iot.v1.cmd", "gw-01")}
    assert cmds[set_cmd]["op"] == "set_actuator"
    assert cmds[set_cmd]["priority"] == "manual"
    assert cmds[set_cmd]["lease_s"] == 20
    assert cmds[set_cmd]["origin"] == {"kind": "request", "ref": sent["set_actuator"]["request_id"]}
    assert cmds[release_cmd]["op"] == "release"
    device_acks = {p["cmd_id"]: p["status"] for p in _payloads(seen, "iot.v1.cmd_ack", "mcu-cpp-01")}
    assert device_acks[set_cmd] == "done"
    assert device_acks[release_cmd] == "done"
    # 代役の telemetry で、指令の間だけ owner が manual になる（共通仕様 第3章の owner）
    owners = [
        r.get("owner")
        for p in _payloads(seen, "iot.v1.telemetry", "mcu-cpp-01")
        for r in p["readings"]
        if r["channel"] == "led_grow"
    ]
    if owners:
        assert "manual" in owners, owners
    assert int(stats["mqtt_cmd_retries"]) == 0


async def _fake_online_state(broker: BrokerAddress, device_id: str) -> None:
    """応答しないデバイス：online の state だけを（retain なしで）発行する。"""
    payload = {
        "status": "online",
        "mode": "auto",
        "channels": [{"channel": "temp_air", "unit": "degC", "period_ms": 1000}],
        "actuators": [
            {"id": "fan_main", "min": 0, "max": 1000, "max_on_s": None},
            {"id": "led_grow", "min": 0, "max": 1000, "max_on_s": None},
            {"id": "pump", "min": 0, "max": 1000, "max_on_s": 30},
            {"id": "buzzer", "min": 0, "max": 1000, "max_on_s": 10},
        ],
        "fw_version": "0.0.1",
    }
    message = {
        "schema": "iot.v1.state",
        "msg_id": new_ulid(SystemClock()),
        "device_id": device_id,
        "seq": 0,
        "boot_id": f"{int(time.time()) & 0xFFFFFFFF:08x}",
        "ts": format_ts(datetime.now(UTC)),
        "ts_source": "device",
        "payload": payload,
    }
    async with client_for(broker, device_id, unique=True) as client:
        await client.publish(f"iot/v1/home/{device_id}/state", json.dumps(message), qos=1)


async def test_request_flow_rejection_and_unresponsive_device(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart")
    raw = yaml.safe_load((SCENARIO_DIR / "request_flow.yaml").read_text(encoding="utf-8"))
    raw["devices"] = [d for d in raw["devices"] if d["device_id"] == "app-01"]  # gw-01 は作品03
    raw["start"] = format_ts(datetime.now(UTC))
    raw["speed"] = 1
    raw["duration_s"] = 22
    scenario = parse_scenario(raw)
    async with observing(broker) as seen:
        await _fake_online_state(broker, "mcu-cpp-01")
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\S+ to=online"
        ), gateway.log()
        await run_over_mqtt(Simulation(scenario), broker)
    stats = gateway.stop()
    acks = _request_acks(seen)
    by_target = {
        (r["op"], (r.get("args") or {}).get("id")): r["request_id"] for r in _requests(seen)
    }
    # requests.yaml で許可していない buzzer は not_permitted（指令を作らない）
    buzzer = acks[by_target[("set_actuator", "buzzer")]]
    assert [a["status"] for a in buzzer][0] == "rejected"
    assert buzzer[0]["reason"] == "not_permitted"
    assert "cmd_id" not in buzzer[0]
    # 応答しないデバイス：2秒×4回で TIMED_OUT → failed（target_offline、open-questions #22）
    for key in (("set_actuator", "pump"), ("set_mode", None), ("set_actuator", "led_grow")):
        statuses = acks[by_target[key]]
        _assert_accepted_then(statuses, "failed")
        assert statuses[-1]["reason"] == "target_offline", statuses
    failed_events = [
        p for p in _payloads(seen, "iot.v1.event", "gw-01") if p["code"] == "COMMAND_FAILED"
    ]
    assert len(failed_events) == 3, failed_events
    assert {e["detail"]["reason"] for e in failed_events} == {"timeout"}
    # 同じ cmd_id で3回再送している
    cmds = _payloads(seen, "iot.v1.cmd", "gw-01")
    per_cmd: dict[str, int] = {}
    for c in cmds:
        per_cmd[c["cmd_id"]] = per_cmd.get(c["cmd_id"], 0) + 1
    assert sorted(per_cmd.values()) == [4, 4, 4], per_cmd
    assert int(stats["cmd_timed_out"]) == 3
