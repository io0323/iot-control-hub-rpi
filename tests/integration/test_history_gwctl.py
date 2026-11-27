"""Phase 5 の結合テスト：履歴・起動時の復元・gwctl。

1. 再起動をまたいで、同じ request_id の要求が重複として扱われる（新しい指令を作らず、前の結果を返す）
2. `gwctl diag mcu-c-01` で mcu-c-01 の代役（iotspec-sim）の DIAG が見え、MQTT には出ていない
3. `gwctl approve` で EMERGENCY から NORMAL に戻り、reason が approved_by_gwctl
4. `gwctl cmd` の手動の指令が auto のルールに勝つ（ルールの更新が手動の制御を上書きしない）
5. EMERGENCY のまま再起動すると EMERGENCY で起動する
"""

from __future__ import annotations

import asyncio
import os
import subprocess
import sys
import time
from collections.abc import Iterator
from datetime import UTC, datetime
from pathlib import Path
import pytest
import yaml

from iotspec.clock import SystemClock, format_ts
from iotspec.ids import new_ulid
from iotspec.sim.mqtt import BrokerAddress

from .conftest import GatewayFactory, clear_retained, observing
from .test_rules_mode import _CPP, _events, _payloads, _request, _until, fake_device

pytestmark = pytest.mark.asyncio


@pytest.fixture(autouse=True)
def _clear_retained_states(broker: BrokerAddress) -> Iterator[None]:
    yield
    asyncio.run(clear_retained(broker, ("mcu-cpp-01", "app-01", "mcu-c-01")))


HEAT_RULES = """
rules:
  - id: heat
    priority: safety
    when: {device: mcu-cpp-01, channel: humidity, op: ">=", value: 90.0}
    then:
      - {action: set_system_mode, mode: EMERGENCY}
"""

COOL_RULES = """
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 6s, hold: true}
"""


async def test_request_id_is_remembered_across_restart(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    state = Path(f"/tmp/gw-restart-{os.getpid()}")
    request = {
        "request_id": new_ulid(SystemClock()),
        "kind": "manual",
        "target": _CPP,
        "op": "set_actuator",
        "args": {"id": "led_grow", "level": 300},
        "lease_s": 20,
    }
    async with observing(broker) as seen, fake_device(broker) as dev:
        first = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", None, state)
        await dev.state("online")
        assert await asyncio.to_thread(
            first.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\S+ to=online"
        ), first.log()
        await _request(broker, request)
        assert await _until(
            lambda: any(
                p["status"] == "done"
                for p in _payloads(seen, "iot.v1.request_ack", "gw-01")
                if p["request_id"] == request["request_id"]
            )
        ), first.log()
        commands_before = len(dev.cmds)
        first.stop()  # 正常終了：履歴を書き切る
        second = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", None, state)
        assert await asyncio.to_thread(
            second.wait_for_log, r"event=history_restored commands_closed=0 requests=1"
        ), second.log()
        await dev.state("online")
        assert await asyncio.to_thread(
            second.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\S+ to=online"
        ), second.log()
        since = len(seen)
        await _request(broker, request)  # 作品04 が同じ request_id で送り直した
        assert await _until(
            lambda: any(
                p["request_id"] == request["request_id"]
                for p in _payloads(seen[since:], "iot.v1.request_ack", "gw-01")
            )
        ), second.log()
        acks = [
            p
            for p in _payloads(seen[since:], "iot.v1.request_ack", "gw-01")
            if p["request_id"] == request["request_id"]
        ]
        assert acks[0]["status"] == "done", acks  # 前の結果
        await asyncio.sleep(1)
        assert len(dev.cmds) == commands_before, "再起動のあと同じ要求で新しい指令を作った"
    stats = second.stop()
    assert stats["request_duplicates"] == "1"


@pytest.fixture
def mcu_c_sim(tmp_path: Path) -> Iterator[str]:
    """mcu-c-01 の代役だけを iotspec-sim で別プロセスに起動し、iot-gw が開く pty のパスを返す。"""
    scenario = {
        "name": "phase5_diag",
        "start": format_ts(datetime.now(UTC)),
        "speed": 1,
        "duration_s": 40,
        "devices": [
            {
                "device_id": "mcu-c-01",
                "type": "mcu_c_01",
                "reset_reason": "watchdog",
                "signals": {"temp_board": {"kind": "constant", "value": 30.0}},
            }
        ],
    }
    path = tmp_path / "phase5_diag.yaml"
    path.write_text(yaml.safe_dump(scenario, allow_unicode=True), encoding="utf-8")
    log_path = tmp_path / "sim.log"
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
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    try:
        peer: str | None = None
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and peer is None:
            for line in log_path.read_text(encoding="utf-8").splitlines():
                if "相手は " in line and " を開く" in line:
                    peer = line.split("相手は ", 1)[1].split(" を開く", 1)[0]
            time.sleep(0.05)
        assert peer is not None, log_path.read_text(encoding="utf-8")
        yield peer
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


async def test_gwctl_diag_shows_mcu_c_01_diag_not_on_mqtt(
    broker: BrokerAddress, gateway_factory: GatewayFactory, mcu_c_sim: str
) -> None:
    async with observing(broker) as seen:
        gateway = await asyncio.to_thread(gateway_factory, mcu_c_sim)
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-c-01 from=\S+ to=online", 20
        ), gateway.log()
        reply = await asyncio.to_thread(gateway.gwctl, "diag", "mcu-c-01")
        assert reply["ok"], reply
        diag = reply["result"]["diag"]
        assert reply["result"]["status"] == "received"
        assert diag["device_id"] == "mcu-c-01"
        assert diag["reset_reason"] == 3  # watchdog（v0.2.2 #38 の表）
        for key in ("uptime_ms", "reset_count", "rx_crc_errors", "rx_frame_errors", "max_loop_us",
                    "overruns"):
            assert key in diag
        await asyncio.sleep(1)
    gateway.stop()
    # DIAG とその ACK は MQTT に出ない（v0.2.2 #37）
    for o in seen:
        assert o.message is None or "reset_count" not in str(o.message), o
    assert not [
        p for p in _payloads(seen, "iot.v1.cmd_ack", "mcu-c-01") if p.get("cmd_id") == reply["result"]["cmd_id"]
    ]
    assert "event=uart_diag device=mcu-c-01" in gateway.log()


async def test_gwctl_approve_returns_from_emergency(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", HEAT_RULES)
    async with observing(broker) as seen, fake_device(broker) as dev:
        await dev.feed("humidity", 95.0, 2)
        assert await _until(lambda: _events(seen, "SYSTEM_MODE_CHANGED")), gateway.log()
        early = await asyncio.to_thread(gateway.gwctl, "approve")
        assert early["result"]["status"] == "rejected" and early["result"]["reason"] == "unsafe"
        await dev.feed("humidity", 50.0, 2)
        assert await _until(lambda: _events(seen, "RULE_CLEARED"))
        reply = await asyncio.to_thread(gateway.gwctl, "approve")
        assert reply["ok"] and reply["result"]["status"] == "done", reply
        assert reply["result"]["mode"] == "NORMAL"
        assert await _until(lambda: len(_events(seen, "SYSTEM_MODE_CHANGED")) >= 2)
        back = _events(seen, "SYSTEM_MODE_CHANGED")[1]
        assert back["detail"] == {"from": "EMERGENCY", "to": "NORMAL", "reason": "approved_by_gwctl"}
        rules = await asyncio.to_thread(gateway.gwctl, "rules")
        assert rules["result"]["mode"] == "NORMAL"
    gateway.stop()


async def test_gwctl_cmd_beats_the_auto_rule(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", COOL_RULES)
    async with observing(broker) as seen, fake_device(broker) as dev:
        feeding = asyncio.create_task(dev.feed("temp_air", 32.0, 25))
        assert await _until(lambda: any(c["args"]["level"] == 800 for c in dev.cmds)), gateway.log()
        reply = await asyncio.to_thread(
            gateway.gwctl, "cmd", _CPP, "set_actuator", "id=fan_main", "level=300", "--lease", "30"
        )
        assert reply["ok"] and reply["result"]["status"] == "done", reply
        manual = [c for c in dev.cmds if c["cmd_id"] == reply["result"]["cmd_id"]]
        assert manual and manual[0]["priority"] == "manual"
        assert manual[0]["origin"]["kind"] == "gwctl"
        after = len(dev.cmds)
        await asyncio.sleep(8)  # ルールのリース更新（2秒ごと）は手動の制御を上書きしない
        autos = [c for c in dev.cmds[after:] if c["args"].get("level") == 800]
        assert not autos, f"auto のルールが手動の指令を上書きした: {autos}"
        feeding.cancel()
        await asyncio.gather(feeding, return_exceptions=True)
        # gwctl の要求は作品04 宛ての request/ack にならない
        assert not [
            p
            for p in _payloads(seen, "iot.v1.request_ack", "gw-01")
            if p.get("cmd_id") == reply["result"]["cmd_id"]
        ]
        devices = await asyncio.to_thread(gateway.gwctl, "devices")
        assert any(d["device_id"] == _CPP and d["status"] == "online" for d in devices["result"])
        stats = await asyncio.to_thread(gateway.gwctl, "stats")
        assert stats["result"]["process"]["history"]["written"] > 0
    gateway.stop()


async def test_emergency_survives_restart(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    state = Path(f"/tmp/gw-emergency-{os.getpid()}")
    first = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", HEAT_RULES, state)
    async with observing(broker) as seen, fake_device(broker) as dev:
        await dev.feed("humidity", 95.0, 2)
        assert await _until(lambda: _events(seen, "SYSTEM_MODE_CHANGED")), first.log()
    first.stop()
    async with observing(broker) as seen:
        second = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", HEAT_RULES, state)
        assert await asyncio.to_thread(
            second.wait_for_log, r"event=history_restored .* mode=EMERGENCY"
        ), second.log()
        assert await _until(
            lambda: any(
                p.get("mode") == "EMERGENCY" and p.get("status") == "online"
                for p in _payloads(seen, "iot.v1.state", "gw-01")
            )
        ), second.log()
        rules = await asyncio.to_thread(second.gwctl, "rules")
        assert rules["result"]["mode"] == "EMERGENCY"
        # 条件はまだ判定できていない（値が来ていない）ので heat は idle：承認で戻せる
        reply = await asyncio.to_thread(second.gwctl, "approve")
        assert reply["result"]["status"] == "done", reply
    second.stop()
