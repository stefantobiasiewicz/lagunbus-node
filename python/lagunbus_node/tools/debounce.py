"""Contact bounce filter — a tool, not a type.

The Python twin of `c/lagunbus/include/lagunbus/tools/debounce.h`, which says why it exists:
bounce belongs to how an input is built, so the device decides whether to use this and
with what window. Polarity is the device's too — pass in "is the finger on it".
"""

from __future__ import annotations

from lagunbus_node.node import U32


class Debounce:
    def __init__(self, level: bool, now_ms: int) -> None:
        """Start from `level` as already stable — a start is not a change."""
        self.stable = level
        self.last_raw = level
        self.changed_ms = now_ms & U32

    def update(self, level: bool, now_ms: int, window_ms: int) -> bool:
        """One step; returns the stable level. A change is accepted once the reading
        has held for `window_ms` since its LAST change."""
        now = now_ms & U32
        if level != self.last_raw:
            self.last_raw = level
            self.changed_ms = now
        if self.last_raw != self.stable and (now - self.changed_ms) & U32 >= window_ms:
            self.stable = self.last_raw
        return self.stable
