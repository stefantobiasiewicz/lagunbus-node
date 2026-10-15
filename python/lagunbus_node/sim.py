"""The C node from Python — `liblagunbus_sim` through `ctypes`.

One binding for the two users of the C library outside a board: the conformance tests
and the simulator `lagun-sim` (zadania/zrobione/FEAT-02.md, N10). What the library exposes and
why it is a flat API rather than the structs — `c/sim/sim.h`. Which devices exist —
`c/sim/devices.c`, listed by `devices()`.

The library is **built on first use** with CMake, into `c/build`, and rebuilt when a
source is newer: `cmake --build` is incremental, so it costs nothing when nothing
changed, and a simulator or a test never runs yesterday's C code. This needs the source
tree next to the package (`pip install -e`), a C compiler and CMake. `LAGUNBUS_SIM` set to
a library file skips the build and loads that file.
"""

from __future__ import annotations

import ctypes
import functools
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Final, NamedTuple

from lagunbus_node.frame import Frame

C_DIR: Final = Path(__file__).resolve().parents[2] / "c"
BUILD: Final = C_DIR / "build"
ENV: Final = "LAGUNBUS_SIM"

KEY_SIZE: Final = 16
VALUE_SIZE: Final = 8


BUS_STATES: Final = {"active": 0, "warning": 1, "passive": 2, "off": 3}
"""`LB_BUS_*` from transport.h, by the names scenarios use."""

RESET_REASONS: Final = {
    "POWER_ON": 0,
    "EXTERNAL": 1,
    "WATCHDOG": 2,
    "SOFTWARE": 3,
    "BROWNOUT": 4,
    "UNKNOWN": 5,
}
"""`LastResetReason` (types/0x0001-device.md §2.4) — the values, not the C constants, so
a constant that drifts from the specification fails a scenario."""


class BuildError(RuntimeError):
    """The C library could not be built — the message carries the compiler's output."""


def _library_name() -> str:
    # A lookup, not an `if` on `sys.platform`: mypy evaluates those for the platform it
    # runs on and calls the other branch unreachable.
    suffix = {"darwin": "dylib"}.get(sys.platform, "so")
    return f"liblagunbus_sim.{suffix}"


def library_path() -> Path:
    """The library file, built or brought up to date first. Raises `BuildError`."""
    override = os.environ.get(ENV)
    if override:
        return Path(override)
    cmake = shutil.which("cmake")
    if cmake is None:
        raise BuildError("building the C library needs cmake on PATH")
    if not (BUILD / "CMakeCache.txt").exists():
        configured = subprocess.run(
            [cmake, "-S", str(C_DIR), "-B", str(BUILD)], capture_output=True, text=True
        )
        if configured.returncode != 0:
            raise BuildError(f"cmake configure failed:\n{configured.stdout}{configured.stderr}")
    built = subprocess.run(
        [cmake, "--build", str(BUILD), "--target", "lagunbus_sim"],
        capture_output=True,
        text=True,
    )
    if built.returncode != 0:
        raise BuildError(f"building liblagunbus_sim failed:\n{built.stdout}{built.stderr}")
    return BUILD / _library_name()


@functools.cache
def load() -> ctypes.CDLL:
    """The library, loaded once per process with every `lbs_…` signature declared.

    The library's own functions (`lb_encode`, …) are in it too, undeclared — a caller
    that wants them declares them itself, as the codec conformance tests do.
    """
    lib = ctypes.CDLL(str(library_path()))
    u8, u16, u32, b = ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_bool
    ptr, i32, size, text = ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t, ctypes.c_char_p

    def sig(name: str, res: object, *args: object) -> None:
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = list(args)

    sig("lbs_device_count", size)
    sig("lbs_device_name", text, size)
    sig("lbs_device_about", text, size)
    sig("lbs_device_max_channels", i32, size)
    sig("lbs_create", ptr, text, text, u32, ctypes.c_uint8)
    sig("lbs_destroy", None, ptr)
    sig("lbs_set_clock", None, ptr, u32)
    sig("lbs_allow_restart", None, ptr)
    sig("lbs_restart_requested", b, ptr)
    sig("lbs_store_put", b, ptr, text, text, u8)
    sig("lbs_store_get", i32, ptr, text, text, u8)
    sig("lbs_store_at", i32, ptr, i32, text, text)
    sig("lbs_store_slots", i32)
    sig("lbs_store_version", u32, ptr)
    sig("lbs_start", None, ptr, u32)
    sig("lbs_rx", None, ptr, u32, text, u8)
    sig("lbs_loop", None, ptr, u32)
    sig("lbs_tx_count", i32, ptr)
    sig("lbs_tx_get", None, ptr, i32, ctypes.POINTER(u32), text, ctypes.POINTER(u8))
    sig("lbs_tx_clear", None, ptr)
    sig("lbs_input_count", i32, ptr)
    sig("lbs_inputs_idle", u32, ptr)
    sig("lbs_output_count", i32, ptr)
    sig("lbs_driven", i32, ptr, i32)
    sig("lbs_output_on", i32, ptr, i32)
    sig("lbs_service_press", None, ptr)
    sig("lbs_state", i32, ptr)
    sig("lbs_state_reports", u32, ptr)
    sig("lbs_state_name", text, i32)
    sig("lbs_led", b, ptr)
    sig("lbs_set_bus", None, ptr, u8)
    sig("lbs_set_errors", None, ptr, u16, u16)
    sig("lbs_set_reset_reason", None, ptr, u8)
    sig("lbs_address", u16, ptr)
    sig("lbs_short_id", u32, ptr)
    sig("lbs_display_count", i32, ptr)
    sig("lbs_display_endpoint", i32, ptr, i32)
    sig("lbs_display_content", i32, ptr, i32)
    sig("lbs_display_quantity", i32, ptr, i32)
    sig("lbs_display_value", b, ptr, i32, ctypes.POINTER(i32))
    sig("lbs_display_text", i32, ptr, i32, text, i32)
    sig("lbs_display_drawn", u32, ptr)
    sig("lbs_display_glass", i32, ptr, i32, text, i32)
    sig("lbs_display_pending", b, ptr)
    sig("lbs_display_refreshes", u32, ptr)
    sig("lbs_sense_count", i32, ptr)
    sig("lbs_sense_endpoint", i32, ptr, i32)
    sig("lbs_sense_quantity", i32, ptr, i32)
    sig("lbs_sense_value", b, ptr, i32, ctypes.POINTER(i32))
    sig("lbs_sense_update", None, ptr, i32, i32)
    sig("lbs_sense_fail", None, ptr, i32)
    sig("lbs_dial_count", i32, ptr)
    sig("lbs_dial_endpoint", i32, ptr, i32)
    sig("lbs_dial_value", i32, ptr, i32)
    sig("lbs_dial_range", None, ptr, i32, ctypes.POINTER(i32), ctypes.POINTER(i32))
    sig("lbs_dial_turn", None, ptr, i32, i32)
    return lib


CONTENT: Final = ("NONE", "VALUE", "TEXT")
"""`Display.Content` (types/0x0022-display.md §2.3), by value."""


class DisplayField(NamedTuple):
    """What one display field shows — what a screen would draw."""

    endpoint: int
    content: str
    """`NONE`, `VALUE` or `TEXT`."""
    quantity: int | None
    """`Quantity` of a field with VALUE; the scale and unit are the viewer's."""
    value: int | None
    """The raw number, `None` when there is none to show."""
    text: str
    glass: str | None = None
    """On an e-paper of segment digits (`epd`): what the glass shows now, one character a
    place (`lagunbus/tools/digits.h`) — behind the field until the next refresh. `None`
    for a field without segment places."""


class Sensor(NamedTuple):
    """One `Sense` endpoint (types/0x0030-sense.md) — what it measures and what it holds."""

    endpoint: int
    quantity: int
    """`Quantity`; the scale and unit are the viewer's (`registry/quantities.yaml`)."""
    value: int | None
    """The raw `Value`, `None` while absent — before the first measurement or after a
    failed one."""


class Dial(NamedTuple):
    """One `ValueInput` endpoint (types/0x0011-value-input.md) — its value and range, in ‰."""

    endpoint: int
    value: int
    min_value: int
    max_value: int


class DeviceInfo(NamedTuple):
    name: str
    about: str
    max_channels: int
    """0: the device comes as it is. Otherwise it is made with 1 … `max_channels`
    channels, 1 by default — the board it copies."""


def devices() -> list[DeviceInfo]:
    """The catalogue, in the order of `devices.c`."""
    lib = load()
    return [
        DeviceInfo(
            lib.lbs_device_name(i).decode(),
            lib.lbs_device_about(i).decode(),
            int(lib.lbs_device_max_channels(i)),
        )
        for i in range(lib.lbs_device_count())
    ]


class SimDevice:
    """One device from the catalogue: the C node, its endpoints, a clock and a store
    set from Python. Frames go in through `rx` and come out of `take_tx`; nothing here
    touches a bus."""

    def __init__(self, name: str, uid: bytes, jitter: int = 0, channels: int = 0) -> None:
        """`channels` 0 is the device's default; another number only for a device that
        comes in several sizes (`DeviceInfo.max_channels`)."""
        if len(uid) != 8:
            raise ValueError("a Uid is 8 bytes")
        self.lib = load()
        self.name = name
        self.uid = uid
        self.channels = channels
        info = {d.name: d for d in devices()}.get(name)
        if info is None:
            raise ValueError(
                f"no device {name!r} in the catalogue ({', '.join(d.name for d in devices())})"
            )
        if channels and not 1 <= channels <= info.max_channels:
            sizes = f"1 … {info.max_channels}" if info.max_channels else "only as it is"
            raise ValueError(f"{name!r} comes with {sizes} channels, not {channels}")
        self._h = self.lib.lbs_create(name.encode(), uid, jitter & 0xFFFFFFFF, channels)
        if not self._h:
            raise ValueError(f"the C library refused {name!r} with {channels} channels")

    def close(self) -> None:
        if self._h:
            self.lib.lbs_destroy(self._h)
            self._h = None

    def __del__(self) -> None:
        if getattr(self, "_h", None):
            self.close()

    # --- the port --------------------------------------------------------------------

    def set_clock(self, ms: int) -> None:
        self.lib.lbs_set_clock(self._h, ms & 0xFFFFFFFF)

    def allow_restart(self) -> None:
        """Give the node a restart, as a board has: after `FactoryReset` it stops, and
        `restart_requested` says a new node is to be booted from the store."""
        self.lib.lbs_allow_restart(self._h)

    @property
    def restart_requested(self) -> bool:
        return bool(self.lib.lbs_restart_requested(self._h))

    def store_put(self, key: str, data: bytes) -> bool:
        return bool(self.lib.lbs_store_put(self._h, key.encode(), data, len(data)))

    def store_get(self, key: str) -> bytes | None:
        buf = ctypes.create_string_buffer(VALUE_SIZE)
        n = self.lib.lbs_store_get(self._h, key.encode(), buf, VALUE_SIZE)
        return None if n < 0 else buf.raw[:n]

    def store_items(self) -> dict[str, bytes]:
        """The whole persistent store — what a simulator saves between runs."""
        key = ctypes.create_string_buffer(KEY_SIZE)
        value = ctypes.create_string_buffer(VALUE_SIZE)
        items: dict[str, bytes] = {}
        for slot in range(self.lib.lbs_store_slots()):
            n = self.lib.lbs_store_at(self._h, slot, key, value)
            if n >= 0:
                items[key.value.decode()] = value.raw[:n]
        return items

    @property
    def store_version(self) -> int:
        """Goes up with every write to the store — compare it to save only changes."""
        return int(self.lib.lbs_store_version(self._h))

    # --- the loop --------------------------------------------------------------------

    def start(self, pins: int) -> None:
        """Start as a board does. `pins`: bit i is the level of input i."""
        self.lib.lbs_start(self._h, pins)

    def rx(self, frame: Frame) -> None:
        self.lib.lbs_rx(self._h, frame.can_id, frame.data, len(frame.data))

    def loop(self, pins: int) -> None:
        """One turn of the board's loop: the node, then every endpoint."""
        self.lib.lbs_loop(self._h, pins)

    def take_tx(self) -> list[Frame]:
        """Everything the node sent since the last call."""
        out: list[Frame] = []
        can_id = ctypes.c_uint32()
        length = ctypes.c_uint8()
        buf = ctypes.create_string_buffer(8)
        for i in range(self.lib.lbs_tx_count(self._h)):
            self.lib.lbs_tx_get(self._h, i, ctypes.byref(can_id), buf, ctypes.byref(length))
            out.append(Frame(int(can_id.value), buf.raw[: length.value]))
        self.lib.lbs_tx_clear(self._h)
        return out

    # --- what the device looks like --------------------------------------------------

    @property
    def input_count(self) -> int:
        return int(self.lib.lbs_input_count(self._h))

    @property
    def inputs_idle(self) -> int:
        """The pin levels with nothing pressed; flip bit i to press input i."""
        return int(self.lib.lbs_inputs_idle(self._h))

    @property
    def output_count(self) -> int:
        return int(self.lib.lbs_output_count(self._h))

    def driven(self, output: int = 0) -> int | None:
        """The last coil level output `output` drove (after `Invert`, where the endpoint has
        it), `None` before any."""
        level = int(self.lib.lbs_driven(self._h, output))
        return None if level < 0 else level

    def output_on(self, output: int) -> bool:
        """`OnOff` of the output — the state, before `Invert`."""
        return int(self.lib.lbs_output_on(self._h, output)) == 1

    # --- the service button and the status LED (zadania/zrobione/CR-05.md) -------------------

    def service_press(self) -> None:
        """The service button held long enough: the node's own factory reset."""
        self.lib.lbs_service_press(self._h)

    @property
    def state(self) -> str | None:
        """The last state the node reported for its LED — `"READY"`, `"NO_ADDRESS"`, … —
        or `None` before the first report."""
        name = self.lib.lbs_state_name(self.lib.lbs_state(self._h))
        return None if name is None else str(name.decode())

    @property
    def led(self) -> bool:
        """The status LED as the library's `tools/led.h` drives it, at the last loop."""
        return bool(self.lib.lbs_led(self._h))

    @property
    def state_reports(self) -> int:
        """How many times the node called its state watch — it is to call on change only."""
        return int(self.lib.lbs_state_reports(self._h))

    def set_bus(self, state: int) -> None:
        """Make the port answer `health` with `state` (`BUS_STATES`) from now on."""
        self.lib.lbs_set_bus(self._h, state)

    def set_errors(self, tec: int, rec: int) -> None:
        """Make the port answer `health` with these error counters from now on."""
        self.lib.lbs_set_errors(self._h, tec, rec)

    def set_reset_reason(self, reason: int) -> None:
        """What the platform says the last restart was — read at `start`."""
        self.lib.lbs_set_reset_reason(self._h, reason)

    # --- display fields (types/0x0022-display.md) -------------------------------------

    @property
    def display_fields(self) -> list[DisplayField]:
        fields = []
        for i in range(self.lib.lbs_display_count(self._h)):
            content = int(self.lib.lbs_display_content(self._h, i))
            quantity = int(self.lib.lbs_display_quantity(self._h, i))
            raw = ctypes.c_int32()
            has_value = bool(self.lib.lbs_display_value(self._h, i, ctypes.byref(raw)))
            buf = ctypes.create_string_buffer(256)
            n = int(self.lib.lbs_display_text(self._h, i, buf, 256))
            places = ctypes.create_string_buffer(32)
            g = int(self.lib.lbs_display_glass(self._h, i, places, 32))
            fields.append(
                DisplayField(
                    endpoint=int(self.lib.lbs_display_endpoint(self._h, i)),
                    content=CONTENT[content] if 0 <= content < len(CONTENT) else str(content),
                    quantity=None if quantity < 0 else quantity,
                    value=int(raw.value) if has_value else None,
                    # The field draws what it can and `?` for the rest (§5.3) — so does
                    # this, for bytes that are not UTF-8.
                    text=buf.raw[: max(n, 0)].decode("utf-8", errors="replace"),
                    glass=places.raw[:g].decode("ascii") if g >= 0 else None,
                )
            )
        return fields

    # --- sensors (types/0x0030-sense.md) -----------------------------------------------

    @property
    def sensors(self) -> list[Sensor]:
        out = []
        for i in range(self.lib.lbs_sense_count(self._h)):
            raw = ctypes.c_int32()
            has_value = bool(self.lib.lbs_sense_value(self._h, i, ctypes.byref(raw)))
            out.append(
                Sensor(
                    endpoint=int(self.lib.lbs_sense_endpoint(self._h, i)),
                    quantity=int(self.lib.lbs_sense_quantity(self._h, i)),
                    value=int(raw.value) if has_value else None,
                )
            )
        return out

    def sense(self, endpoint: int, raw: int | None) -> None:
        """A measurement on the sensor at `endpoint`, in the quantity's units on the wire;
        `None` is a failed reading. A report goes out when the change is worth one."""
        for i, sensor in enumerate(self.sensors):
            if sensor.endpoint == endpoint:
                if raw is None:
                    self.lib.lbs_sense_fail(self._h, i)
                else:
                    self.lib.lbs_sense_update(self._h, i, raw)
                return
        raise ValueError(f"no sensor at ep{endpoint}")

    # --- value inputs (types/0x0011-value-input.md) -------------------------------------

    @property
    def dials(self) -> list[Dial]:
        out = []
        for i in range(self.lib.lbs_dial_count(self._h)):
            lo, hi = ctypes.c_int32(), ctypes.c_int32()
            self.lib.lbs_dial_range(self._h, i, ctypes.byref(lo), ctypes.byref(hi))
            out.append(
                Dial(
                    endpoint=int(self.lib.lbs_dial_endpoint(self._h, i)),
                    value=int(self.lib.lbs_dial_value(self._h, i)),
                    min_value=int(lo.value),
                    max_value=int(hi.value),
                )
            )
        return out

    def turn(self, endpoint: int, detents: int) -> None:
        """Turn the `ValueInput` at `endpoint` by `detents` (negative: down). Collected
        as a board's interrupt collects them and handed to the library at the next loop —
        several before one loop are one step of several (zadania/devices/DEVICE-02.md N11)."""
        for i, dial in enumerate(self.dials):
            if dial.endpoint == endpoint:
                self.lib.lbs_dial_turn(self._h, i, detents)
                return
        raise ValueError(f"no value input at ep{endpoint}")

    @property
    def display_pending(self) -> bool:
        """The fields changed and the e-paper glass waits for its refresh."""
        return bool(self.lib.lbs_display_pending(self._h))

    @property
    def display_refreshes(self) -> int:
        """Refreshes of the e-paper glass, the clear at start included."""
        return int(self.lib.lbs_display_refreshes(self._h))

    @property
    def drawn(self) -> int:
        """Grows whenever a field changed what it shows — the view's cue to redraw."""
        return int(self.lib.lbs_display_drawn(self._h))

    @property
    def address(self) -> int:
        return int(self.lib.lbs_address(self._h))

    @property
    def short_id(self) -> int:
        return int(self.lib.lbs_short_id(self._h))
