"""A transport over UDP multicast — the node's side of `lagun-hub run --udp`.

**Test and development only**: the hub and simulated nodes as separate processes on one
machine without CAN (zadania/zrobione/FEAT-02.md, N1). The hub has its own implementation,
sharing no code with this one; the format is written down once, in README.md
§ Decisions, and both sides pin it with the same byte vector.

    offset  size
    0       4     magic  b"LBU1"   — LagunBus over UDP, version 1
    4       4     sender           — random u32 per open transport, little-endian
    8       16    struct can_frame — the SocketCAN layout (`socketcan.CAN_FRAME`)

Every process in the group hears every other one — a cable, with no broker. A process
also hears ITSELF; `sender` is how it drops its own frames, as a SocketCAN socket never
gets its own back. There is no arbitration between processes: frames arrive in the
order they were sent.
"""

from __future__ import annotations

import errno
import secrets
import socket
import struct
from typing import Final

from lagunbus_node.frame import Frame
from lagunbus_node.ports.socketcan import CAN_FRAME, pack, unpack

DEFAULT_GROUP: Final = "239.255.76.66"
DEFAULT_PORT: Final = 7607
LOOPBACK: Final = "127.0.0.1"

MAGIC: Final = b"LBU1"
HEADER: Final = struct.Struct("<4sI")
DATAGRAM_SIZE: Final = HEADER.size + CAN_FRAME.size  # 24


def encode(sender: int, frame: Frame) -> bytes:
    return HEADER.pack(MAGIC, sender) + pack(frame)


def decode(datagram: bytes) -> tuple[int, Frame] | None:
    """`(sender, frame)`, or `None` for anything that is not our datagram — any program
    on the machine may write to the group."""
    if len(datagram) != DATAGRAM_SIZE:
        return None
    magic, sender = HEADER.unpack_from(datagram)
    if magic != MAGIC:
        return None
    frame = unpack(datagram, HEADER.size)
    return None if frame is None else (sender, frame)


class UdpTransport:
    """One port on the group. Satisfies `interfaces.Transport`.

    Two sockets: macOS refuses to send from a socket bound to a group address, and
    binding to the group keeps other traffic on the port out. Both are pinned to the
    loopback interface — the frames stay on this machine.
    """

    def __init__(self, group: str = DEFAULT_GROUP, port: int = DEFAULT_PORT) -> None:
        self.address = (group, port)
        self.sender = secrets.randbits(32)
        loopback = socket.inet_aton(LOOPBACK)
        self._rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self._tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        try:
            self._rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):  # macOS needs it for a second listener
                self._rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            self._rx.bind((group, port))
            self._rx.setsockopt(
                socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton(group) + loopback
            )
            self._rx.setblocking(False)
            self._tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, loopback)
            self._tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
            self._tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
            self._tx.setblocking(False)
        except OSError:
            self.close()
            raise

    def send(self, frame: Frame) -> bool:
        try:
            self._tx.sendto(encode(self.sender, frame), self.address)
        except OSError as exc:
            if exc.errno in (errno.ENOBUFS, errno.EAGAIN):
                return False  # queue full: the node drops and counts, it does not wait
            raise
        return True

    def recv(self) -> Frame | None:
        while True:
            try:
                datagram = self._rx.recv(64)
            except BlockingIOError:
                return None
            decoded = decode(datagram)
            if decoded is not None and decoded[0] != self.sender:
                return decoded[1]

    def health(self) -> tuple[int, int, int] | None:
        return None  # there is no controller to ask; `UNAVAILABLE` is honest

    def close(self) -> None:
        self._rx.close()
        self._tx.close()
