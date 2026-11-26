"""Phase 4 の結合テスト：ルール・システムモード・再読込（基本設計 第8章「構想のイベント／アクション」）。

1. 高温で冷却の指令：iotspec-sim の mcu-cpp-01 の代役が高温を送り続けると、ルールが for の後に発火し、
   hold の指令（set_actuator）を出してリースを更新する。代役は done を返す
2. 高温継続で EMERGENCY と schedule の拒否
3. 承認での NORMAL への復帰（条件が回復していなければ unsafe、回復後は done と SYSTEM_MODE_CHANGED）
4. offline での再接続と通知（MQTT のデバイスには何もせずログに残す）
5. SIGHUP での再読込（成功で入れ替え、失敗で旧いまま）
6. stale での停止（値が途絶えたらリースの更新を止める）

2〜6 は、値を自由に変えられるよう、テストが mcu-cpp-01 の名義で state・telemetry を送り、cmd に done を返す。
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from collections.abc import AsyncIterator, Iterator
from datetime import UTC, datetime
from typing import Any

import pytest

from iotspec.clock import SystemClock, format_ts
from iotspec.ids import new_ulid
from iotspec.sim.mqtt import BrokerAddress, client_for, run_over_mqtt
from iotspec.sim.scenario import parse_scenario
from iotspec.sim.simulation import Simulation

from .conftest import GatewayFactory, Observed, clear_retained, observing
from .test_ingest_liveness import SIGNALS

pytestmark = pytest.mark.asyncio

_WAIT_S = 20.0
_CPP = "mcu-cpp-01"


@pytest.fixture(autouse=True)
def _clear_retained_states(broker: BrokerAddress) -> Iterator[None]:
    yield
    asyncio.run(clear_retained(broker, ("mcu-cpp-01", "app-01", "mcu-c-01")))


def _payloads(
    seen: list[Observed], schema: str, device_id: str | None = None
) -> list[dict[str, Any]]:
    return [
        o.message["payload"]
        for o in seen
        if o.message is not None
        and o.message.get("schema") == schema
        and (device_id is None or o.message.get("device_id") == device_id)
    ]


def _events(seen: list[Observed], code: str) -> list[dict[str, Any]]:
    return [p for p in _payloads(seen, "iot.v1.event", "gw-01") if p["code"] == code]


def _acks(seen: list[Observed], request_id: str) -> list[dict[str, Any]]:
    return [
        p for p in _payloads(seen, "iot.v1.request_ack", "gw-01") if p["request_id"] == request_id
    ]


def _gw_modes(seen: list[Observed]) -> list[str]:
    return [
        p["mode"]
        for p in _payloads(seen, "iot.v1.state", "gw-01")
        if p.get("status") == "online"
    ]


async def _until(predicate: Any, timeout: float = _WAIT_S) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        await asyncio.sleep(0.1)
    return False


def _envelope(schema: str, device_id: str, seq: int, boot_id: str, payload: dict[str, Any]) -> str:
    return json.dumps(
        {
            "schema": schema,
            "msg_id": new_ulid(SystemClock()),
            "device_id": device_id,
            "seq": seq,
            "boot_id": boot_id,
            "ts": format_ts(datetime.now(UTC)),
            "ts_source": "device",
            "payload": payload,
        }
    )


class FakeDevice:
    """mcu-cpp-01 の名義の state・telemetry を送り、届いた cmd に done を返す。"""

    def __init__(self, client: Any) -> None:
        self.client = client
        self.boot_id = f"{int(time.time() * 1000) & 0xFFFFFFFF:08x}"
        self.seq: dict[str, int] = {}
        self.cmds: list[dict[str, Any]] = []

    def _next(self, kind: str) -> int:
        n = self.seq.get(kind, 0)
        self.seq[kind] = n + 1
        return n

    async def state(self, status: str) -> None:
        payload = {
            "status": status,
            "mode": "auto",
            "channels": [
                {"channel": "temp_air", "unit": "degC", "period_ms": 1000},
                {"channel": "humidity", "unit": "pct_rh", "period_ms": 1000},
            ],
            "actuators": [{"id": "fan_main", "min": 0, "max": 1000, "max_on_s": None}],
            "fw_version": "0.0.1",
        }
        await self.client.publish(
            f"iot/v1/home/{_CPP}/state",
            _envelope("iot.v1.state", _CPP, self._next("state"), self.boot_id, payload),
            qos=1,
        )

    async def reading(self, channel: str, value: float) -> None:
        unit = "pct_rh" if channel == "humidity" else "degC"
        payload = {"readings": [{"channel": channel, "value": value, "unit": unit, "quality": "good"}]}
        await self.client.publish(
            f"iot/v1/home/{_CPP}/telemetry",
            _envelope("iot.v1.telemetry", _CPP, self._next("telemetry"), self.boot_id, payload),
            qos=0,
        )

    async def feed(self, channel: str, value: float, seconds: float) -> None:
        """0.5秒ごとに値を送り続ける。"""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            await self.reading(channel, value)
            await asyncio.sleep(0.5)

    async def ack_loop(self) -> None:
        async for message in self.client.messages:
            raw = message.payload if isinstance(message.payload, bytes | bytearray) else b""
            cmd = json.loads(raw)["payload"]
            self.cmds.append(cmd)
            ack: dict[str, Any] = {"cmd_id": cmd["cmd_id"], "status": "done", "limited": False}
            if cmd["op"] == "set_actuator":
                ack["applied"] = {"level": cmd["args"]["level"]}
            ack["reason"] = None
            await self.client.publish(
                f"iot/v1/home/{_CPP}/cmd/ack",
                _envelope("iot.v1.cmd_ack", _CPP, self._next("cmd/ack"), self.boot_id, ack),
                qos=1,
            )


@contextlib.asynccontextmanager
async def fake_device(broker: BrokerAddress) -> AsyncIterator[FakeDevice]:
    async with client_for(broker, _CPP, unique=True) as client:
        await client.subscribe(f"iot/v1/home/{_CPP}/cmd", qos=1)
        device = FakeDevice(client)
        task = asyncio.create_task(device.ack_loop())
        await device.state("online")
        try:
            yield device
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)


async def _request(broker: BrokerAddress, payload: dict[str, Any]) -> None:
    async with client_for(broker, "app-01", unique=True) as client:
        await client.publish(
            "iot/v1/home/app-01/request",
            _envelope("iot.v1.request", "app-01", int(time.time() * 1000) % 1_000_000, "0000a001", payload),
            qos=1,
        )


def _approval(mode: str = "NORMAL") -> dict[str, Any]:
    return {
        "request_id": new_ulid(SystemClock()),
        "kind": "manual",
        "target": "gw-01",
        "op": "set_mode",
        "args": {"mode": mode},
    }


# ------------------------------------------------------------ 1. 高温で冷却の指令

COOL_RULES = """
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    for: 3s
    hysteresis: 2.0
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 15s, hold: true}
"""


async def test_high_temperature_sends_cooling_command_and_renews(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", COOL_RULES)
    hot = {**SIGNALS, "temp_air": {"kind": "constant", "value": 32.0}}
    scenario = parse_scenario(
        {
            "name": "phase4_cooling",
            "start": format_ts(datetime.now(UTC)),
            "speed": 1,
            "duration_s": 16,
            "devices": [{"device_id": _CPP, "type": "mcu_cpp_01", "signals": hot}],
        }
    )
    async with observing(broker) as seen:
        await run_over_mqtt(Simulation(scenario), broker)
    gateway.stop()
    fired = _events(seen, "RULE_FIRED")
    assert [f["detail"] for f in fired] == [{"rule_id": "cool"}], gateway.log()
    cmds = [c for c in _payloads(seen, "iot.v1.cmd", "gw-01") if c["op"] == "set_actuator"]
    cmd_ids = list(dict.fromkeys(c["cmd_id"] for c in cmds))
    assert len(cmd_ids) >= 2, f"リースを更新していない: {cmds}"  # 15秒の3分の1＝5秒ごと
    assert all(c["priority"] == "auto" and c["lease_s"] == 15 for c in cmds)
    assert all(c["origin"] == {"kind": "rule", "ref": "cool"} for c in cmds)
    acks = {p["cmd_id"]: p["status"] for p in _payloads(seen, "iot.v1.cmd_ack", _CPP)}
    assert acks.get(cmd_ids[0]) == "done", acks


# ------------------------------------------------------------ 2・3. EMERGENCY と承認

HEAT_RULES = """
rules:
  - id: heat
    priority: safety
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    for: 2s
    hysteresis: 2.0
    then:
      - {action: set_system_mode, mode: EMERGENCY}
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 1000}, lease: 30s, hold: true}
      - {action: notify, level: critical, message: "高温状態が継続"}
"""


async def test_sustained_heat_enters_emergency_rejects_schedule_and_approval_restores(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", HEAT_RULES)
    async with observing(broker) as seen, fake_device(broker) as dev:
        heat = asyncio.create_task(dev.feed("temp_air", 33.0, 30))
        assert await _until(lambda: _events(seen, "SYSTEM_MODE_CHANGED")), gateway.log()
        entered = _events(seen, "SYSTEM_MODE_CHANGED")[0]
        assert entered["severity"] == "critical"
        assert entered["detail"] == {"from": "NORMAL", "to": "EMERGENCY", "reason": "heat"}
        fired = _events(seen, "RULE_FIRED")[0]
        assert fired["severity"] == "critical" and fired["detail"]["message"] == "高温状態が継続"
        assert await _until(lambda: "EMERGENCY" in _gw_modes(seen))
        assert await _until(lambda: any(c["args"].get("level") == 1000 for c in dev.cmds))

        # EMERGENCY ではスケジュールの要求を拒否する
        schedule = {
            "request_id": new_ulid(SystemClock()),
            "kind": "schedule",
            "target": _CPP,
            "op": "set_actuator",
            "args": {"id": "led_grow", "level": 500},
            "lease_s": 60,
            "schedule": {"id": 7, "planned_at": format_ts(datetime.now(UTC))},
        }
        await _request(broker, schedule)
        assert await _until(lambda: _acks(seen, schedule["request_id"]))
        assert _acks(seen, schedule["request_id"])[0]["reason"] == "emergency_mode"

        # 条件が回復していない承認は unsafe
        early = _approval()
        await _request(broker, early)
        assert await _until(lambda: _acks(seen, early["request_id"]))
        assert _acks(seen, early["request_id"])[0]["reason"] == "unsafe"

        # 回復（28 を下回る）してから承認すると NORMAL に戻る
        heat.cancel()
        await asyncio.gather(heat, return_exceptions=True)
        cool = asyncio.create_task(dev.feed("temp_air", 25.0, 20))
        assert await _until(lambda: _events(seen, "RULE_CLEARED")), gateway.log()
        approval = _approval()
        await _request(broker, approval)
        assert await _until(lambda: len(_acks(seen, approval["request_id"])) >= 2), gateway.log()
        statuses = [a["status"] for a in _acks(seen, approval["request_id"])]
        assert statuses == ["accepted", "done"], statuses
        assert await _until(lambda: len(_events(seen, "SYSTEM_MODE_CHANGED")) >= 2)
        back = _events(seen, "SYSTEM_MODE_CHANGED")[1]
        assert back["severity"] == "info"
        assert back["detail"] == {"from": "EMERGENCY", "to": "NORMAL", "reason": "approved_by_request"}
        assert await _until(lambda: _gw_modes(seen)[-1:] == ["NORMAL"])
        cool.cancel()
        await asyncio.gather(cool, return_exceptions=True)
    gateway.stop()


# ------------------------------------------------------------ 4. offline での再接続と通知

OFFLINE_RULES = """
rules:
  - id: reconnect-on-offline
    priority: auto
    when: {device: "*", status: offline}
    then:
      - {action: reconnect}
      - {action: notify, level: warning, message: "デバイスが offline"}
"""


async def test_offline_triggers_reconnect_and_notify(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", OFFLINE_RULES)
    async with observing(broker) as seen, fake_device(broker) as dev:
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\S+ to=online"
        ), gateway.log()
        await dev.state("offline")
        assert await _until(lambda: _events(seen, "RULE_FIRED")), gateway.log()
        fired = _events(seen, "RULE_FIRED")[0]
        assert fired["severity"] == "warning"
        assert fired["detail"] == {"rule_id": "reconnect-on-offline", "message": "デバイスが offline"}
        # MQTT のデバイスには何もせず、ログに残す（作品03 の接続は健全なので開き直さない）
        assert await asyncio.to_thread(
            gateway.wait_for_log,
            r"event=rule_reconnect rule=reconnect-on-offline uart=false mqtt=false "
            r"mqtt_devices_offline=mcu-cpp-01",
        ), gateway.log()
        await dev.state("online")
        assert await _until(lambda: _events(seen, "RULE_CLEARED"))
    gateway.stop()


# ------------------------------------------------------------ 5. SIGHUP での再読込

def _notify_rule(rule_id: str) -> str:
    return f"""
rules:
  - id: {rule_id}
    priority: auto
    when: {{device: mcu-cpp-01, channel: humidity, op: ">", value: 50.0}}
    then:
      - {{action: notify, level: info, message: "{rule_id}"}}
"""


async def test_sighup_reloads_rules_and_keeps_old_on_failure(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", _notify_rule("before"))
    assert gateway.rules_path is not None
    async with observing(broker) as seen, fake_device(broker) as dev:
        humid = asyncio.create_task(dev.feed("humidity", 60.0, 30))
        assert await _until(lambda: _events(seen, "RULE_FIRED")), gateway.log()
        gateway.rules_path.write_text(_notify_rule("after"), encoding="utf-8")
        gateway.hup()
        assert await asyncio.to_thread(gateway.wait_for_log, r"event=reloaded rules=1"), gateway.log()
        assert await _until(lambda: len(_events(seen, "RULE_FIRED")) >= 2)
        assert [e["detail"]["rule_id"] for e in _events(seen, "RULE_CLEARED")] == ["before"]
        assert _events(seen, "RULE_FIRED")[1]["detail"]["rule_id"] == "after"
        # 検証に失敗したら全部旧いまま（理由をログに出す）
        gateway.rules_path.write_text("rules: [{id: Broken!}]", encoding="utf-8")
        gateway.hup()
        assert await asyncio.to_thread(gateway.wait_for_log, r"event=reload_failed kept=all"), gateway.log()
        await asyncio.sleep(2)
        assert [e["detail"]["rule_id"] for e in _events(seen, "RULE_CLEARED")] == ["before"]
        humid.cancel()
        await asyncio.gather(humid, return_exceptions=True)
    stats = gateway.stop()
    assert stats["rules_fired"] == "2"


# ------------------------------------------------------------ 6. stale での停止

STALE_RULES = """
rules:
  - id: cool
    priority: auto
    when: {device: mcu-cpp-01, channel: temp_air, op: ">=", value: 30.0}
    then:
      - {action: command, device: mcu-cpp-01, op: set_actuator,
         args: {id: fan_main, level: 800}, lease: 6s, hold: true}
"""


async def test_stale_value_stops_lease_renewal(
    broker: BrokerAddress, gateway_factory: GatewayFactory
) -> None:
    gateway = await asyncio.to_thread(gateway_factory, "/nonexistent/uart", STALE_RULES)
    async with observing(broker) as seen, fake_device(broker) as dev:
        await dev.feed("temp_air", 32.0, 7)
        renewing = len({c["cmd_id"] for c in dev.cmds})
        assert renewing >= 3, f"リースを2秒ごとに更新していない: {dev.cmds}"
        # 値が途絶える：period 1000ms の3倍＋判定の1秒で stale。その後は更新しない
        await asyncio.sleep(10)
        assert await asyncio.to_thread(gateway.wait_for_log, r"event=rule_unknown rule=cool"), gateway.log()
        total = len({c["cmd_id"] for c in dev.cmds})
        # stale と判定されるまで（最大で約4秒）の更新は2回まで
        assert total <= renewing + 2, f"stale のあとも更新を続けた: {total} > {renewing} + 2"
        # 値が戻ったら、hold の指令を送り直す
        before = len(dev.cmds)
        await dev.feed("temp_air", 32.0, 2)
        assert len(dev.cmds) > before
        assert await asyncio.to_thread(gateway.wait_for_log, r"event=rule_resumed rule=cool")
    gateway.stop()
