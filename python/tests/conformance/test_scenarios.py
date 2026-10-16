"""`vectors/scenarios/*.json` through both libraries.

A scenario is a device profile, a starting store, and a list of steps. Each step sets
the clock, hands the node the frames in `rx`, sets the pin if given, and runs ONE turn
of the application loop (node tick, then the key or output logic) — the same turn
`devices/apps/` runs on a board. Then everything the node sent is compared with
`expect`, **exactly**: a frame too many is as much a failure as one missing.

Frames are written as `candump` does — `IDENTIFIER#DATA`, both in hex.

The service button and the status LED (zadania/zrobione/CR-05.md): `reset_reason` at the top is
what the platform says the last restart was, `bus` (at the top or in a step) is what the
port's `health` answers from then on — `active`, `warning`, `passive`, `off`. In a step,
`errors: [tec, rec]` (at the top or in a step) are the controller's error counters
`health` answers from then on, `service: true` presses the service button before the
turn, and `state` is the state the node last REPORTED to its state watch —
`on_start_state` the same after the start.

A measurement (types/0x0030-sense.md): `sense` in a step maps an endpoint number to the
raw value its sensor read, `null` for a failed reading — handed in before the turn, as a
board's application does after reading its sensor.

A turn (types/0x0011-value-input.md): `turn` in a step maps an endpoint number to the
detents of its value input, negative down — what a board's interrupt counted since the
last loop, handed to the library at the turn as one step of several
(zadania/zrobione/DEVICE-02.md N11).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from lagunbus_node.frame import Frame, decode_header, decode_id, is_join
from lagunbus_node.registry import MessageKind, ResultCode
from tests.conformance.conftest import VECTORS, DeviceFactory

SCENARIOS = sorted((VECTORS / "scenarios").glob("*.json"))


def parse(text: str) -> Frame:
    can_id, _, data = text.partition("#")
    return Frame(int(can_id, 16), bytes.fromhex(data))


def show(f: Frame) -> str:
    """A frame with its meaning, for the failure message — bytes alone make a diff
    of two frame lists unreadable."""
    raw = f"{f.can_id:08X}#{f.data.hex().upper()}"
    if is_join(f.can_id):
        return f"{raw}  JOIN"
    fields = decode_id(f.can_id)
    try:
        kind = MessageKind(fields.kind).name
    except ValueError:
        kind = f"kind {fields.kind}"
    meaning = f"{kind} 0x{fields.address:03X} ep{fields.endpoint} el 0x{fields.element:02X}"
    if fields.kind == MessageKind.RESPONSE and f.data:
        tid, result = decode_header(f.data[0])
        try:
            meaning += f" tid={tid} {ResultCode(result).name}"
        except ValueError:
            meaning += f" tid={tid} result {result}"
    return f"{raw}  {meaning}"


def check(where: str, sent: list[Frame], expected: list[str]) -> None:
    want = [parse(e) for e in expected]
    if sent != want:

        def listing(frames: list[Frame]) -> list[str]:
            return [f"    {show(f)}" for f in frames] or ["    nothing"]

        lines = [f"{where}: the node sent", *listing(sent), "  expected", *listing(want)]
        pytest.fail("\n".join(lines), pytrace=False)


def pin_mask(pin: bool | list[bool]) -> int:
    """`pin` is one level for a device with one input, a list for one with several —
    as the mask of pin levels a board reads, bit i for input i."""
    if isinstance(pin, bool):
        return int(pin)
    return sum(1 << i for i, level in enumerate(pin) if level)


@pytest.mark.parametrize("path", SCENARIOS, ids=[p.stem for p in SCENARIOS])
def test_scenario(device: DeviceFactory, path: Path) -> None:
    s: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    node = device(s["profile"], bytes.fromhex(s["uid"]), s["jitter"], s.get("channels", 0))
    if s.get("restart", False):
        node.allow_restart()
    for key, value in s.get("store", {}).items():
        node.store_put(key, bytes.fromhex(value))
    if "reset_reason" in s:
        node.set_reset_reason(s["reset_reason"])
    if "bus" in s:
        node.set_bus(s["bus"])
    if "errors" in s:
        node.set_errors(*s["errors"])

    pins = pin_mask(s.get("pin", False))
    node.set_clock(s.get("start_at", 0))
    node.start(pins)
    check("start", node.take_tx(), s.get("on_start", []))
    if "on_start_state" in s:
        assert node.state() == s["on_start_state"], f"start: state {node.state()}"

    for i, step in enumerate(s["steps"]):
        where = f"step {i} at {step['at']} ms" + (f" ({step['note']})" if "note" in step else "")
        node.set_clock(step["at"])
        for text in step.get("rx", []):
            node.rx(parse(text))
        if "pin" in step:
            pins = pin_mask(step["pin"])
        if "bus" in step:
            node.set_bus(step["bus"])
        if "errors" in step:
            node.set_errors(*step["errors"])
        if step.get("service", False):
            node.service_press()
        for endpoint, raw in step.get("sense", {}).items():
            node.sense(int(endpoint), raw)
        for endpoint, detents in step.get("turn", {}).items():
            node.turn(int(endpoint), detents)
        node.loop(pins)
        check(where, node.take_tx(), step.get("expect", []))

        for key, value in step.get("store", {}).items():
            stored = node.store_get(key)
            wanted = None if value is None else bytes.fromhex(value)
            assert stored == wanted, f"{where}: store[{key!r}] = {stored!r}, expected {wanted!r}"
        if "state" in step:
            assert node.state() == step["state"], f"{where}: state {node.state()}"
        if "restarted" in step:
            assert node.restart_requested() is step["restarted"], f"{where}: restart"
        if "driven" in step:
            # One level for a single output, a list for a device with several.
            wanted_levels = step["driven"] if isinstance(step["driven"], list) else [step["driven"]]
            levels = [node.driven(i) for i in range(len(wanted_levels))]
            assert levels == wanted_levels, f"{where}: the outputs drove {levels}"
