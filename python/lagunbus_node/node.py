"""The node — the loop that receives frames and answers them.

The Python twin of `c/lagunbus/src/node.c`, **function for function**. Where the C code
has a comment explaining why, the reason holds here too and is not repeated; the C
file is the one to read for the rationale. What this file adds is the places where
Python has to work to behave like C:

- **fixed widths.** C counters wrap on their own — `event_seq` is a `uint8_t`, `sent` a
  `uint32_t`. Python integers do not, so every such field is masked where it changes.
- **time is u32.** `millis()` wraps after 49 days; deadlines are computed modulo 2**32
  and compared the way node.c compares them, including the one place that uses a
  signed difference (`reset_at_ms`).

The two libraries are held together by the conformance tests
(python/tests/conformance/), which run the same scenarios through both.

**There is no thread here.** `tick()` is an ordinary method; who calls it is the
port's business.
"""

from __future__ import annotations

import zlib
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import Final

from lagunbus_node import values
from lagunbus_node.endpoint import COMMON_BASE, Endpoint
from lagunbus_node.frame import (
    ADDRESS_UNASSIGNED,
    MAX_PAYLOAD,
    Fields,
    Frame,
    decode_header,
    decode_id,
    encode_header,
    encode_id,
    is_join,
    join_id,
    join_is_announce,
)
from lagunbus_node.interfaces import Platform, Transport
from lagunbus_node.registry import CommonAttribute, EndpointType, MessageKind, ResultCode
from lagunbus_node.values import ValueType

U8: Final = 0xFF
U16: Final = 0xFFFF
U32: Final = 0xFFFFFFFF

UID_LEN: Final = 8

# Reason for the last restart (types/0x0001-device.md §3).
RESET_POWER_ON: Final = 0x00
RESET_SOFTWARE: Final = 0x01
RESET_WATCHDOG: Final = 0x02
RESET_BROWNOUT: Final = 0x03

# `Device` (ep0) elements.
DEV_UID: Final = 0x00
DEV_SHORT_ID: Final = 0x01
DEV_DEVICE_TYPE: Final = 0x02
DEV_PROTOCOL_VERSION: Final = 0x03
DEV_FIRMWARE_VERSION: Final = 0x04
DEV_HARDWARE_VERSION: Final = 0x05
DEV_VENDOR_ID: Final = 0x06
DEV_ENDPOINT_COUNT: Final = 0x07
DEV_UPTIME: Final = 0x10
DEV_BOOT_COUNT: Final = 0x11
DEV_LAST_RESET: Final = 0x12
DEV_EVENT_BOOTED: Final = 0x01

DEV_CMD_FACTORY_RESET: Final = 0x02
FACTORY_RESET_MAGIC: Final = 0x4C41474E
"""`LAGN` — without it `FactoryReset` is refused (types/0x0001-device.md §3.3)."""
RESET_GRACE_MS: Final = 50

# `Diagnostics` (ep1) and `Groups` (ep2) elements.
DIAG_TX_ERROR_COUNT: Final = 0x00
DIAG_RX_ERROR_COUNT: Final = 0x01
DIAG_BUS_STATE: Final = 0x02
DIAG_BUS_OFF_COUNT: Final = 0x03
DIAG_TX_FRAMES: Final = 0x04
DIAG_RX_FRAMES: Final = 0x05
DIAG_TX_DROPPED: Final = 0x06
DIAG_ERROR_THRESHOLD: Final = 0x20
DIAG_CMD_RESET: Final = 0x00

GROUPS_CAPACITY: Final = 0x00
GROUPS_COUNT: Final = 0x01

# System endpoint numbers — the same in every device (01-model-danych.md §2).
EP_DEVICE: Final = 0
EP_DIAGNOSTICS: Final = 1
EP_GROUPS: Final = 2
EP_FIRST_USER: Final = 4

# Segments (05-mapowanie-can.md §6). Byte [0] = LAST|Tid, [1] = number, [2..7] = data.
SEGMENT_DATA: Final = 6
SEGMENT_LAST: Final = 0x80

# Keys in persistent storage — the same strings as in node.c, so that a state
# directory is readable by both.
KEY_ADDRESS: Final = "address"
KEY_BOOT: Final = "boot"

_SYSTEM_TYPES: Final = (EndpointType.Device, EndpointType.Diagnostics, EndpointType.Groups)


@dataclass(frozen=True)
class Identity:
    """Identity and whatever the hub reads from `Device`. Fixed for the node's life."""

    uid: bytes
    node_class: int
    device_type: int
    vendor_id: int = 0
    firmware: int = 0
    hardware: int = 0
    protocol: int = 1

    def __post_init__(self) -> None:
        if len(self.uid) != UID_LEN:
            raise ValueError(f"Uid is {UID_LEN} bytes, got {len(self.uid)}")


@dataclass
class Reporting:
    """The reporting settings of one system endpoint — `lb_reporting_t`."""

    report_interval: int = 0
    report_min_gap: int = 0
    report_hysteresis: int = 0


Watch = Callable[[Frame, bool], None]
"""Traffic watch — `(frame, outgoing)`, for EVERY frame, other nodes' included."""


def _u16(data: bytes) -> int:
    return int.from_bytes(data, "little")


class Node:
    def __init__(
        self,
        bus: Transport,
        platform: Platform,
        identity: Identity,
        endpoints: Sequence[Endpoint] = (),
    ) -> None:
        self.bus = bus
        self.platform = platform
        self.identity = identity
        # **Functional** endpoints, from number 4 up. `Device` is served by the node.
        self.endpoints = list(endpoints)

        self.address = ADDRESS_UNASSIGNED
        self.reset_reason = RESET_POWER_ON
        self.boot_count = 0

        self.next_join_ms = 0
        self.join_backoff_s = 0
        self.join_attempts = 0

        self.started_ms = 0
        self.booted = False

        self.reset_pending = False
        self.reset_at_ms = 0

        self.received = 0
        self.sent = 0
        self.dropped = 0
        self.device_event_seq = 0

        self.error_threshold = 0
        self.system_reporting = [Reporting() for _ in range(EP_GROUPS + 1)]
        self.bus_off_count = 0

        self.on_frame: Watch | None = None
        self._system_defaults()

    # --- identity -------------------------------------------------------------------

    def short_id(self) -> int:
        """`ShortId` — CRC-32 of `Uid` (types/0x0001-device.md §2.1). zlib's CRC-32 is
        the same one node.c computes without a table."""
        return zlib.crc32(self.identity.uid) & U32

    def endpoint(self, number: int) -> Endpoint | None:
        for ep in self.endpoints:
            if ep.number == number:
                return ep
        return None

    # --- sending --------------------------------------------------------------------

    def now(self) -> int:
        """The platform clock as u32 — the one every deadline in the node is measured on."""
        return self.platform.millis() & U32

    def _send(self, can_id: int, data: bytes) -> bool:
        frame = Frame(can_id, data)
        if self.bus.send(frame):
            self.sent = (self.sent + 1) & U32
            if self.on_frame is not None:
                self.on_frame(frame, True)
            return True
        self.dropped = (self.dropped + 1) & U32
        return False

    def send_segments(self, endpoint: int, element: int, tid: int, value: bytes) -> bool:
        """A value too large for one frame, as a run of `LONG_RESPONSE` frames."""
        try:
            can_id = encode_id(MessageKind.LONG_RESPONSE, self.address, endpoint, element)
        except ValueError:
            return False
        number = 0
        done = 0
        while done < len(value):
            chunk = value[done : done + SEGMENT_DATA]
            last = done + len(chunk) >= len(value)
            header = (SEGMENT_LAST if last else 0) | (tid & 0x07)
            if not self._send(can_id, bytes([header, number & U8]) + chunk):
                return False  # a broken run is a lost segment (05-mapowanie-can.md §6.6)
            done += len(chunk)
            number += 1
        return True

    def report(self, endpoint: int, element: int, value: bytes) -> bool:
        """A REPORT — the bare value, no header byte: nobody asked, nobody acknowledges."""
        if not self.booted or self.address == ADDRESS_UNASSIGNED:
            return False
        try:
            can_id = encode_id(MessageKind.REPORT, self.address, endpoint, element)
        except ValueError:
            return False
        return self._send(can_id, value)

    def emit(self, endpoint: int, event: int, params: bytes = b"") -> bool:
        """An event with its sequence number first (01-model-danych.md §3.3)."""
        if not self.booted or len(params) + 1 > MAX_PAYLOAD:
            return False
        if endpoint == 0:
            seq = self.device_event_seq
            self.device_event_seq = (seq + 1) & U8
        else:
            ep = self.endpoint(endpoint)
            if ep is None:
                return False
            seq = ep.event_seq
            ep.event_seq = (seq + 1) & U8
        try:
            can_id = encode_id(MessageKind.EVENT, self.address, endpoint, event)
        except ValueError:
            return False
        return self._send(can_id, bytes([seq]) + params)

    def _announce_booted(self) -> None:
        params = bytes([self.reset_reason]) + values.encode(ValueType.U32, self.identity.firmware)
        self.emit(0, DEV_EVENT_BOOTED, params)

    # --- life cycle -----------------------------------------------------------------

    def _system_defaults(self) -> None:
        self.system_reporting = [Reporting() for _ in range(EP_GROUPS + 1)]
        self.error_threshold = 96
        self.system_reporting[EP_DIAGNOSTICS].report_interval = 300
        self.system_reporting[EP_DIAGNOSTICS].report_hysteresis = 16

    def start(self) -> None:
        """Load the address, count the boot, and only then answer and announce."""
        self.started_ms = self.now()

        saved = self.platform.load(KEY_ADDRESS, 2)
        if saved is not None:
            self.address = _u16(saved)
        boot = self.platform.load(KEY_BOOT, 2)
        if boot is not None:
            self.boot_count = _u16(boot)
        self.boot_count = (self.boot_count + 1) & U16
        self.platform.store(KEY_BOOT, self.boot_count.to_bytes(2, "little"))

        self.booted = True

        if self.address != ADDRESS_UNASSIGNED:
            self._announce_booted()
        else:
            self._schedule_announce()

    def _schedule_announce(self) -> None:
        now = self.now()
        jitter = self.platform.random() & U32
        if self.join_attempts == 0:
            self.join_backoff_s = 1
        elif self.join_backoff_s < 60:
            self.join_backoff_s = (self.join_backoff_s * 2) & U16
        window = self.join_backoff_s * 1000
        self.next_join_ms = (now + jitter % window) & U32

    def _announce(self) -> None:
        short_id = self.short_id()
        payload = values.encode(ValueType.U32, short_id) + bytes([self.identity.node_class])
        self._send(join_id(short_id, True), payload)
        self.join_attempts = (self.join_attempts + 1) & U16
        self._schedule_announce()

    def _accept_assignment(self, frame: Frame) -> None:
        if self.address != ADDRESS_UNASSIGNED or len(frame.data) < 6:
            return
        # "Absent" decodes to 0 here, as `LB_ABSENT` carries 0 in node.c.
        who = values.decode(ValueType.U32, frame.data[0:4]) or 0
        address = values.decode(ValueType.U16, frame.data[4:6]) or 0
        if who != self.short_id():
            return
        self.address = address
        self.platform.store(KEY_ADDRESS, self.address.to_bytes(2, "little"))
        self._announce_booted()

    def _factory_reset(self) -> None:
        self.reset_pending = False
        self.address = ADDRESS_UNASSIGNED
        self.platform.store(KEY_ADDRESS, self.address.to_bytes(2, "little"))
        if self.platform.restart():
            return
        self._system_defaults()
        self.join_attempts = 0
        self._schedule_announce()

    # --- Device, Diagnostics, Groups ------------------------------------------------

    def _device_read(self, element: int) -> tuple[int, bytes]:
        ident = self.identity
        match element:
            case 0x00:  # DEV_UID
                return ResultCode.SEGMENTED, b""
            case 0x01:
                return ResultCode.OK, values.encode(ValueType.U32, self.short_id())
            case 0x02:
                return ResultCode.OK, values.encode(ValueType.U16, ident.device_type)
            case 0x03:
                return ResultCode.OK, values.encode(ValueType.U16, ident.protocol)
            case 0x04:
                return ResultCode.OK, values.encode(ValueType.U32, ident.firmware)
            case 0x05:
                return ResultCode.OK, values.encode(ValueType.U16, ident.hardware)
            case 0x06:
                return ResultCode.OK, values.encode(ValueType.U16, ident.vendor_id)
            case 0x07:
                return ResultCode.OK, values.encode(ValueType.U8, len(self.endpoints) & U8)
            case 0x10:
                uptime = ((self.now() - self.started_ms) & U32) // 1000
                return ResultCode.OK, values.encode(ValueType.U32, uptime)
            case 0x11:
                return ResultCode.OK, values.encode(ValueType.U16, self.boot_count)
            case 0x12:
                return ResultCode.OK, values.encode(ValueType.ENUM8, self.reset_reason)
        return ResultCode.UNSUPPORTED_ELEMENT, b""

    def _diagnostics_read(self, element: int) -> tuple[int, bytes]:
        health = self.bus.health()
        if element in (DIAG_TX_ERROR_COUNT, DIAG_RX_ERROR_COUNT, DIAG_BUS_STATE):
            if health is None:
                return ResultCode.UNAVAILABLE, b""
            tec, rec, state = health
            if element == DIAG_TX_ERROR_COUNT:
                return ResultCode.OK, values.encode(ValueType.U16, tec)
            if element == DIAG_RX_ERROR_COUNT:
                return ResultCode.OK, values.encode(ValueType.U16, rec)
            return ResultCode.OK, values.encode(ValueType.ENUM8, state)
        match element:
            case 0x03:
                return ResultCode.OK, values.encode(ValueType.U16, self.bus_off_count)
            case 0x04:
                return ResultCode.OK, values.encode(ValueType.U32, self.sent)
            case 0x05:
                return ResultCode.OK, values.encode(ValueType.U32, self.received)
            case 0x06:
                return ResultCode.OK, values.encode(ValueType.U16, self.dropped & U16)
            case 0x20:
                return ResultCode.OK, values.encode(ValueType.U16, self.error_threshold)
        return ResultCode.UNSUPPORTED_ELEMENT, b""

    def _groups_read(self, element: int) -> tuple[int, bytes]:
        if element not in (GROUPS_CAPACITY, GROUPS_COUNT):
            return ResultCode.UNSUPPORTED_ELEMENT, b""
        return ResultCode.OK, values.encode(ValueType.U8, 0)

    def _system_common_read(self, endpoint: int, element: int) -> tuple[int, bytes]:
        reporting = self.system_reporting[endpoint]
        match element:
            case CommonAttribute.TypeId:
                return ResultCode.OK, values.encode(ValueType.U16, _SYSTEM_TYPES[endpoint])
            case CommonAttribute.TypeRevision:
                return ResultCode.OK, values.encode(ValueType.U8, 1)
            case CommonAttribute.Features | CommonAttribute.QueryEnabled:
                return ResultCode.OK, values.encode(ValueType.MAP16, 0)
            case CommonAttribute.Enabled:
                return ResultCode.OK, values.encode(ValueType.BOOL, 1)
            case CommonAttribute.Channel:
                return ResultCode.OK, values.encode(ValueType.U8, 0)
            case CommonAttribute.ReportInterval:
                return ResultCode.OK, values.encode(ValueType.U16, reporting.report_interval)
            case CommonAttribute.ReportMinGap:
                return ResultCode.OK, values.encode(ValueType.U16, reporting.report_min_gap)
            case CommonAttribute.ReportHysteresis:
                return ResultCode.OK, values.encode(ValueType.U16, reporting.report_hysteresis)
        return ResultCode.UNSUPPORTED_ELEMENT, b""

    def _system_common_write(self, endpoint: int, element: int, data: bytes) -> int:
        reporting = self.system_reporting[endpoint]
        match element:
            case (
                CommonAttribute.TypeId
                | CommonAttribute.TypeRevision
                | CommonAttribute.Features
                | CommonAttribute.Channel
            ):
                return ResultCode.READ_ONLY
            case CommonAttribute.Enabled:
                v = _decode_or_none(ValueType.BOOL, data)
                return ResultCode.INVALID_VALUE if not v else ResultCode.OK
            case CommonAttribute.QueryEnabled:
                v = _decode_or_none(ValueType.MAP16, data)
                return ResultCode.INVALID_VALUE if v is None or v != 0 else ResultCode.OK
            case (
                CommonAttribute.ReportInterval
                | CommonAttribute.ReportMinGap
                | CommonAttribute.ReportHysteresis
            ):
                v = _decode_or_none(ValueType.U16, data)
                if v is None:
                    return ResultCode.INVALID_VALUE
                if element == CommonAttribute.ReportInterval:
                    reporting.report_interval = v
                elif element == CommonAttribute.ReportMinGap:
                    reporting.report_min_gap = v
                else:
                    reporting.report_hysteresis = v
                return ResultCode.OK
        return ResultCode.UNSUPPORTED_ELEMENT

    def _system_read(self, endpoint: int, element: int) -> tuple[int, bytes]:
        if endpoint > EP_GROUPS:
            return ResultCode.UNSUPPORTED_ENDPOINT, b""  # ep3 Scenes — not in this device
        if element >= COMMON_BASE:
            return self._system_common_read(endpoint, element)
        if endpoint == EP_DIAGNOSTICS:
            return self._diagnostics_read(element)
        return self._groups_read(element)

    def _system_write(self, endpoint: int, element: int, data: bytes) -> int:
        if endpoint > EP_GROUPS:
            return ResultCode.UNSUPPORTED_ENDPOINT
        if element >= COMMON_BASE:
            return self._system_common_write(endpoint, element, data)
        if endpoint != EP_DIAGNOSTICS or element != DIAG_ERROR_THRESHOLD:
            return ResultCode.READ_ONLY
        v = _decode_or_none(ValueType.U16, data)
        if v is None or not 16 <= v <= 255:
            return ResultCode.INVALID_VALUE
        self.error_threshold = v
        return ResultCode.OK

    def _system_command(self, endpoint: int, element: int) -> int:
        if endpoint == EP_DIAGNOSTICS and element == DIAG_CMD_RESET:
            self.received = 0
            self.sent = 0
            self.dropped = 0
            return ResultCode.OK
        if endpoint == EP_GROUPS:
            return (
                ResultCode.INSUFFICIENT_SPACE if element <= 0x03 else ResultCode.UNSUPPORTED_ELEMENT
            )
        if endpoint != EP_DIAGNOSTICS:
            return ResultCode.UNSUPPORTED_ENDPOINT
        return ResultCode.UNSUPPORTED_ELEMENT

    def _device_command(self, element: int, args: bytes) -> int:
        if element != DEV_CMD_FACTORY_RESET:
            return ResultCode.UNSUPPORTED_ELEMENT
        if len(args) < 4 or values.decode(ValueType.U32, args[0:4]) != FACTORY_RESET_MAGIC:
            return ResultCode.INVALID_VALUE
        self.reset_pending = True
        self.reset_at_ms = (self.now() + RESET_GRACE_MS) & U32
        return ResultCode.OK

    # --- the loop -------------------------------------------------------------------

    def _respond(self, f: Fields, tid: int, result: int, data: bytes) -> None:
        try:
            can_id = encode_id(MessageKind.RESPONSE, self.address, f.endpoint, f.element)
        except ValueError:
            return
        self._send(can_id, bytes([encode_header(tid, result)]) + data)

    def _handle(self, frame: Frame) -> None:
        if is_join(frame.can_id):
            if not join_is_announce(frame.can_id):
                self._accept_assignment(frame)
            return
        f = decode_id(frame.can_id)
        if f.address != self.address or self.address == ADDRESS_UNASSIGNED:
            return
        if not frame.data:
            return
        tid, _ = decode_header(frame.data[0])
        body = frame.data[1:]

        data = b""
        if f.kind == MessageKind.READ:
            if f.endpoint == EP_DEVICE and f.element < COMMON_BASE:
                result, data = self._device_read(f.element)
            elif f.endpoint < EP_FIRST_USER:
                result, data = self._system_read(f.endpoint, f.element)
            else:
                ep = self.endpoint(f.endpoint)
                if ep is None:
                    result = ResultCode.UNSUPPORTED_ENDPOINT
                else:
                    result, data = ep.read(f.element)
        elif f.kind == MessageKind.WRITE:
            ep = self.endpoint(f.endpoint)
            if f.endpoint < EP_FIRST_USER:
                result = self._system_write(f.endpoint, f.element, body)
            elif ep is None:
                result = ResultCode.UNSUPPORTED_ENDPOINT
            else:
                result = ep.write(f.element, body)
        elif f.kind == MessageKind.COMMAND:
            ep = self.endpoint(f.endpoint)
            if f.endpoint < EP_FIRST_USER:
                result = (
                    self._device_command(f.element, body)
                    if f.endpoint == EP_DEVICE
                    else self._system_command(f.endpoint, f.element)
                )
            elif ep is None:
                result = ResultCode.UNSUPPORTED_ENDPOINT
            elif ep.on_command is None:
                result = ResultCode.UNSUPPORTED_ELEMENT
            else:
                result = ep.on_command(ep, f.element, body)
        else:
            return  # reports, events, other nodes' replies

        # A failed read carries no value — node.c sends `len = 0` whenever the result
        # is not OK, whatever the buffer holds.
        self._respond(f, tid, result, data if result == ResultCode.OK else b"")

        if result == ResultCode.SEGMENTED and f.endpoint == EP_DEVICE and f.element == DEV_UID:
            self.send_segments(f.endpoint, f.element, tid, values.encode_blob(self.identity.uid))

    def tick(self) -> None:
        """One turn of the loop. Never blocks."""
        if not self.booted:
            return  # before Booted a node ANSWERS NOTHING (types/0x0001-device.md §5.1)

        if self.reset_pending:
            diff = (self.now() - self.reset_at_ms) & U32
            if diff < 0x80000000:  # `(int32_t)(now - reset_at) >= 0`
                self._factory_reset()

        if self.address == ADDRESS_UNASSIGNED and self.now() >= self.next_join_ms:
            self._announce()

        for _ in range(16):
            frame = self.bus.recv()
            if frame is None:
                return
            self.received = (self.received + 1) & U32
            if self.on_frame is not None:
                self.on_frame(frame, False)
            self._handle(frame)


def _decode_or_none(t: ValueType, data: bytes) -> int | None:
    """Decoded value, or `None` for a wrong length AND for "absent" — the cases node.c
    folds into one `!lb_decode(...) || v.absent`."""
    try:
        return values.decode(t, data)
    except ValueError:
        return None
