"""Endpoint — the smallest thing the hub addresses (01-model-danych.md §1).

The Python twin of `c/lagunbus/include/lagunbus/endpoint.h`. The attributes are a list
the application owns, exactly as the array is in C; the node only reads and writes it.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Final

from lagunbus_node import values
from lagunbus_node.registry import CommonAttribute, ResultCode
from lagunbus_node.values import Value, ValueType

# Attribute access (01-model-danych.md §3.1). `PERSIST` says the value survives a restart.
RO: Final = 0x01
WO: Final = 0x02
RW: Final = RO | WO
PERSIST: Final = 0x04

COMMON_BASE: Final = 0xF0
"""First attribute number reserved for the common ones (01-model-danych.md §4)."""


@dataclass
class Attr:
    id: int
    type: ValueType
    access: int
    value: Value


CommandFn = Callable[["Endpoint", int, bytes], int]
"""Command handler: `(endpoint, element, args) -> result code`."""

WriteFn = Callable[["Endpoint", int, Value], int]
"""Reaction to a write, AFTER the value has already landed in the list.

Separate from the write itself, because writing is common to all types while the
reaction belongs to the endpoint: `OnOff` has to flip the relay, `DebounceTime` has to
do nothing at all.
"""

CheckFn = Callable[["Endpoint", int, Value], int]
"""A check BEFORE a write lands; anything but OK refuses it whole — see `lb_check_fn`
in endpoint.h."""

FeatureFn = Callable[[int], int]
"""The features an element of the type needs, 0 when the type does not know it — see
`lb_feature_fn` in endpoint.h."""


@dataclass
class Endpoint:
    number: int
    type_id: int
    revision: int
    features: int
    channel: int
    attrs: list[Attr] = field(default_factory=list)

    event_seq: int = 0
    """Event counter, **separate for every endpoint** (01-model-danych.md §3.3), u8."""

    on_command: CommandFn | None = None
    on_write: WriteFn | None = None
    on_check: CheckFn | None = None
    feature_of: FeatureFn | None = None

    def attr(self, element: int) -> Attr | None:
        for a in self.attrs:
            if a.id == element:
                return a
        return None

    def _common(self, element: int) -> tuple[ValueType, int] | None:
        """The common attributes come from fields, not from the list — keeping them in
        both would be two descriptions of the same thing, free to drift apart."""
        if element == CommonAttribute.TypeId:
            return ValueType.U16, self.type_id
        if element == CommonAttribute.TypeRevision:
            return ValueType.U8, self.revision
        if element == CommonAttribute.Features:
            return ValueType.MAP16, self.features
        if element == CommonAttribute.Channel:
            return ValueType.U8, self.channel
        return None

    def _feature_gate(self, element: int) -> int:
        """`UNSUPPORTED_FEATURE` for an element of the type outside the declared
        features — decided by the declaration, not by the list (endpoint.c)."""
        if self.feature_of is None:
            return ResultCode.OK
        needs = self.feature_of(element)
        if needs and not self.features & needs:
            return ResultCode.UNSUPPORTED_FEATURE
        return ResultCode.OK

    def read(self, element: int) -> tuple[int, bytes]:
        """`(result code, value bytes)`."""
        common = self._common(element)
        if common is not None:
            return ResultCode.OK, values.encode(*common)
        gate = self._feature_gate(element)
        if gate != ResultCode.OK:
            return gate, b""
        a = self.attr(element)
        if a is None:
            return ResultCode.UNSUPPORTED_ELEMENT, b""
        if not a.access & RO:
            # The attribute exists but is write-only — a different thing from "absent".
            return ResultCode.WRITE_ONLY, b""
        try:
            return ResultCode.OK, values.encode(a.type, a.value)
        except ValueError:
            return ResultCode.FAILURE, b""

    def write(self, element: int, data: bytes) -> int:
        """Write an attribute. Checks access, type and range — in that order."""
        if self._common(element) is not None:
            return ResultCode.READ_ONLY  # TypeId and friends describe the device
        gate = self._feature_gate(element)
        if gate != ResultCode.OK:
            return gate
        a = self.attr(element)
        if a is None:
            return ResultCode.UNSUPPORTED_ELEMENT
        if not a.access & WO:
            return ResultCode.READ_ONLY
        try:
            value = values.decode(a.type, data)
        except ValueError:
            # A wrong length is a wrong value — the node does not guess what was meant.
            return ResultCode.INVALID_VALUE
        if not values.in_range(a.type, value):
            return ResultCode.INVALID_VALUE
        if self.on_check is not None:
            verdict = self.on_check(self, element, value)
            if verdict != ResultCode.OK:
                return verdict  # refused whole — the old value stays
        a.value = value
        if self.on_write is not None:
            return self.on_write(self, element, value)
        return ResultCode.OK

    def setting(self, element: int, fallback: int) -> int:
        """A setting from an attribute, or `fallback` when it is missing or absent.

        `setting()` in key.c and output.c — an optional attribute that is not there must
        not upset the logic, only mean "use the default".
        """
        a = self.attr(element)
        return fallback if a is None or a.value is None else a.value
