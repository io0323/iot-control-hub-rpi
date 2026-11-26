"""作品02（io0323/iot-modular-node-esp32）のホスト版を相手にした結合テスト。

作品02 は共通仕様 v0.2.1 の電文を送る（v0.2.2 の Schema でも通ることを確かめる）。
state で自動登録され、強制終了（LWT）で offline になり DEVICE_OFFLINE が出る。
MCU_CPP_01_BIN に作品02 のホスト版の実行ファイルを指定する（make integration が用意する）。
"""

from __future__ import annotations

import asyncio
import os
import subprocess
from pathlib import Path

import pytest

from iotspec.sim.mqtt import BrokerAddress

from .conftest import Gateway, device_events, observing

pytestmark = pytest.mark.asyncio


async def test_work02_host_is_registered_and_goes_offline_on_kill(
    broker: BrokerAddress, gateway: Gateway, tmp_path: Path
) -> None:
    binary = os.environ.get("MCU_CPP_01_BIN")
    if not binary:
        pytest.fail("MCU_CPP_01_BIN（作品02 のホスト版）が指定されていない")
    async with observing(broker) as seen:
        log = (tmp_path / "work02.log").open("w", encoding="utf-8")
        device = subprocess.Popen(
            [binary, "--broker", f"{broker.host}:{broker.port}"], stdout=log, stderr=subprocess.STDOUT
        )
        try:
            assert await asyncio.to_thread(
                gateway.wait_for_log, r"event=device_registered device=mcu-cpp-01", 15
            ), gateway.log()
            assert await asyncio.to_thread(
                gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\w+ to=online", 15
            ), gateway.log()
            await asyncio.sleep(5)  # telemetry をいくつか受ける
        finally:
            device.kill()  # 強制終了：ブローカーが LWT を発行する
            device.wait()
            log.close()
        assert await asyncio.to_thread(
            gateway.wait_for_log, r"event=device_status device=mcu-cpp-01 from=\w+ to=offline", 15
        ), gateway.log()
        await asyncio.sleep(0.5)
    assert len(device_events(seen, "DEVICE_OFFLINE", "mcu-cpp-01")) == 1
    stats = gateway.stop()
    assert stats["device_mcu-cpp-01"] == "offline"
    # 作品02 の電文（v0.2.1）はすべて v0.2.2 の Schema に適合する
    assert not [k for k in stats if k.startswith("rejected_")], stats
    assert int(stats["mqtt_accepted"]) > 3
