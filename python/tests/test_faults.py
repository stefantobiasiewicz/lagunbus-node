"""The fault overlay — each of the four faults, seen from the bus."""

from __future__ import annotations

import pytest

from lagunbus_node.faults import Faults, FaultyTransport
from lagunbus_node.frame import Frame
from lagunbus_node.memory import MemoryPlatform, QueueTransport
from lagunbus_node.node import Node
from lagunbus_node.output import Output
from tests.conformance import profiles

UID = bytes.fromhex("A1B2C3D400010203")
TOGGLE = Frame(0x06108402, b"\x01")
READ_UID = Frame(0x0C108000, b"\x03")
READ_BOOT = Frame(0x0C108011, b"\x01")


class Rig:
    """An output node at 0x042 behind the overlay."""

    def __init__(self) -> None:
        self.bus = QueueTransport()
        self.faulty = FaultyTransport(self.bus)
        self.platform = MemoryPlatform(stored={"address": b"\x42\x00"})
        endpoint = profiles.output_endpoint()
        self.node = Node(self.faulty, self.platform, profiles.identity(0x20, UID), [endpoint])
        self.faulty.attach(self.node.now, lambda: self.node.address)
        self.node.start()
        output = Output(self.node, endpoint)
        endpoint.on_command = lambda _ep, element, _args: output.command(element)
        self.output = output
        self.bus.take()  # Booted

    @property
    def faults(self) -> Faults:
        return self.faulty.faults

    def ask(self, frame: Frame, at: int = 0) -> list[str]:
        self.platform.clock = at
        self.bus.inbox.append(frame)
        self.node.tick()
        return [f"{f.can_id:08X}#{f.data.hex().upper()}" for f in self.bus.take()]


@pytest.fixture
def rig() -> Rig:
    return Rig()


def test_no_faults_is_transparent(rig: Rig) -> None:
    assert rig.ask(READ_BOOT) == ["0E108011#010100"]


def test_a_dropped_response_still_carries_the_command_out(rig: Rig) -> None:
    rig.faults.drop_responses = 1
    assert rig.ask(TOGGLE) == ["10108400#01"]  # the report goes; the reply does not
    assert rig.output.is_on()
    assert rig.faults.drop_responses == 0
    assert rig.ask(TOGGLE, at=500) == ["10108400#00", "0E108402#01"]


def test_busy_refuses_without_reaching_the_node(rig: Rig) -> None:
    rig.faults.busy_responses = 1
    assert rig.ask(TOGGLE) == ["0E108402#41"]  # tid 1, BUSY
    assert not rig.output.is_on()
    assert rig.ask(TOGGLE, at=10) == ["10108400#01", "0E108402#01"]


def test_busy_leaves_other_nodes_requests_alone(rig: Rig) -> None:
    rig.faults.busy_responses = 1
    assert rig.ask(Frame(0x0610C402, b"\x01")) == []  # for 0x043
    assert rig.faults.busy_responses == 1


def test_silence_swallows_everything_until_it_ends(rig: Rig) -> None:
    rig.faults.silent_until = 1000
    assert rig.ask(READ_BOOT, at=999) == []
    assert rig.node.sent > 0  # the node believes it sent
    assert rig.ask(READ_BOOT, at=1000) == ["0E108011#010100"]


def test_a_dropped_segment_leaves_a_hole(rig: Rig) -> None:
    rig.faults.drop_segment = 0
    assert rig.ask(READ_UID) == ["0E108000#5B", "12108000#830100010203"]
    assert rig.faults == Faults()  # used up
    assert len(rig.ask(READ_UID, at=10)) == 3
