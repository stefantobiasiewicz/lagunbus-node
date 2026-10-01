"""Injecting faults — as an overlay on the transport, not a feature of the node.

A node that always behaves will not exercise half of a hub: transaction retries,
noticing silence, discarding a broken segment run. `hub/tests/node/faults.py` gives its
node double exactly these four faults, and a simulator needs the same ones
(zadania/zrobione/FEAT-02.md).

They live **between the node and the bus**, not inside the node, and that is the whole
design (zadania/zrobione/FEAT-03.md):

- every one of the four can be told from the frames alone — a RESPONSE, a segment
  number, a moment in time, a request addressed to this node;
- so the node needs no hook, no flag and no test-only branch; the code under test is
  the code that ships;
- and the overlay wraps anything that is a `Transport`.

What the overlay cannot do is make the node *itself* busy — `BUSY` here is answered on
the node's behalf and the request never reaches it, which is what a hub sees either way.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Final

from lagunbus_node.frame import (
    ADDRESS_UNASSIGNED,
    Frame,
    decode_header,
    decode_id,
    encode_header,
    encode_id,
    is_join,
)
from lagunbus_node.interfaces import Transport
from lagunbus_node.registry import MessageKind, ResultCode

_REQUESTS: Final = frozenset({MessageKind.READ, MessageKind.WRITE, MessageKind.COMMAND})


@dataclass
class Faults:
    """What is to go wrong. The counters go down with every use."""

    drop_responses: int = 0
    """This many of the next RESPONSE frames are lost **after the request was carried
    out** — the hard case: the command happened and the hub does not know it."""

    busy_responses: int = 0
    """This many of the next requests to this node are answered `BUSY` and never reach
    it."""

    silent_until: int = 0
    """Until this moment (node clock, ms) nothing the node sends reaches the bus."""

    drop_segment: int | None = None
    """The number of the segment to lose, once. A hole in the numbering makes the hub
    discard the value as a whole (05-mapowanie-can.md §6.6)."""


class FaultyTransport:
    """A `Transport` that loses and refuses frames as `faults` says.

    Lost frames count as SENT from the node's side: `send` answers `True`, because on a
    real bus the node's controller did put them out — they went missing afterwards.
    """

    def __init__(self, inner: Transport, faults: Faults | None = None) -> None:
        self.inner = inner
        self.faults = faults if faults is not None else Faults()
        self._clock: Callable[[], int] = lambda: 0
        self._address: Callable[[], int] = lambda: ADDRESS_UNASSIGNED

    def attach(self, clock: Callable[[], int], address: Callable[[], int]) -> None:
        """Tell the overlay the node's time and address — for a `sim.SimDevice`,
        `attach(lambda: now_ms, lambda: device.address)`. Until then silence is
        measured from 0 and no request is recognised as the node's."""
        self._clock = clock
        self._address = address

    def send(self, frame: Frame) -> bool:
        f = self.faults
        if self._clock() < f.silent_until:
            return True
        if not is_join(frame.can_id):
            kind = decode_id(frame.can_id).kind
            if kind == MessageKind.RESPONSE and f.drop_responses > 0:
                f.drop_responses -= 1
                return True
            if (
                kind == MessageKind.LONG_RESPONSE
                and f.drop_segment is not None
                and len(frame.data) >= 2
                and frame.data[1] == f.drop_segment
            ):
                f.drop_segment = None
                return True
        return self.inner.send(frame)

    def recv(self) -> Frame | None:
        while True:
            frame = self.inner.recv()
            if frame is None or not self._take_busy(frame):
                return frame

    def health(self) -> tuple[int, int, int] | None:
        return self.inner.health()

    def _take_busy(self, frame: Frame) -> bool:
        """Answer `BUSY` in the node's name when this request is one to refuse."""
        if self.faults.busy_responses <= 0 or is_join(frame.can_id) or not frame.data:
            return False
        fields = decode_id(frame.can_id)
        address = self._address()
        if fields.kind not in _REQUESTS or address == ADDRESS_UNASSIGNED:
            return False
        if fields.address != address:
            return False
        self.faults.busy_responses -= 1
        tid, _ = decode_header(frame.data[0])
        reply = encode_id(MessageKind.RESPONSE, address, fields.endpoint, fields.element)
        # Through `self.send`, so a silence in force swallows the refusal too.
        self.send(Frame(reply, bytes([encode_header(tid, ResultCode.BUSY)])))
        return True
