"""`vectors/scenarios/*.json` through both libraries.

A scenario is a device profile, a starting store, and a list of steps. Each step sets
the clock, hands the node the frames in `rx`, sets the pin if given, and runs ONE turn
of the application loop (node tick, then the key or output logic) — the same turn
`devices/apps/` runs on a board. Then everything the node sent is compared with
`expect`, **exactly**: a frame too many is as much a failure as one missing.

Frames are written as `candump` does — `IDENTIFIER#DATA`, both in hex.
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


@pytest.mark.parametrize("path", SCENARIOS, ids=[p.stem for p in SCENARIOS])
def test_scenario(device: DeviceFactory, path: Path) -> None:
    s: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    node = device(s["profile"], bytes.fromhex(s["uid"]), s["jitter"])
    for key, value in s.get("store", {}).items():
        node.store_put(key, bytes.fromhex(value))

    pin = bool(s.get("pin", False))
    node.set_clock(s.get("start_at", 0))
    node.start(pin)
    check("start", node.take_tx(), s.get("on_start", []))

    for i, step in enumerate(s["steps"]):
        where = f"step {i} at {step['at']} ms" + (f" ({step['note']})" if "note" in step else "")
        node.set_clock(step["at"])
        for text in step.get("rx", []):
            node.rx(parse(text))
        if "pin" in step:
            pin = bool(step["pin"])
        node.loop(pin)
        check(where, node.take_tx(), step.get("expect", []))

        for key, value in step.get("store", {}).items():
            stored = node.store_get(key)
            wanted = None if value is None else bytes.fromhex(value)
            assert stored == wanted, f"{where}: store[{key!r}] = {stored!r}, expected {wanted!r}"
        if "driven" in step:
            assert node.driven() == step["driven"], f"{where}: the output drove {node.driven()}"
