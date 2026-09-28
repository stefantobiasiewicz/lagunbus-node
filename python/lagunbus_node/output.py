"""A driven output — type 0x0020 Output, the variant without dimming.

The Python twin of `c/lagunbus/src/output.c`. The logic lives here, driving the pin
lives in the device — a callback, as in C.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Final

from lagunbus_node import values
from lagunbus_node.endpoint import Endpoint
from lagunbus_node.node import U32, Node
from lagunbus_node.registry import CommonAttribute, ResultCode
from lagunbus_node.values import ValueType

ON_OFF: Final = 0x00
POWER_ON_BEHAVIOUR: Final = 0x13
INVERT: Final = 0x14
AUTO_OFF_TIME: Final = 0x16
PURPOSE: Final = 0x17

CMD_OFF: Final = 0x00
CMD_ON: Final = 0x01
CMD_TOGGLE: Final = 0x02

POWER_ON_OFF: Final = 0
POWER_ON_ON: Final = 1
POWER_ON_RESTORE: Final = 2

KEY_OUTPUT: Final = "output"
"""The key in persistent storage — the same string as in output.c."""

Drive = Callable[[bool], None]
"""Set the physical output. `Invert` has already been applied — this is the pin level."""


class Output:
    def __init__(self, node: Node, endpoint: Endpoint, drive: Drive | None = None) -> None:
        """Prepare the output and apply `PowerOnBehaviour`. Call it AFTER
        `node.start()` — `RESTORE` reads the persistent store."""
        self.node = node
        self.endpoint = endpoint
        self.drive = drive
        self.lit_ms = 0
        self.last_report_ms = 0
        self.report_pending = False

        behaviour = endpoint.setting(POWER_ON_BEHAVIOUR, POWER_ON_OFF)
        target = behaviour == POWER_ON_ON
        if behaviour == POWER_ON_RESTORE:
            stored = node.platform.load(KEY_OUTPUT, 1)
            if stored is not None:
                target = stored[0] != 0

        # The pin is ALWAYS driven: after a restart the hardware is in an undefined state.
        on_off = endpoint.attr(ON_OFF)
        if on_off is not None:
            on_off.value = 1 if target else 0
        self._drive(target)
        self.lit_ms = node.now()

    def is_on(self) -> bool:
        return self.endpoint.setting(ON_OFF, 0) != 0

    def _drive(self, state: bool) -> None:
        invert = self.endpoint.setting(INVERT, 0) != 0
        if self.drive is not None:
            self.drive((not state) if invert else state)

    def _report(self, now_ms: int) -> None:
        """A report with a rate limit (`ReportMinGap`) that never loses the final state."""
        min_gap = self.endpoint.setting(CommonAttribute.ReportMinGap, 0)
        if (
            min_gap > 0
            and self.last_report_ms != 0
            and (now_ms - self.last_report_ms) & U32 < min_gap
        ):
            self.report_pending = True
            return
        value = values.encode(ValueType.BOOL, 1 if self.is_on() else 0)
        self.node.report(self.endpoint.number, ON_OFF, value)
        self.last_report_ms = 1 if now_ms == 0 else now_ms
        self.report_pending = False

    def _remember(self) -> None:
        """Store the state, but only under `RESTORE` — a hallway relay switches a dozen
        times a day for twenty years."""
        if self.endpoint.setting(POWER_ON_BEHAVIOUR, POWER_ON_OFF) != POWER_ON_RESTORE:
            return
        self.node.platform.store(KEY_OUTPUT, bytes([1 if self.is_on() else 0]))

    def set(self, fresh: bool) -> None:
        on_off = self.endpoint.attr(ON_OFF)
        # `on_off->value.value != 0` in C — an absent value carries 0 there.
        if on_off is None or ((on_off.value or 0) != 0) == fresh:
            return
        on_off.value = 1 if fresh else 0
        self._drive(fresh)
        self.lit_ms = self.node.now()
        self._remember()
        self._report(self.lit_ms)

    def command(self, element: int) -> int:
        if element == CMD_OFF:
            self.set(False)
        elif element == CMD_ON:
            self.set(True)
        elif element == CMD_TOGGLE:
            self.set(not self.is_on())
        else:
            return ResultCode.UNSUPPORTED_ELEMENT  # SetLevel without LEVEL, and so on
        return ResultCode.OK

    def tick(self, now_ms: int) -> None:
        """Counts down `AutoOffTime` and releases held-back reports."""
        now = now_ms & U32
        auto_off = self.endpoint.setting(AUTO_OFF_TIME, 0)
        if auto_off > 0 and self.is_on() and (now - self.lit_ms) & U32 >= (auto_off * 1000) & U32:
            self.set(False)
        if self.report_pending:
            self._report(now)
