"""The LagunBus frame and the codec for the 29-bit identifier.

The Python twin of `c/lagunbus/include/lagunbus/frame.h`. The field layout and every
constant come from lagunbus-spec/05-mapowanie-can.md. Nothing here knows about a
socket — this is a pure codec.

It is deliberately **not** shared with the hub (`lagunhub.protocol.frame`): the node
library has no line in common with the hub (zadania/zrobione/FEAT-03.md, N2 and N7). Agreement
between the two is what the codec vectors in `vectors/` are for.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Final

# --- identifier layout, 05-mapowanie-can.md §1 -----------------------------------
#
#   28 27 26 25 │ 24 … 14 │ 13 … 8 │ 7 … 0
#      KIND     │ ADDRESS │   EP   │ ELEM
KIND_SHIFT: Final = 25
KIND_MASK: Final = 0x0F
ADDRESS_SHIFT: Final = 14
ADDRESS_MASK: Final = 0x7FF
ENDPOINT_SHIFT: Final = 8
ENDPOINT_MASK: Final = 0x3F
ELEMENT_MASK: Final = 0xFF

CAN_ID_MASK: Final = 0x1FFFFFFF
MAX_PAYLOAD: Final = 8

# --- addresses, 03-adresacja.md §2 -------------------------------------------------
ADDRESS_UNASSIGNED: Final = 0x000
ADDRESS_NODE_MIN: Final = 0x001
ADDRESS_NODE_MAX: Final = 0x7FD
ADDRESS_HUB: Final = 0x7FE
ADDRESS_BROADCAST: Final = 0x7FF
ENDPOINT_MAX: Final = 63

# --- the join frame has a layout of its own, 05-mapowanie-can.md §4 ----------------
#
#   28 27 26 25 │ 24 │ 23 … 0
#      0xF      │ K  │ ShortId[23:0]
JOIN_KIND: Final = 0x0F
JOIN_DIRECTION_SHIFT: Final = 24
JOIN_SHORT_ID_MASK: Final = 0x00FFFFFF

# --- header byte of transactional messages, 05-mapowanie-can.md §5.1 ---------------
#
#   bit 7 6 5 4 3 │ 2 1 0
#       RESULT    │  Tid
TID_MASK: Final = 0x07
RESULT_SHIFT: Final = 3
RESULT_MASK: Final = 0x1F


@dataclass(frozen=True)
class Frame:
    """A frame exactly as it goes over the wire.

    Checked on construction, unlike `lb_frame_t`: in C the struct cannot refuse to
    exist, here it can — and a frame of nine bytes caught at its birth is cheaper than
    one caught by a transport that silently truncates it.
    """

    can_id: int
    data: bytes = b""

    def __post_init__(self) -> None:
        if not 0 <= self.can_id <= CAN_ID_MASK:
            raise ValueError(f"identifier 0x{self.can_id:X} does not fit in 29 bits")
        if len(self.data) > MAX_PAYLOAD:
            raise ValueError(f"payload of {len(self.data)} bytes; a frame carries {MAX_PAYLOAD}")

    def __repr__(self) -> str:
        return f"Frame(0x{self.can_id:08X}, {self.data.hex().upper() or '-'})"


@dataclass(frozen=True)
class Fields:
    """The identifier fields once taken apart."""

    kind: int
    address: int
    endpoint: int
    element: int


def encode_id(kind: int, address: int, endpoint: int, element: int) -> int:
    """Build an identifier. `ValueError` when any field does not fit into its own.

    Every field is checked instead of truncated. A truncated address would land on
    somebody else's node and switch on the wrong light — and the bug would only become
    visible at that point.
    """
    if not (
        0 <= kind <= KIND_MASK
        and 0 <= address <= ADDRESS_MASK
        and 0 <= endpoint <= ENDPOINT_MASK
        and 0 <= element <= ELEMENT_MASK
    ):
        raise ValueError(
            f"field out of range: kind={kind} address=0x{address:X} "
            f"endpoint={endpoint} element=0x{element:X}"
        )
    return (
        (kind << KIND_SHIFT) | (address << ADDRESS_SHIFT) | (endpoint << ENDPOINT_SHIFT) | element
    )


def decode_id(can_id: int) -> Fields:
    """Take an identifier apart. Checks nothing — any 29 bits can be taken apart."""
    return Fields(
        kind=(can_id >> KIND_SHIFT) & KIND_MASK,
        address=(can_id >> ADDRESS_SHIFT) & ADDRESS_MASK,
        endpoint=(can_id >> ENDPOINT_SHIFT) & ENDPOINT_MASK,
        element=can_id & ELEMENT_MASK,
    )


def is_join(can_id: int) -> bool:
    """Whether this is a join frame — asked BEFORE taking the identifier apart."""
    return ((can_id >> KIND_SHIFT) & KIND_MASK) == JOIN_KIND


def join_id(short_id: int, announce: bool) -> int:
    """Identifier of a join frame: an ANNOUNCEMENT (K=1) or an ASSIGNMENT (K=0).

    An assignment has the lower identifier, so in arbitration it goes ahead of further
    announcements (05-mapowanie-can.md §4.4).
    """
    return (
        (JOIN_KIND << KIND_SHIFT)
        | ((1 if announce else 0) << JOIN_DIRECTION_SHIFT)
        | (short_id & JOIN_SHORT_ID_MASK)
    )


def join_is_announce(can_id: int) -> bool:
    return (can_id >> JOIN_DIRECTION_SHIFT) & 1 == 1


def join_short_id(can_id: int) -> int:
    """`ShortId` out of a join frame. Meaningless for a frame that is not one."""
    return can_id & JOIN_SHORT_ID_MASK


def encode_header(tid: int, result: int) -> int:
    """The header byte: result and `Tid` in one."""
    return ((result & RESULT_MASK) << RESULT_SHIFT) | (tid & TID_MASK)


def decode_header(header: int) -> tuple[int, int]:
    """`(tid, result)` out of the header byte."""
    return header & TID_MASK, (header >> RESULT_SHIFT) & RESULT_MASK
