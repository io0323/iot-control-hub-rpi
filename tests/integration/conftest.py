"""結合テスト（Phase 2）の共通部品。開発用コンテナで `make integration` から動かす。

前提：
- Mosquitto（iot-common-spec の deploy/mosquitto の ACL・パスワード）に IOTSPEC_MQTT_URL でつながる
- GW_BIN：iot-gw の実行ファイル。GW_CONFIG：gateway.yaml（既定 ci/gateway.yaml）
- MCU_CPP_01_BIN：作品02 のホスト版（test_work02.py だけが使う）
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import re
import signal
import subprocess
import time
from collections.abc import AsyncIterator, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pytest

from iotspec.sim.mqtt import BrokerAddress, broker_from_env, client_for

ROOT = Path(__file__).resolve().parents[2]
STATS_RE = re.compile(r"event=stats final=true (.*)$", re.MULTILINE)
_START_WAIT_S = 10.0


@pytest.fixture
def broker() -> BrokerAddress:
    os.environ.setdefault("IOTSPEC_MQTT_PASSWORD", "iotspec-dev")
    return broker_from_env()


@dataclass
class Gateway:
    """起動した iot-gw。stop() で SIGTERM を送り、終了時の統計（event=stats final=true）を読む。"""

    process: subprocess.Popen[str]
    log_path: Path
    stats: dict[str, str] = field(default_factory=dict)

    def log(self) -> str:
        return self.log_path.read_text(encoding="utf-8")

    def wait_for_log(self, pattern: str, timeout: float = _START_WAIT_S) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if re.search(pattern, self.log()):
                return True
            time.sleep(0.05)
        return False

    def stop(self) -> dict[str, str]:
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            self.process.wait(timeout=10)
        match = STATS_RE.search(self.log())
        assert match is not None, f"終了時の統計がない:\n{self.log()}"
        self.stats = dict(item.split("=", 1) for item in match.group(1).split())
        return self.stats


@pytest.fixture
def gateway(tmp_path: Path) -> Iterator[Gateway]:
    binary = os.environ.get("GW_BIN")
    if not binary:
        pytest.fail("GW_BIN（iot-gw の実行ファイル）が指定されていない")
    config = os.environ.get("GW_CONFIG", str(ROOT / "ci" / "gateway.yaml"))
    log_path = tmp_path / "gw.log"
    log = log_path.open("w", encoding="utf-8")
    # UART の相手は使わない（開けないパスなので uart-io は再オープンを続けるだけ）
    process = subprocess.Popen(
        [binary, "--config", config, "--uart", "/nonexistent/uart"],
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    gw = Gateway(process, log_path)
    assert gw.wait_for_log(r"event=mqtt_connected"), f"MQTT につながらない:\n{gw.log()}"
    try:
        yield gw
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        log.close()


@dataclass
class Observed:
    topic: str
    message: dict[str, Any] | None
    retain: bool


@contextlib.asynccontextmanager
async def observing(broker: BrokerAddress, topic: str = "iot/v1/home/#") -> AsyncIterator[list[Observed]]:
    """observer の名義で購読し、届いた電文を順に貯める。"""
    seen: list[Observed] = []
    async with client_for(broker, "observer", unique=True) as client:
        await client.subscribe(topic, qos=1)

        async def collect() -> None:
            async for message in client.messages:
                raw = message.payload if isinstance(message.payload, bytes | bytearray) else b""
                try:
                    parsed = json.loads(raw)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    parsed = None
                seen.append(Observed(str(message.topic), parsed, bool(message.retain)))

        task = asyncio.create_task(collect())
        await asyncio.sleep(0.3)
        try:
            yield seen
        finally:
            await asyncio.sleep(0.5)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)


def device_events(seen: list[Observed], code: str, device_id: str) -> list[dict[str, Any]]:
    """gw-01 が発行した code のイベントのうち、detail.device_id が device_id のもの。"""
    return [
        o.message
        for o in seen
        if o.message is not None
        and o.message.get("device_id") == "gw-01"
        and o.message.get("schema") == "iot.v1.event"
        and o.message["payload"]["code"] == code
        and o.message["payload"].get("detail", {}).get("device_id") == device_id
    ]
