"""The bus — the Python twin of `transport.h`.

What the fault overlay wraps and what the ports (`ports.udp`, `ports.socketcan`)
provide. The node itself is the C library (zadania/zrobione/FEAT-02.md, N11) and takes its
frames from whoever pumps it — a board's port, or a simulator moving frames between a
`Transport` and `lagunbus_node.sim`. `platform.h` has no twin here any more: the C
node's platform in a simulator is `c/sim/sim.c`.
"""

from __future__ import annotations

from typing import Protocol

from lagunbus_node.frame import Frame


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
