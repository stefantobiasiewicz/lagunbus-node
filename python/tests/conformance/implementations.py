"""The two implementations behind one interface, so a test does not know which it drives.

`Codec` and `Device` are what every conformance test is written against. There are two
of each: `Py*` over the Python library, `C*` over `liblagunbus_harness` through
`ctypes`. A test parametrised over both runs the same data through both, and any
difference in output is a failure — of one library, or of the spec's wording.
"""

from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Protocol

from lagunbus_node import frame, values
from lagunbus_node.endpoint import Endpoint
from lagunbus_node.frame import Frame
from lagunbus_node.key import Key
from lagunbus_node.memory import MemoryPlatform, QueueTransport
from lagunbus_node.node import Node
from lagunbus_node.output import Output
from lagunbus_node.values import Value, ValueType
from tests.conformance import profiles

# --- the interface ------------------------------------------------------------------


class Codec(Protocol):
    def encode_id(self, kind: int, address: int, endpoint: int, element: int) -> int | None: ...
    def decode_id(self, can_id: int) -> tuple[int, int, int, int]: ...
    def join_id(self, short_id: int, announce: bool) -> int: ...
    def encode_header(self, tid: int, result: int) -> int: ...
    def decode_header(self, byte: int) -> tuple[int, int]: ...
    def encode(self, t: ValueType, value: Value) -> bytes | None: ...
    def decode(self, t: ValueType, data: bytes) -> tuple[Value] | None: ...
    def lo(self, t: ValueType) -> int: ...
    def hi(self, t: ValueType) -> int: ...
    def in_range(self, t: ValueType, value: Value) -> bool: ...
    def short_id(self, uid: bytes) -> int: ...
    def encode_blob(self, data: bytes) -> bytes | None: ...
    def decode_blob(self, data: bytes) -> bytes | None: ...


class Device(Protocol):
    """A node with one of the two profiles, a clock and a store the test controls."""

    def set_clock(self, ms: int) -> None: ...
    def store_put(self, key: str, data: bytes) -> None: ...
    def store_get(self, key: str) -> bytes | None: ...
    def start(self, pin: bool) -> None: ...
    def rx(self, f: Frame) -> None: ...
    def loop(self, pin: bool) -> None: ...
    def take_tx(self) -> list[Frame]: ...
    def driven(self) -> int | None: ...


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

    def encode(self, t: ValueType, value: Value) -> bytes | None:
        try:
            return values.encode(t, value)
        except ValueError:
            return None

    def decode(self, t: ValueType, data: bytes) -> tuple[Value] | None:
        try:
            return (values.decode(t, data),)
        except ValueError:
            return None

    def lo(self, t: ValueType) -> int:
        return values.lo(t)

    def hi(self, t: ValueType) -> int:
        return values.hi(t)

    def in_range(self, t: ValueType, value: Value) -> bool:
        return values.in_range(t, value)

    def short_id(self, uid: bytes) -> int:
        return Node(QueueTransport(), MemoryPlatform(), profiles.identity(0, uid)).short_id()

    def encode_blob(self, data: bytes) -> bytes | None:
        try:
            return values.encode_blob(data)
        except ValueError:
            return None

    def decode_blob(self, data: bytes) -> bytes | None:
        try:
            return values.decode_blob(data)
        except ValueError:
            return None


class PyDevice:
    def __init__(self, profile: str, uid: bytes, jitter: int) -> None:
        self.bus = QueueTransport()
        self.platform = MemoryPlatform(jitter=jitter)
        self.profile = profile
        self._driven: int | None = None
        self.endpoint: Endpoint
        if profile == "key":
            self.endpoint = profiles.key_endpoint()
            device_type = 0x0010
        elif profile == "output":
            self.endpoint = profiles.output_endpoint()
            device_type = 0x0020
        else:
            raise ValueError(f"unknown profile {profile!r}")
        self.node = Node(
            self.bus, self.platform, profiles.identity(device_type, uid), [self.endpoint]
        )
        self.key: Key | None = None
        self.output: Output | None = None

    def _drive(self, level: bool) -> None:
        self._driven = 1 if level else 0

    def set_clock(self, ms: int) -> None:
        self.platform.clock = ms

    def store_put(self, key: str, data: bytes) -> None:
        self.platform.store(key, data)

    def store_get(self, key: str) -> bytes | None:
        return self.platform.stored.get(key)

    def start(self, pin: bool) -> None:
        self.node.start()
        if self.profile == "key":
            self.key = Key(self.endpoint, pin, self.platform.clock)
        else:
            output = Output(self.node, self.endpoint, self._drive)
            self.endpoint.on_command = lambda _ep, element, _args: output.command(element)
            self.output = output

    def rx(self, f: Frame) -> None:
        self.bus.inbox.append(f)

    def loop(self, pin: bool) -> None:
        self.node.tick()
        if self.key is not None:
            self.key.update(self.node, pin, self.platform.clock)
        if self.output is not None:
            self.output.tick(self.platform.clock)

    def take_tx(self) -> list[Frame]:
        return self.bus.take()

    def driven(self) -> int | None:
        return self._driven


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
"""`lb_type_t` numbers. `ValueType` is declared in the order of the C enum —
test_codec.py checks that the sizes agree, which would catch a reordering."""


def load_library(path: Path) -> ctypes.CDLL:
    lib = ctypes.CDLL(str(path))
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

    sig("lbh_create", ptr, ctypes.c_char_p, ctypes.c_char_p, u32)
    sig("lbh_destroy", None, ptr)
    sig("lbh_set_clock", None, ptr, u32)
    sig("lbh_store_put", b, ptr, ctypes.c_char_p, ctypes.c_char_p, u8)
    sig("lbh_store_get", ctypes.c_int, ptr, ctypes.c_char_p, ctypes.c_char_p, u8)
    sig("lbh_start", None, ptr, b)
    sig("lbh_rx", None, ptr, u32, ctypes.c_char_p, u8)
    sig("lbh_loop", None, ptr, b)
    sig("lbh_tx_count", ctypes.c_int, ptr)
    sig(
        "lbh_tx_get",
        None,
        ptr,
        ctypes.c_int,
        ctypes.POINTER(u32),
        ctypes.c_char_p,
        ctypes.POINTER(u8),
    )
    sig("lbh_tx_clear", None, ptr)
    sig("lbh_driven", ctypes.c_int, ptr)
    sig("lbh_short_id", u32, ptr)
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
        h = self.lib.lbh_create(b"output", uid, 0)
        try:
            return int(self.lib.lbh_short_id(h))
        finally:
            self.lib.lbh_destroy(h)

    def encode_blob(self, data: bytes) -> bytes | None:
        buf = ctypes.create_string_buffer(len(data) + 2)
        n = self.lib.lb_encode_blob(data, len(data), buf, len(data) + 2)
        return buf.raw[:n] if n else None

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
    def __init__(self, lib: ctypes.CDLL, profile: str, uid: bytes, jitter: int) -> None:
        self.lib = lib
        self.h = lib.lbh_create(profile.encode(), uid, jitter)
        if not self.h:
            raise ValueError(f"unknown profile {profile!r}")

    def __del__(self) -> None:
        if getattr(self, "h", None):
            self.lib.lbh_destroy(self.h)

    def set_clock(self, ms: int) -> None:
        self.lib.lbh_set_clock(self.h, ms)

    def store_put(self, key: str, data: bytes) -> None:
        self.lib.lbh_store_put(self.h, key.encode(), data, len(data))

    def store_get(self, key: str) -> bytes | None:
        buf = ctypes.create_string_buffer(8)
        n = self.lib.lbh_store_get(self.h, key.encode(), buf, 8)
        return None if n < 0 else buf.raw[:n]

    def start(self, pin: bool) -> None:
        self.lib.lbh_start(self.h, pin)

    def rx(self, f: Frame) -> None:
        self.lib.lbh_rx(self.h, f.can_id, f.data, len(f.data))

    def loop(self, pin: bool) -> None:
        self.lib.lbh_loop(self.h, pin)

    def take_tx(self) -> list[Frame]:
        out: list[Frame] = []
        can_id = ctypes.c_uint32()
        length = ctypes.c_uint8()
        buf = ctypes.create_string_buffer(8)
        for i in range(self.lib.lbh_tx_count(self.h)):
            self.lib.lbh_tx_get(self.h, i, ctypes.byref(can_id), buf, ctypes.byref(length))
            out.append(Frame(int(can_id.value), buf.raw[: length.value]))
        self.lib.lbh_tx_clear(self.h)
        return out

    def driven(self) -> int | None:
        level = int(self.lib.lbh_driven(self.h))
        return None if level < 0 else level
