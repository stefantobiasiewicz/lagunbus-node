"""The bus and the platform — everything the library needs from the world.

The Python twins of `transport.h` and `platform.h`, and the **same two interfaces and
nothing more**. A simulator, a test and a SocketCAN node differ only in what they plug
in here; the node itself cannot tell them apart.
"""

from __future__ import annotations

from typing import Final, Protocol

from lagunbus_node.frame import Frame

# Bus state as the controller sees it (`Diagnostics.BusState`).
BUS_ACTIVE: Final = 0x00
BUS_WARNING: Final = 0x01
BUS_PASSIVE: Final = 0x02
BUS_OFF: Final = 0x03


class Transport(Protocol):
    def send(self, frame: Frame) -> bool:
        """Send a frame. `False` means "queue full" — **not an error**: the node drops
        the frame and counts it rather than wait (04-interakcje.md §2.1)."""
        ...

    def recv(self) -> Frame | None:
        """One waiting frame, or `None`. **Never blocks** — the loop has to get back to
        `tick()` even when the bus is silent, or deadlines stand still."""
        ...

    def health(self) -> tuple[int, int, int] | None:
        """`(tec, rec, state)` for `Diagnostics`, or `None` when the port cannot know.
        The node then answers `UNAVAILABLE` instead of an invented number."""
        ...


class Platform(Protocol):
    def millis(self) -> int:
        """Milliseconds since start, **u32** and monotonic. The node does its time
        arithmetic modulo 2**32, exactly as the C library, so a port may wrap."""
        ...

    def store(self, key: str, data: bytes) -> bool:
        """Store persistently. `False` means it did not work — the node survives that."""
        ...

    def load(self, key: str, length: int) -> bytes | None:
        """Persisted data of exactly `length` bytes, or `None` — the normal state on a
        first boot, not a fault."""
        ...

    def random(self) -> int:
        """A random u32. Needed for one reason: **the delay before announcing must be
        random**, or forty nodes back from a power cut announce in the same
        millisecond (03-adresacja.md §5)."""
        ...

    def restart(self) -> bool:
        """Restart the node. `False` means "cannot" — then `FactoryReset` does what it
        can without one (types/0x0001-device.md §3.3). In C this is a NULL pointer."""
        ...
