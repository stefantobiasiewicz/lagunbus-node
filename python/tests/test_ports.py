"""The transports for a node on a computer — `ports.udp` and the frame format of
`ports.socketcan`. Every UDP test takes a port of its own, so that a simulator running
on the default port neither hears it nor disturbs it."""

from __future__ import annotations

import random
import socket
import time
from collections.abc import Iterator

import pytest

from lagunbus_node.frame import Frame
from lagunbus_node.interfaces import Transport
from lagunbus_node.ports import socketcan
from lagunbus_node.ports.udp import (
    DATAGRAM_SIZE,
    DEFAULT_GROUP,
    LOOPBACK,
    UdpTransport,
    decode,
    encode,
)
from lagunbus_node.sim import SimDevice

SETTLE = 0.02
"""Loopback delivery is fast but not instant — the frame goes through the kernel."""


@pytest.fixture
def port() -> int:
    return random.randint(20_000, 60_000)


@pytest.fixture
def opened() -> Iterator[list[UdpTransport]]:
    transports: list[UdpTransport] = []
    yield transports
    for t in transports:
        t.close()


def receive(t: UdpTransport) -> list[Frame]:
    time.sleep(SETTLE)
    return list(iter(t.recv, None))


def test_the_datagram_is_pinned_byte_for_byte() -> None:
    """The hub (`lagunhub.protocol.bus.udp`) is written separately and shares no code
    with this library — these bytes are the contract between them. The same vector
    stands in `hub/tests/test_udp_bus.py`; README.md § Decisions describes it."""
    datagram = encode(0x11223344, Frame(0x06108402, b"\x01"))
    assert datagram == bytes.fromhex(
        "4C425531"  # magic "LBU1"
        "44332211"  # sender, little-endian
        "02841086"  # can_id | CAN_EFF_FLAG, little-endian
        "01000000"  # length, three bytes of padding
        "0100000000000000"  # data, padded to 8
    )
    assert len(datagram) == DATAGRAM_SIZE == 24
    assert decode(datagram) == (0x11223344, Frame(0x06108402, b"\x01"))


def test_what_is_not_our_datagram_decodes_to_nothing() -> None:
    good = encode(1, Frame(0x06108402, b"\x01"))
    assert decode(good[:-1]) is None, "too short"
    assert decode(b"XXXX" + good[4:]) is None, "a foreign magic"
    assert decode(good[:12] + b"\x09" + good[13:]) is None, "a length past 8"


def test_the_socketcan_layout_round_trips() -> None:
    f = Frame(0x06108402, b"\x01\x02")
    raw = socketcan.pack(f)
    assert len(raw) == 16
    assert socketcan.unpack(raw) == f


def test_socketcan_imports_everywhere_and_refuses_clearly_without_af_can() -> None:
    if socketcan.HAS_SOCKETCAN:
        pytest.skip("this machine has SocketCAN")
    with pytest.raises(RuntimeError, match="Linux only"):
        socketcan.SocketCanTransport("vcan0")


def test_it_is_a_transport(port: int, opened: list[UdpTransport]) -> None:
    t = UdpTransport(DEFAULT_GROUP, port)
    opened.append(t)
    transport: Transport = t
    assert transport.health() is None


def test_the_others_hear_a_frame_and_the_sender_does_not(
    port: int, opened: list[UdpTransport]
) -> None:
    a, b, c = (UdpTransport(DEFAULT_GROUP, port) for _ in range(3))
    opened.extend((a, b, c))
    f = Frame(0x02108001, b"\x00\x00\x00\x01\x00\x00")
    assert a.send(f)
    assert receive(b) == [f]
    assert receive(c) == [f]
    assert receive(a) == []


def test_garbage_in_the_group_is_dropped(port: int, opened: list[UdpTransport]) -> None:
    b = UdpTransport(DEFAULT_GROUP, port)
    opened.append(b)
    stranger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stranger.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(LOOPBACK))
    stranger.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
    good = Frame(0x06108402, b"\x01")
    try:
        stranger.sendto(b"hello", (DEFAULT_GROUP, port))
        stranger.sendto(encode(99, good), (DEFAULT_GROUP, port))
    finally:
        stranger.close()
    assert receive(b) == [good]


def test_a_node_answers_a_read_over_udp(port: int, opened: list[UdpTransport]) -> None:
    """The whole path: the C node on one socket, a "hub" on another — pumped as the
    simulator pumps it."""
    node_side, hub_side = UdpTransport(DEFAULT_GROUP, port), UdpTransport(DEFAULT_GROUP, port)
    opened.extend((node_side, hub_side))
    device = SimDevice("output", bytes.fromhex("A1B2C3D400010203"))
    device.store_put("address", bytes.fromhex("4200"))
    device.start(0)
    for sent in device.take_tx():
        node_side.send(sent)
    assert receive(hub_side) == [Frame(0x02108001, bytes.fromhex("000000010000"))], "Booted"

    hub_side.send(Frame(0x0C108400, b"\x06"))  # READ OnOff of ep4, tid 6
    time.sleep(SETTLE)
    while (incoming := node_side.recv()) is not None:
        device.rx(incoming)
    device.loop(0)
    for sent in device.take_tx():
        node_side.send(sent)
    assert receive(hub_side) == [Frame(0x0E108400, b"\x06\x00")]
