"""Momentary key — type 0x0010 KeyInput. **Tools, not a key.**

The Python twin of `c/lagunbus/include/lagunbus/types/key.h`, which lists the tools and says
why they are split this way (zadania/CR-01.md, N6 and K1–K5): attribute rows, the
endpoint hooks, the event state machine fed an already-filtered "pressed", and the
event encoders for a device with its own logic. Bounce and polarity are the device's.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Final

from lagunbus_node import values
from lagunbus_node.endpoint import PERSIST, RO, RW, Attr, Endpoint
from lagunbus_node.node import U32, Node
from lagunbus_node.registry import ResultCode
from lagunbus_node.values import Value, ValueType

PRESS_COUNT: Final = 0x00
EVENT_MODE: Final = 0x11
MULTI_CLICK_WINDOW: Final = 0x12
MAX_CLICKS: Final = 0x13
HOLD_ENABLED: Final = 0x14
HOLD_THRESHOLD: Final = 0x15
HOLD_TICK_INTERVAL: Final = 0x16

EV_DOWN: Final = 0x01
EV_UP: Final = 0x02
EV_CLICK: Final = 0x03
EV_HOLD_START: Final = 0x05
EV_HOLD_TICK: Final = 0x06
EV_HOLD_END: Final = 0x07

MODE_CLICK: Final = 0
MODE_MULTICLICK: Final = 1
MODE_RAW: Final = 2

F_MULTICLICK: Final = 0x0001
F_HOLD: Final = 0x0002
F_RAW: Final = 0x0004
"""`KeyInput` features (types/0x0010-key-input.md §1)."""

HOLD_TICK_LIMIT_MS: Final = 60_000
"""A key held this long stops sending HoldTick and waits for the release."""

_EVENT_NAMES: Final = {
    EV_DOWN: "Down",
    EV_UP: "Up",
    EV_CLICK: "Click",
    EV_HOLD_START: "HoldStart",
    EV_HOLD_TICK: "HoldTick",
    EV_HOLD_END: "HoldEnd",
}

_P: Final = RW | PERSIST

# --- attribute rows -------------------------------------------------------------------
# One per attribute the device EXPOSES — `LB_KEY_ATTR_…` in key.h.


def attr_press_count() -> Attr:
    return Attr(PRESS_COUNT, ValueType.U32, RO, 0)


def attr_event_mode() -> Attr:
    return Attr(EVENT_MODE, ValueType.ENUM8, _P, MODE_CLICK)


def attr_multi_click_window(ms: int) -> Attr:
    return Attr(MULTI_CLICK_WINDOW, ValueType.U16, _P, ms)


def attr_max_clicks(n: int) -> Attr:
    return Attr(MAX_CLICKS, ValueType.U8, _P, n)


def attr_hold_enabled() -> Attr:
    return Attr(HOLD_ENABLED, ValueType.BOOL, _P, 1)


def attr_hold_threshold(ms: int) -> Attr:
    return Attr(HOLD_THRESHOLD, ValueType.U16, _P, ms)


def attr_hold_tick_interval(ms: int) -> Attr:
    return Attr(HOLD_TICK_INTERVAL, ValueType.U16, _P, ms)


# --- endpoint hooks -------------------------------------------------------------------

_RANGES: Final = {
    MULTI_CLICK_WINDOW: (150, 800),
    MAX_CLICKS: (2, 5),
    HOLD_THRESHOLD: (200, 3000),
    HOLD_TICK_INTERVAL: (50, 1000),
}
"""The type's table (types/0x0010-key-input.md §2.2) — see `lb_key_check`."""

_MODE_FEATURE: Final = {MODE_CLICK: 0, MODE_MULTICLICK: F_MULTICLICK, MODE_RAW: F_RAW}


def check(endpoint: Endpoint, element: int, value: Value) -> int:
    """`on_check` for a `KeyInput` endpoint — `lb_key_check` in key.c."""
    if element == EVENT_MODE:
        needs = _MODE_FEATURE.get(value if isinstance(value, int) else -1)
        if needs is None or (needs and not endpoint.features & needs):
            return ResultCode.INVALID_VALUE
        return ResultCode.OK
    bounds = _RANGES.get(element)
    if bounds is not None and isinstance(value, int) and not bounds[0] <= value <= bounds[1]:
        return ResultCode.INVALID_VALUE
    return ResultCode.OK


def feature_of(element: int) -> int:
    """`feature_of` for a `KeyInput` endpoint — `lb_key_feature_of` in key.c."""
    if element == EVENT_MODE:
        return F_MULTICLICK | F_RAW
    if element in (MULTI_CLICK_WINDOW, MAX_CLICKS):
        return F_MULTICLICK
    if element in (HOLD_ENABLED, HOLD_THRESHOLD, HOLD_TICK_INTERVAL):
        return F_HOLD
    return 0


# --- events ---------------------------------------------------------------------------


def _duration(ms: int) -> bytes:
    # Saturating is more honest than truncating: "a very long time".
    return values.encode(ValueType.U16, min(ms, 0xFFFF))


def emit_down(node: Node, endpoint: int) -> bool:
    return node.emit(endpoint, EV_DOWN, b"")


def emit_up(node: Node, endpoint: int, held_ms: int) -> bool:
    return node.emit(endpoint, EV_UP, _duration(held_ms))


def emit_click(node: Node, endpoint: int, count: int) -> bool:
    return node.emit(endpoint, EV_CLICK, bytes([count]))


def emit_hold_start(node: Node, endpoint: int) -> bool:
    return node.emit(endpoint, EV_HOLD_START, b"")


def emit_hold_tick(node: Node, endpoint: int, held_ms: int) -> bool:
    return node.emit(endpoint, EV_HOLD_TICK, _duration(held_ms))


def emit_hold_end(node: Node, endpoint: int, held_ms: int) -> bool:
    return node.emit(endpoint, EV_HOLD_END, _duration(held_ms))


def event_name(event: int) -> str:
    """Event name for the log; `?` for an unknown one."""
    return _EVENT_NAMES.get(event, "?")


# --- the state machine ----------------------------------------------------------------


@dataclass(frozen=True)
class KeyConfig:
    """The device's own decisions — `lb_key_config_t`. An attribute the endpoint
    exposes wins; this applies when it does not. Defaults: the type's recommended
    values."""

    multi_click_window_ms: int = 350
    max_clicks: int = 3
    hold_threshold_ms: int = 500
    hold_tick_ms: int = 200
    silent_start_press: bool = True
    """A press already on at start produces nothing, its release included — a choice of
    this implementation, not a rule of the type (CR-01 N1b, K4)."""


class Key:
    def __init__(self, endpoint: Endpoint, config: KeyConfig, pressed: bool, now_ms: int) -> None:
        """`pressed` is the filtered input now."""
        self.endpoint = endpoint
        self.config = config
        self.pressed = pressed
        self.unseen_press = pressed and config.silent_start_press
        self.pressed_ms = now_ms & U32
        self.hold_active = False
        self.last_tick_ms = 0
        self.clicks = 0
        self.window_until_ms = 0
        self.window_open = False

    def _setting(self, element: int, own: int) -> int:
        return self.endpoint.setting(element, own)

    def _flush_clicks(self, node: Node) -> None:
        if not self.window_open or self.clicks == 0:
            return
        emit_click(node, self.endpoint.number, self.clicks)
        self.clicks = 0
        self.window_open = False

    def _count_press(self) -> None:
        counter = self.endpoint.attr(PRESS_COUNT)
        if counter is not None and counter.value is not None:
            # `value.value++` on an int32_t in key.c — it wraps at 2**31 there, which
            # a key does not live long enough to reach.
            counter.value += 1

    def update(self, node: Node, pressed: bool, now_ms: int) -> None:
        """One step. `pressed` is the FILTERED input — see `lb_key_update`."""
        cfg = self.config
        features = self.endpoint.features
        number = self.endpoint.number
        now = now_ms & U32

        mode = self._setting(EVENT_MODE, MODE_CLICK) & 0xFF
        window = self._setting(MULTI_CLICK_WINDOW, cfg.multi_click_window_ms)
        max_clicks = self._setting(MAX_CLICKS, cfg.max_clicks) & 0xFF
        hold_allowed = bool(features & F_HOLD) and self._setting(HOLD_ENABLED, 1) != 0
        hold_threshold = self._setting(HOLD_THRESHOLD, cfg.hold_threshold_ms)
        tick = self._setting(HOLD_TICK_INTERVAL, cfg.hold_tick_ms)

        pressed_now = pressed and not self.pressed
        released_now = not pressed and self.pressed
        self.pressed = pressed

        # --- edges: Down and Up belong to RAW alone — see key.c ------------------
        if pressed_now:
            self.pressed_ms = now
            self.hold_active = False
            self._count_press()
            if mode == MODE_RAW:
                emit_down(node, number)
        elif released_now:
            elapsed = (now - self.pressed_ms) & U32
            if self.unseen_press:
                self.unseen_press = False
            elif self.hold_active:
                # A release is exactly ONE event; after a hold it is HoldEnd — see key.c.
                emit_hold_end(node, number, elapsed)
                self.hold_active = False
            elif mode == MODE_RAW:
                emit_up(node, number, elapsed)
            elif mode == MODE_CLICK:
                emit_click(node, number, 1)
            elif mode == MODE_MULTICLICK:
                if self.clicks < 0xFF:
                    self.clicks += 1
                self.window_open = True
                self.window_until_ms = (now + window) & U32
                if self.clicks >= max_clicks:
                    self._flush_clicks(node)

        # --- hold: every mode, only with HOLD; cancels a sequence as it starts ------
        if self.pressed and hold_allowed and not self.unseen_press:
            active = (now - self.pressed_ms) & U32
            if not self.hold_active and active >= hold_threshold:
                self.hold_active = True
                self.last_tick_ms = now
                self.clicks = 0
                self.window_open = False
                emit_hold_start(node, number)
            elif (
                self.hold_active
                and tick > 0
                and active < HOLD_TICK_LIMIT_MS
                and (now - self.last_tick_ms) & U32 >= tick
            ):
                self.last_tick_ms = now
                emit_hold_tick(node, number, active)

        # --- the multi-click window ----------------------------------------------
        if self.window_open and not self.pressed and now >= self.window_until_ms:
            self._flush_clicks(node)
