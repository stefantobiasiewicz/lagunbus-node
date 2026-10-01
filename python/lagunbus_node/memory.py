"""A bus in memory — a `Transport` whose two ends a test holds in its hands.

Used to test what sits between a node and the bus (the fault overlay) without a socket.
Until zadania/zrobione/FEAT-02.md N11 this module also had an in-memory platform for the Python
node; the node is now the C library, whose port (clock, store) is `c/sim/sim.c`.
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
