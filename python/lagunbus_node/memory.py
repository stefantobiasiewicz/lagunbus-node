"""A bus and a platform in memory — for tests, and for a simulator.

The Python counterpart of `c/tests/mock.h`, but part of the library rather than of the
tests: a simulator (zadania/FEAT-02.md) needs exactly this — a clock it sets itself, a
store it can save between runs, and a bus it wires nodes together with.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

from lagunbus_node.frame import Frame


@dataclass
class QueueTransport:
    """Frames in through `inbox`, out into `outbox`.

    `capacity` is the transmit queue: past it `send` answers "full", which is how a
    test reaches the node's drop-and-count path.
    """

    inbox: deque[Frame] = field(default_factory=deque)
    outbox: list[Frame] = field(default_factory=list)
    capacity: int = 256
    health_state: tuple[int, int, int] | None = None

    def send(self, frame: Frame) -> bool:
        if len(self.outbox) >= self.capacity:
            return False
        self.outbox.append(frame)
        return True

    def recv(self) -> Frame | None:
        return self.inbox.popleft() if self.inbox else None

    def health(self) -> tuple[int, int, int] | None:
        return self.health_state

    def take(self) -> list[Frame]:
        """Everything sent since the last call."""
        out, self.outbox = self.outbox, []
        return out


@dataclass
class MemoryPlatform:
    """A clock set by hand, a dict for a store, a fixed "random" number."""

    clock: int = 0
    jitter: int = 0
    stored: dict[str, bytes] = field(default_factory=dict)

    def millis(self) -> int:
        return self.clock & 0xFFFFFFFF

    def store(self, key: str, data: bytes) -> bool:
        self.stored[key] = bytes(data)
        return True

    def load(self, key: str, length: int) -> bytes | None:
        data = self.stored.get(key)
        return data if data is not None and len(data) == length else None

    def random(self) -> int:
        return self.jitter

    def restart(self) -> bool:
        return False
