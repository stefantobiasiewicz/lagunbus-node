"""The two device profiles, built in Python — the twins of those in `c/tests/harness.c`.

Both copy `devices/apps/key` and `devices/apps/output` attribute for attribute, so the
scenarios exercise the configuration that actually goes onto a board.
"""

from __future__ import annotations

from lagunbus_node.endpoint import PERSIST, RO, RW, Attr, Endpoint
from lagunbus_node.node import Identity
from lagunbus_node.registry import CommonAttribute, EndpointType, NodeClass
from lagunbus_node.types import key, output
from lagunbus_node.values import ValueType

P = RW | PERSIST


def identity(device_type: int, uid: bytes) -> Identity:
    return Identity(
        uid=uid,
        node_class=NodeClass.CONTROL,
        device_type=device_type,
        vendor_id=0,
        firmware=0x00000100,
        hardware=1,
        protocol=1,
    )


KEY_DEBOUNCE_MS = 15
"""The key board's own bounce window — a device decision, as in `key_board()` in
harness.c. The board's key shorts to ground, so a LOW pin is a press."""


def key_endpoint() -> Endpoint:
    """Everything: all three features, every timing tunable from the hub."""
    return Endpoint(
        number=4,
        type_id=EndpointType.KeyInput,
        revision=1,
        features=key.F_MULTICLICK | key.F_HOLD | key.F_RAW,
        channel=1,
        attrs=[
            key.attr_press_count(),
            key.attr_event_mode(),
            key.attr_multi_click_window(350),
            key.attr_max_clicks(3),
            key.attr_hold_enabled(),
            key.attr_hold_threshold(500),
            key.attr_hold_tick_interval(200),
        ],
        on_check=key.check,
        feature_of=key.feature_of,
    )


def bell_endpoint() -> Endpoint:
    """A doorbell: no features, no attributes — `profile_bell` in harness.c."""
    return Endpoint(
        number=4,
        type_id=EndpointType.KeyInput,
        revision=1,
        features=0,
        channel=1,
        attrs=[],
        on_check=key.check,
        feature_of=key.feature_of,
    )


def key_fixed_endpoint() -> Endpoint:
    """HOLD and MULTICLICK with the device's own timings — `profile_key_fixed`."""
    return Endpoint(
        number=4,
        type_id=EndpointType.KeyInput,
        revision=1,
        features=key.F_HOLD | key.F_MULTICLICK,
        channel=1,
        attrs=[key.attr_event_mode(), key.attr_hold_enabled()],
        on_check=key.check,
        feature_of=key.feature_of,
    )


KEY_FIXED_CONFIG = key.KeyConfig(
    multi_click_window_ms=250, max_clicks=2, hold_threshold_ms=800, hold_tick_ms=400
)


def output_endpoint() -> Endpoint:
    return Endpoint(
        number=4,
        type_id=EndpointType.Output,
        revision=1,
        features=0,
        channel=1,
        attrs=[
            Attr(output.ON_OFF, ValueType.BOOL, RO, 0),
            Attr(output.POWER_ON_BEHAVIOUR, ValueType.ENUM8, P, output.POWER_ON_OFF),
            Attr(output.INVERT, ValueType.BOOL, P, 1),
            Attr(output.AUTO_OFF_TIME, ValueType.U16, P, 0),
            Attr(output.PURPOSE, ValueType.ENUM8, P, 1),
            Attr(CommonAttribute.ReportMinGap, ValueType.U16, P, 200),
        ],
    )
