"""Momentary key — type 0x0010 KeyInput.

The Python twin of `c/lagunbus/src/key.c`. All of the logic is in the library, not in
the device, for the reason given in key.h: bounce, the multi-click window and hold are
time-dependent, and the only way to check them is to drive the clock from a test. How
much of this belongs in the library at all is zadania/CR-01.md.

The settings are not fields here — they are read from the endpoint's attributes on
every step, because the hub owns them and changes them on the fly.
"""

from __future__ import annotations

from typing import Final

from lagunbus_node import values
from lagunbus_node.endpoint import Endpoint
from lagunbus_node.node import U32, Node
from lagunbus_node.values import ValueType

PRESS_COUNT: Final = 0x00
DEBOUNCE_TIME: Final = 0x10
EVENT_MODE: Final = 0x11
MULTI_CLICK_WINDOW: Final = 0x12
MAX_CLICKS: Final = 0x13
HOLD_ENABLED: Final = 0x14
HOLD_THRESHOLD: Final = 0x15
HOLD_TICK_INTERVAL: Final = 0x16
INVERT: Final = 0x17

EV_DOWN: Final = 0x01
EV_UP: Final = 0x02
EV_CLICK: Final = 0x03
EV_HOLD_START: Final = 0x05
EV_HOLD_TICK: Final = 0x06
EV_HOLD_END: Final = 0x07

MODE_CLICK: Final = 0
MODE_MULTICLICK: Final = 1
MODE_RAW: Final = 2

_EVENT_NAMES: Final = {
    EV_DOWN: "Down",
    EV_UP: "Up",
    EV_CLICK: "Click",
    EV_HOLD_START: "HoldStart",
    EV_HOLD_TICK: "HoldTick",
    EV_HOLD_END: "HoldEnd",
}


def event_name(event: int) -> str:
    """Event name for the log; `?` for an unknown one."""
    return _EVENT_NAMES.get(event, "?")


class Key:
    def __init__(self, endpoint: Endpoint, level: bool, now_ms: int) -> None:
        """The initial state comes from `level`, so that a start with the key held
        down does not look like a press."""
        self.endpoint = endpoint
        invert = endpoint.setting(INVERT, 0) != 0
        self.stable = (not level) if invert else level
        self.last_raw = self.stable
        self.changed_ms = now_ms & U32

        self.pressed_ms = 0
        self.hold_active = False
        self.last_tick_ms = 0

        self.clicks = 0
        self.window_until_ms = 0
        self.window_open = False

    def _emit(self, node: Node, event: int, params: bytes = b"") -> None:
        node.emit(self.endpoint.number, event, params)

    def _emit_u16(self, node: Node, event: int, value: int) -> None:
        # Durations travel as u16; saturating is more honest than truncating.
        self._emit(node, event, values.encode(ValueType.U16, min(value, 0xFFFF)))

    def _flush_clicks(self, node: Node) -> None:
        if not self.window_open or self.clicks == 0:
            return
        self._emit(node, EV_CLICK, bytes([self.clicks]))
        self.clicks = 0
        self.window_open = False

    def _count_press(self) -> None:
        counter = self.endpoint.attr(PRESS_COUNT)
        if counter is not None and counter.value is not None:
            # `value.value++` on an int32_t in key.c — it wraps at 2**31 there, which
            # a key does not live long enough to reach.
            counter.value += 1

    def update(self, node: Node, level: bool, now_ms: int) -> None:
        """One step. `level` is the RAW pin reading; `Invert` is applied here."""
        ep = self.endpoint
        now = now_ms & U32
        invert = ep.setting(INVERT, 0) != 0
        level_in = (not level) if invert else level

        debounce = ep.setting(DEBOUNCE_TIME, 15)
        mode = ep.setting(EVENT_MODE, MODE_CLICK) & 0xFF
        window = ep.setting(MULTI_CLICK_WINDOW, 350)
        max_clicks = ep.setting(MAX_CLICKS, 3) & 0xFF
        hold_allowed = ep.setting(HOLD_ENABLED, 1) != 0
        hold_threshold = ep.setting(HOLD_THRESHOLD, 500)
        tick = ep.setting(HOLD_TICK_INTERVAL, 200)

        # --- contact bounce ------------------------------------------------------
        if level_in != self.last_raw:
            self.last_raw = level_in
            self.changed_ms = now
        changed = False
        if self.last_raw != self.stable and (now - self.changed_ms) & U32 >= debounce:
            self.stable = self.last_raw
            changed = True

        # --- edges ---------------------------------------------------------------
        if changed and self.stable:
            self.pressed_ms = now
            self.hold_active = False
            self._count_press()
            self._emit(node, EV_DOWN)
        elif changed and not self.stable:
            elapsed = (now - self.pressed_ms) & U32
            self._emit_u16(node, EV_UP, elapsed)

            if self.hold_active:
                # A hold is NOT a click — see key.c.
                self._emit_u16(node, EV_HOLD_END, elapsed)
                self.hold_active = False
                self.clicks = 0
                self.window_open = False
            elif mode == MODE_CLICK:
                self._emit(node, EV_CLICK, bytes([1]))
            elif mode == MODE_MULTICLICK:
                if self.clicks < 0xFF:
                    self.clicks += 1
                self.window_open = True
                self.window_until_ms = (now + window) & U32
                if self.clicks >= max_clicks:
                    self._flush_clicks(node)
            # MODE_RAW: Down and Up are enough.

        # --- hold ----------------------------------------------------------------
        if self.stable and hold_allowed and mode != MODE_RAW:
            active = (now - self.pressed_ms) & U32
            if not self.hold_active and active >= hold_threshold:
                self.hold_active = True
                self.last_tick_ms = now
                self._emit(node, EV_HOLD_START)
            elif self.hold_active and tick > 0 and (now - self.last_tick_ms) & U32 >= tick:
                self.last_tick_ms = now
                self._emit_u16(node, EV_HOLD_TICK, active)

        # --- the multi-click window ---------------------------------------------
        if self.window_open and not self.stable and now >= self.window_until_ms:
            self._flush_clicks(node)
