"""The implementations behind the interfaces the conformance tests are written against.

**The frame codec** exists twice — `lagunbus_node.frame` in Python (the fault overlay,
the transports and the simulator's traffic log need it) and the C library — so the
identifier, header and join vectors run through both, and a difference is a failure of
one of them or of the spec's wording.

**Everything else** — values, blobs, ShortId and the node itself — exists only in C
since zadania/zrobione/FEAT-02.md N11: the node is in C, the tools around it in Python. Those
vectors and every scenario run through the C library alone, reached through
`lagunbus_node.sim` (`liblagunbus_sim` and `ctypes`).
"""

from __future__ import annotations

import ctypes
from enum import Enum
from typing import Protocol

from lagunbus_node import frame, sim
from lagunbus_node.frame import Frame

Value = int | None
"""A scalar value as the vectors write it; `None` is "absent" (02-typy-danych.md §1.3)."""


class ValueType(Enum):
    """Scalar types from 02-typy-danych.md §1, **in the order of `lb_type_t`** — the
    position is the number passed to C. `size` lets a test notice a reordering."""

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
    def size(self) -> int:
        return _SIZES[self]


_SIZES = {
    ValueType.BOOL: 1,
    ValueType.U8: 1,
    ValueType.U16: 2,
    ValueType.U32: 4,
    ValueType.I8: 1,
    ValueType.I16: 2,
    ValueType.I32: 4,
    ValueType.ENUM8: 1,
    ValueType.ENUM16: 2,
    ValueType.MAP8: 1,
    ValueType.MAP16: 2,
    ValueType.MAP32: 4,
}


# --- the interfaces ------------------------------------------------------------------


class FrameCodec(Protocol):
    """What both languages have."""

    def encode_id(self, kind: int, address: int, endpoint: int, element: int) -> int | None: ...
    def decode_id(self, can_id: int) -> tuple[int, int, int, int]: ...
    def join_id(self, short_id: int, announce: bool) -> int: ...
    def encode_header(self, tid: int, result: int) -> int: ...
    def decode_header(self, byte: int) -> tuple[int, int]: ...


class Device(Protocol):
    """A node with one of the catalogue's devices, a clock and a store the test
    controls. `pins` is the mask of input levels, bit i for input i."""

    def set_clock(self, ms: int) -> None: ...
    def store_put(self, key: str, data: bytes) -> None: ...
    def store_get(self, key: str) -> bytes | None: ...
    def start(self, pins: int) -> None: ...
    def rx(self, f: Frame) -> None: ...
    def loop(self, pins: int) -> None: ...
    def take_tx(self) -> list[Frame]: ...
    def allow_restart(self) -> None: ...
    def restart_requested(self) -> bool: ...
    def driven(self, index: int = 0) -> int | None:
        """The last level output `index` drove, or `None` before any."""
        ...

    def service_press(self) -> None: ...
    def state(self) -> str | None: ...
    def set_bus(self, state: str) -> None: ...
    def set_errors(self, tec: int, rec: int) -> None: ...
    def set_reset_reason(self, reason: str) -> None: ...
    def sense(self, endpoint: int, raw: int | None) -> None: ...


# --- Python -------------------------------------------------------------------------


class PyCodec:
    def encode_id(self, kind: int, address: int, endpoint: int, element: int) -> int | None:
        try:
            return frame.encode_id(kind, address, endpoint, element)
        except ValueError:
            return None

    def decode_id(self, can_id: int) -> tuple[int, int, int, int]:
        f = frame.decode_id(can_id)
        return f.kind, f.address, f.endpoint, f.element

    def join_id(self, short_id: int, announce: bool) -> int:
        return frame.join_id(short_id, announce)

    def encode_header(self, tid: int, result: int) -> int:
        return frame.encode_header(tid, result)

    def decode_header(self, byte: int) -> tuple[int, int]:
        return frame.decode_header(byte)


# --- C, through ctypes --------------------------------------------------------------


class _Value(ctypes.Structure):
    """`lb_value_t` — the only struct that crosses the boundary, and it has two fields."""

    _fields_ = [("value", ctypes.c_int32), ("absent", ctypes.c_bool)]


class _Fields(ctypes.Structure):
    """`lb_fields_t`, returned by value from `lb_decode_id`."""

    _fields_ = [
        ("kind", ctypes.c_uint8),
        ("address", ctypes.c_uint16),
        ("endpoint", ctypes.c_uint8),
        ("element", ctypes.c_uint8),
    ]


_C_TYPES = {t: i for i, t in enumerate(ValueType)}
"""`lb_type_t` numbers — the position in `ValueType`."""


def declare_codec(lib: ctypes.CDLL) -> ctypes.CDLL:
    """Declare the library's own codec functions — `lagunbus_node.sim` declares only
    its `lbs_…` API."""
    u8, u16, u32, b = ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_bool
    ptr, i32, size = ctypes.c_void_p, ctypes.c_int32, ctypes.c_size_t

    def sig(name: str, res: object, *args: object) -> None:
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = list(args)

    sig("lb_encode_id", b, u8, u16, u8, u8, ctypes.POINTER(u32))
    sig("lb_decode_id", _Fields, u32)
    sig("lb_join_id", u32, u32, b)
    sig("lb_encode_header", u8, u8, u8)
    sig("lb_decode_header", None, u8, ctypes.POINTER(u8), ctypes.POINTER(u8))
    sig("lb_size_of", u8, ctypes.c_int)
    sig("lb_lo", i32, ctypes.c_int)
    sig("lb_hi", i32, ctypes.c_int)
    sig("lb_in_range", b, ctypes.c_int, _Value)
    sig("lb_encode", u8, ctypes.c_int, _Value, ctypes.c_char_p, size)
    sig("lb_decode", b, ctypes.c_int, ctypes.c_char_p, size, ctypes.POINTER(_Value))
    sig("lb_encode_blob", size, ctypes.c_char_p, size, ctypes.c_char_p, size)
    sig("lb_decode_blob", b, ptr, size, ctypes.POINTER(ptr), ctypes.POINTER(size))
    sig("lb_string_whole", b, ctypes.c_char_p, size)
    sig("lb_quantity_type", ctypes.c_int, u8)

    return lib


def _c_value(value: Value) -> _Value:
    if value is None:
        return _Value(0, True)
    # `LB_NUM(x)` — an int32_t; u32 values above 2**31 become negative there.
    return _Value(ctypes.c_int32(value).value, False)


class CCodec:
    def __init__(self, lib: ctypes.CDLL) -> None:
        self.lib = lib

    def encode_id(self, kind: int, address: int, endpoint: int, element: int) -> int | None:
        # The C signature takes u8/u16 — a value too wide for the PARAMETER would be
        # truncated by ctypes before the library could refuse it. Those cases are the
        # C compiler's business, not the library's, and a test cannot reach them.
        out = ctypes.c_uint32(0xDEADBEEF)
        ok = self.lib.lb_encode_id(kind, address, endpoint, element, ctypes.byref(out))
        return int(out.value) if ok else None

    def decode_id(self, can_id: int) -> tuple[int, int, int, int]:
        f = self.lib.lb_decode_id(can_id)
        return int(f.kind), int(f.address), int(f.endpoint), int(f.element)

    def join_id(self, short_id: int, announce: bool) -> int:
        return int(self.lib.lb_join_id(short_id, announce))

    def encode_header(self, tid: int, result: int) -> int:
        return int(self.lib.lb_encode_header(tid, result))

    def decode_header(self, byte: int) -> tuple[int, int]:
        tid, result = ctypes.c_uint8(), ctypes.c_uint8()
        self.lib.lb_decode_header(byte, ctypes.byref(tid), ctypes.byref(result))
        return int(tid.value), int(result.value)

    def size_of(self, t: ValueType) -> int:
        return int(self.lib.lb_size_of(_C_TYPES[t]))

    def encode(self, t: ValueType, value: Value) -> bytes | None:
        buf = ctypes.create_string_buffer(8)
        n = self.lib.lb_encode(_C_TYPES[t], _c_value(value), buf, 8)
        return buf.raw[:n] if n else None

    def decode(self, t: ValueType, data: bytes) -> tuple[Value] | None:
        out = _Value()
        if not self.lib.lb_decode(_C_TYPES[t], data, len(data), ctypes.byref(out)):
            return None
        if out.absent:
            return (None,)
        raw = int(out.value)
        # Back from `int32_t` to what the type means: u32 is unsigned on the wire.
        return (raw & 0xFFFFFFFF if t in (ValueType.U32, ValueType.MAP32) else raw,)

    def lo(self, t: ValueType) -> int:
        return int(self.lib.lb_lo(_C_TYPES[t]))

    def hi(self, t: ValueType) -> int:
        return int(self.lib.lb_hi(_C_TYPES[t]))

    def in_range(self, t: ValueType, value: Value) -> bool:
        return bool(self.lib.lb_in_range(_C_TYPES[t], _c_value(value)))

    def short_id(self, uid: bytes) -> int:
        device = sim.SimDevice("output", uid)
        try:
            return device.short_id
        finally:
            device.close()

    def encode_blob(self, data: bytes) -> bytes | None:
        buf = ctypes.create_string_buffer(len(data) + 2)
        n = self.lib.lb_encode_blob(data, len(data), buf, len(data) + 2)
        return buf.raw[:n] if n else None

    def string_whole(self, data: bytes) -> bool:
        return bool(self.lib.lb_string_whole(data, len(data)))

    def quantity_type(self, quantity: int) -> ValueType | None:
        """The type of a quantity's values, `None` for one the library does not know
        (`lb_quantity_type` answers `LB_T_COUNT` — past every position in `ValueType`)."""
        number = int(self.lib.lb_quantity_type(quantity))
        return list(ValueType)[number] if 0 <= number < len(ValueType) else None

    def decode_blob(self, data: bytes) -> bytes | None:
        # `lb_decode_blob` hands back a pointer INTO the input; through ctypes that is
        # an address to read from, so the prefix check is what is compared here.
        out_ptr = ctypes.c_void_p()
        out_len = ctypes.c_size_t()
        buf = ctypes.create_string_buffer(data, max(len(data), 1))
        if not self.lib.lb_decode_blob(
            buf, len(data), ctypes.byref(out_ptr), ctypes.byref(out_len)
        ):
            return None
        return ctypes.string_at(out_ptr.value or 0, out_len.value) if out_len.value else b""


class CDevice:
    """A `SimDevice` behind the test interface."""

    def __init__(self, profile: str, uid: bytes, jitter: int, channels: int = 0) -> None:
        self.device = sim.SimDevice(profile, uid, jitter, channels)

    def set_clock(self, ms: int) -> None:
        self.device.set_clock(ms)

    def store_put(self, key: str, data: bytes) -> None:
        self.device.store_put(key, data)

    def store_get(self, key: str) -> bytes | None:
        return self.device.store_get(key)

    def start(self, pins: int) -> None:
        self.device.start(pins)

    def rx(self, f: Frame) -> None:
        self.device.rx(f)

    def loop(self, pins: int) -> None:
        self.device.loop(pins)

    def take_tx(self) -> list[Frame]:
        return self.device.take_tx()

    def allow_restart(self) -> None:
        self.device.allow_restart()

    def restart_requested(self) -> bool:
        return self.device.restart_requested

    def driven(self, index: int = 0) -> int | None:
        return self.device.driven(index)

    def service_press(self) -> None:
        self.device.service_press()

    def state(self) -> str | None:
        return self.device.state

    def set_bus(self, state: str) -> None:
        self.device.set_bus(sim.BUS_STATES[state])

    def set_errors(self, tec: int, rec: int) -> None:
        self.device.set_errors(tec, rec)

    def set_reset_reason(self, reason: str) -> None:
        self.device.set_reset_reason(sim.RESET_REASONS[reason])

    def sense(self, endpoint: int, raw: int | None) -> None:
        self.device.sense(endpoint, raw)
