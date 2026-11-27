"""結合テスト（Phase 2・3）の共通部品。開発用コンテナで `make integration` から動かす。

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
import tempfile
import time
from collections.abc import AsyncIterator, Callable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import IO, Any, Protocol

import pytest
import yaml

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
    rules_path: Path | None = None
    """テストが渡した rules.yaml（書き換えて hup() で読み直させる）。"""
    state_dir: Path | None = None
    """履歴DB（history.db）と gwctl のソケット（gwctl.sock）を置くディレクトリ。"""

    @property
    def socket(self) -> Path:
        assert self.state_dir is not None
        return self.state_dir / "gwctl.sock"

    def gwctl(self, *args: str) -> dict[str, Any]:
        """gwctl --json を実行して応答（{ok, result|error}）を返す。"""
        binary = Path(os.environ["GW_BIN"]).parent / "gwctl"
        done = subprocess.run(
            [str(binary), "--socket", str(self.socket), "--json", *args],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert done.stdout.strip(), f"gwctl が何も出さない: {done.stderr}"
        result: dict[str, Any] = json.loads(done.stdout)
        return result

    def hup(self) -> None:
        """SIGHUP：rules.yaml・devices.yaml・requests.yaml を読み直させる。"""
        self.process.send_signal(signal.SIGHUP)

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


class GatewayFactory(Protocol):
    def __call__(
        self, uart: str, rules: str | None = None, state_dir: Path | None = None
    ) -> Gateway: ...


@pytest.fixture
def gateway_factory(tmp_path: Path) -> Iterator[GatewayFactory]:
    """iot-gw を起動する関数。引数は UART のパス（UART の相手を使わないなら開けないパス）。"""
    binary = os.environ.get("GW_BIN")
    if not binary:
        pytest.fail("GW_BIN（iot-gw の実行ファイル）が指定されていない")
    config = os.environ.get("GW_CONFIG", str(ROOT / "ci" / "gateway.yaml"))
    started: list[tuple[subprocess.Popen[str], IO[str]]] = []

    def start(uart: str, rules: str | None = None, state_dir: Path | None = None) -> Gateway:
        """ci/gateway.yaml をもとに、このテストの gateway.yaml を一時ディレクトリに作る。
        rules を渡すとその rules.yaml を使う。履歴DB と gwctl のソケットは state_dir（省略時は
        起動ごとに新しいディレクトリ）に置く。同じ state_dir で起動し直すと再起動になる。"""
        n = len(started)
        log_path = tmp_path / f"gw-{n}.log"
        log = log_path.open("w", encoding="utf-8")
        base_config = Path(config)
        base = yaml.safe_load(base_config.read_text(encoding="utf-8"))
        ci = base_config.parent
        state = state_dir or Path(tempfile.mkdtemp(prefix=f"gw{n}-", dir="/tmp"))
        state.mkdir(parents=True, exist_ok=True)
        base.update(
            {
                "devices_file": str((ci / "devices.yaml").resolve()),
                "requests_file": str((ci / "requests.yaml").resolve()),
                "rules_file": str((ci / "rules.yaml").resolve()),
                "history": {"db_path": str(state / "history.db")},
                "admin": {"socket": str(state / "gwctl.sock")},  # UNIX ソケットのパスは短く
            }
        )
        rules_path: Path | None = None
        if rules is not None:
            rules_path = tmp_path / f"rules-{n}.yaml"
            rules_path.write_text(rules, encoding="utf-8")
            base["rules_file"] = str(rules_path)
        gateway_config = tmp_path / f"gateway-{n}.yaml"
        gateway_config.write_text(yaml.safe_dump(base), encoding="utf-8")
        process = subprocess.Popen(
            [binary, "--config", str(gateway_config), "--uart", uart],
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        started.append((process, log))
        gw = Gateway(process, log_path, rules_path=rules_path, state_dir=state)
        assert gw.wait_for_log(r"event=mqtt_connected"), f"MQTT につながらない:\n{gw.log()}"
        return gw

    try:
        yield start
    finally:
        for process, log in started:
            if process.poll() is None:
                process.kill()
                process.wait()
            log.close()


@pytest.fixture
def gateway(gateway_factory: GatewayFactory) -> Gateway:
    # UART の相手は使わない（開けないパスなので uart-io は再オープンを続けるだけ）
    return gateway_factory("/nonexistent/uart")


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


async def clear_retained(broker: BrokerAddress, device_ids: tuple[str, ...]) -> None:
    """テストが残した retain の state を消す（後のテストが「unseen から」を前提にするため）。

    mcu-c-01 の state は gw-01 が発行するので、gw-01 の名義で消す（ACL：bridged のデバイス）。
    """
    for device_id in device_ids:
        owner = "gw-01" if device_id == "mcu-c-01" else device_id
        async with client_for(broker, owner, unique=True) as client:
            await client.publish(f"iot/v1/home/{device_id}/state", b"", qos=1, retain=True)
