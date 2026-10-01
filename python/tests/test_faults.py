"""The fault overlay — each of the four faults, seen from the bus.

The node behind it is the C library (`lagunbus_node.sim`), the one that goes onto a
board: the overlay works on frames alone, so it needs no hook in the node and no
callback into Python (zadania/zrobione/FEAT-02.md, N10).
"""

from __future__ import annotations

import pytest

from lagunbus_node.faults import Faults, FaultyTransport
from lagunbus_node.frame import Frame
from lagunbus_node.memory import QueueTransport
from lagunbus_node.sim import SimDevice

UID = bytes.fromhex("A1B2C3D400010203")
TOGGLE = Frame(0x06108402, b"\x01")
READ_UID = Frame(0x0C108000, b"\x03")
READ_BOOT = Frame(0x0C108011, b"\x01")


class Rig:
    """An `output` device at 0x042 behind the overlay, pumped as a simulator pumps it:
    bus → overlay → node → overlay → bus."""

    def __init__(self) -> None:
        self.bus = QueueTransport()
        self.faulty = FaultyTransport(self.bus)
        self.device = SimDevice("output", UID)
        self.device.store_put("address", b"\x42\x00")
        self.clock = 0
        self.node_sent = 0
        self.faulty.attach(lambda: self.clock, lambda: self.device.address)
        self.device.start(0)
        self._out()
        self.bus.take()  # Booted

    @property
    def faults(self) -> Faults:
        return self.faulty.faults

    def _out(self) -> None:
        for frame in self.device.take_tx():
            self.node_sent += 1
            self.faulty.send(frame)

    def ask(self, frame: Frame, at: int = 0) -> list[str]:
        self.clock = at
        self.device.set_clock(at)
        self.bus.inbox.append(frame)
        while (incoming := self.faulty.recv()) is not None:
            self.device.rx(incoming)
        self.device.loop(0)
        self._out()
        return [f"{f.can_id:08X}#{f.data.hex().upper()}" for f in self.bus.take()]


@pytest.fixture
def rig() -> Rig:
    return Rig()


def test_no_faults_is_transparent(rig: Rig) -> None:
    assert rig.ask(READ_BOOT) == ["0E108011#010100"]


def test_a_dropped_response_still_carries_the_command_out(rig: Rig) -> None:
    rig.faults.drop_responses = 1
    assert rig.ask(TOGGLE) == ["10108400#01"]  # the report goes; the reply does not
    assert rig.device.output_on(0)
    assert rig.faults.drop_responses == 0
    assert rig.ask(TOGGLE, at=500) == ["10108400#00", "0E108402#01"]


def test_busy_refuses_without_reaching_the_node(rig: Rig) -> None:
    rig.faults.busy_responses = 1
    assert rig.ask(TOGGLE) == ["0E108402#41"]  # tid 1, BUSY
    assert not rig.device.output_on(0)
    assert rig.ask(TOGGLE, at=10) == ["10108400#01", "0E108402#01"]


def test_busy_leaves_other_nodes_requests_alone(rig: Rig) -> None:
    rig.faults.busy_responses = 1
    assert rig.ask(Frame(0x0610C402, b"\x01")) == []  # for 0x043
    assert rig.faults.busy_responses == 1


def test_silence_swallows_everything_until_it_ends(rig: Rig) -> None:
    rig.faults.silent_until = 1000
    before = rig.node_sent
    assert rig.ask(READ_BOOT, at=999) == []
    assert rig.node_sent > before  # the node believes it sent
    assert rig.ask(READ_BOOT, at=1000) == ["0E108011#010100"]


def test_a_dropped_segment_leaves_a_hole(rig: Rig) -> None:
    rig.faults.drop_segment = 0
    assert rig.ask(READ_UID) == ["0E108000#5B", "12108000#830100010203"]
    assert rig.faults == Faults()  # used up
    assert len(rig.ask(READ_UID, at=10)) == 3
