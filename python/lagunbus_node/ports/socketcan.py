"""A transport over a raw SocketCAN socket — Linux only, but it imports everywhere.

The constants `AF_CAN` / `CAN_RAW` exist only on Linux; fetching them through `getattr`
lets the module import (and type-check) on macOS, and their absence surfaces only when
a socket is actually opened, with a readable message.
"""

from __future__ import annotations

import errno
import socket
import struct
from typing import Final

from lagunbus_node.frame import Frame

CAN_EFF_FLAG: Final = 0x8000_0000
"""Extended identifier. LagunBus uses nothing else."""

CAN_FRAME: Final = struct.Struct("<IB3x8s")
"""`struct can_frame` as the kernel lays it out: id, length, 3 bytes of padding, data."""

_AF_CAN: int = getattr(socket, "AF_CAN", -1)
_CAN_RAW: int = getattr(socket, "CAN_RAW", -1)

HAS_SOCKETCAN: Final = _AF_CAN != -1 and _CAN_RAW != -1


def pack(frame: Frame) -> bytes:
    return CAN_FRAME.pack(
        frame.can_id | CAN_EFF_FLAG, len(frame.data), frame.data.ljust(8, b"\x00")
    )


def unpack(raw: bytes, offset: int = 0) -> Frame | None:
    """The frame, or `None` for a length past 8 — not a frame we could have sent."""
    can_id, length, payload = CAN_FRAME.unpack_from(raw, offset)
    if length > 8:
        return None
    return Frame(can_id & ~CAN_EFF_FLAG, payload[:length])


class SocketCanTransport:
    """One CAN interface. Satisfies `interfaces.Transport`."""

    def __init__(self, channel: str) -> None:
        if not HAS_SOCKETCAN:
            raise RuntimeError("SocketCAN is available on Linux only; on this machine use udp")
        self.channel = channel
        self._sock = socket.socket(_AF_CAN, socket.SOCK_RAW, _CAN_RAW)
        try:
            self._sock.bind((channel,))
            self._sock.setblocking(False)
        except OSError:
            self._sock.close()
            raise

    def send(self, frame: Frame) -> bool:
        try:
            self._sock.send(pack(frame))
        except OSError as exc:
            if exc.errno in (errno.ENOBUFS, errno.EAGAIN):
                return False  # queue full: the node drops and counts, it does not wait
            raise
        return True

    def recv(self) -> Frame | None:
        while True:
            try:
                raw = self._sock.recv(CAN_FRAME.size)
            except BlockingIOError:
                return None
            if len(raw) == CAN_FRAME.size and (frame := unpack(raw)) is not None:
                return frame

    def health(self) -> tuple[int, int, int] | None:
        return None  # the error counters are not on a raw socket; `UNAVAILABLE` is honest

    def close(self) -> None:
        self._sock.close()
