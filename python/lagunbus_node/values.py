"""Scalar value codec — lagunbus-spec/02-typy-danych.md.

The Python twin of `c/lagunbus/include/lagunbus/values.h`. Everything is little-endian
and there are no floating-point numbers.

A value is a plain `int`, and **`None` is "absent"** — the "I don't know" that the spec
keeps apart from zero (02-typy-danych.md §1.3). In C that is `lb_value_t.absent`; here
the type system carries it, so a forgotten check shows up in mypy, not on the bus.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Final

Value = int | None
"""A scalar value; `None` is the "absent" pattern."""

ABSENT: Final[Value] = None


@dataclass(frozen=True)
class _Spec:
    size: int
    signed: bool
    has_absent: bool
    absent: int


class ValueType(Enum):
    """Scalar types from 02-typy-danych.md §1, named as the spec spells them."""

    BOOL = "bool"
    U8 = "u8"
    U16 = "u16"
    U32 = "u32"
    I8 = "i8"
    I16 = "i16"
    I32 = "i32"
    ENUM8 = "enum8"
    ENUM16 = "enum16"
    MAP8 = "map8"
    MAP16 = "map16"
    MAP32 = "map32"

    @property
    def spec(self) -> _Spec:
        return _SPECS[self]


# Table from 02-typy-danych.md §1 — the same one as `SPECS` in values.c.
_SPECS: Final[dict[ValueType, _Spec]] = {
    ValueType.BOOL: _Spec(1, False, True, 0xFF),
    ValueType.U8: _Spec(1, False, True, 0xFF),
    ValueType.U16: _Spec(2, False, True, 0xFFFF),
    ValueType.U32: _Spec(4, False, True, 0xFFFFFFFF),
    ValueType.I8: _Spec(1, True, True, 0x80),
    ValueType.I16: _Spec(2, True, True, 0x8000),
    ValueType.I32: _Spec(4, True, True, 0x80000000),
    ValueType.ENUM8: _Spec(1, False, True, 0xFF),
    ValueType.ENUM16: _Spec(2, False, True, 0xFFFF),
    ValueType.MAP8: _Spec(1, False, False, 0),
    ValueType.MAP16: _Spec(2, False, False, 0),
    ValueType.MAP32: _Spec(4, False, False, 0),
}

INT32_MAX: Final = 0x7FFFFFFF


def size_of(t: ValueType) -> int:
    return t.spec.size


def has_absent(t: ValueType) -> bool:
    """Bit fields (`map*`) have no "absent": any set of bits is a valid state."""
    return t.spec.has_absent


def lo(t: ValueType) -> int:
    """Smallest value. For signed types one above the range — the extreme is "absent"."""
    spec = t.spec
    if not spec.signed:
        return 0
    return -(1 << (spec.size * 8 - 1)) + 1


def hi(t: ValueType) -> int:
    """Largest value.

    `u32` is capped at `INT32_MAX`, **exactly as the C library does**: there a value
    lives in an `int32_t`, and the upper half of `u32` cannot be carried. The spec
    allows up to `0xFFFFFFFE`. The two libraries are to behave identically, so the cap
    is copied here rather than fixed on one side only — see zadania/FEAT-03.md.
    """
    spec = t.spec
    if spec.signed:
        return (1 << (spec.size * 8 - 1)) - 1
    if spec.size == 4:
        return INT32_MAX
    full = (1 << (spec.size * 8)) - 1
    return full - 1 if spec.has_absent else full


def in_range(t: ValueType, value: Value) -> bool:
    """Whether a value fits the type. "Absent" fits wherever it exists."""
    if value is None:
        return t.spec.has_absent
    if t is ValueType.BOOL:
        return value in (0, 1)
    return lo(t) <= value <= hi(t)


def encode(t: ValueType, value: Value) -> bytes:
    """A value as bytes. `ValueError` for "absent" in a type that has none.

    The range is **not** checked, as in `lb_encode`: the bits are what they are. What a
    node refuses on a write is `in_range`'s business.
    """
    spec = t.spec
    if value is None:
        if not spec.has_absent:
            raise ValueError(f"{t.name} has no absent value")
        raw = spec.absent
    else:
        raw = value & ((1 << (spec.size * 8)) - 1)
    return raw.to_bytes(spec.size, "little")


def decode(t: ValueType, data: bytes) -> Value:
    """Read a value back. `ValueError` when the length does not match the type."""
    spec = t.spec
    if len(data) != spec.size:
        raise ValueError(f"{t.name} takes {spec.size} bytes, got {len(data)}")
    raw = int.from_bytes(data, "little")
    if spec.has_absent and raw == spec.absent:
        return None
    if spec.signed and raw & (1 << (spec.size * 8 - 1)):
        raw -= 1 << (spec.size * 8)
    return raw


# --- values longer than a frame, 02-typy-danych.md §2.2 ------------------------------

STRING_PREFIX: Final = 1
BLOB_PREFIX: Final = 2


def encode_blob(data: bytes) -> bytes:
    """A blob with a two-byte length prefix."""
    if len(data) > 0xFFFF:
        raise ValueError("a blob holds at most 65535 bytes")
    return len(data).to_bytes(2, "little") + data


def decode_blob(data: bytes) -> bytes:
    """The data inside a blob. `ValueError` when the prefix lies — refuse, not guess."""
    if len(data) < BLOB_PREFIX:
        raise ValueError("too short for a blob prefix")
    announced = int.from_bytes(data[:2], "little")
    if announced + BLOB_PREFIX != len(data):
        raise ValueError(f"prefix says {announced} bytes, got {len(data) - BLOB_PREFIX}")
    return data[BLOB_PREFIX:]
