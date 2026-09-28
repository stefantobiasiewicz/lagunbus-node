"""The two device profiles, built in Python — the twins of those in `c/tests/harness.c`.

Both copy `devices/apps/key` and `devices/apps/output` attribute for attribute, so the
scenarios exercise the configuration that actually goes onto a board.
"""

from __future__ import annotations

from lagunbus_node import key, output
from lagunbus_node.endpoint import PERSIST, RO, RW, Attr, Endpoint
from lagunbus_node.node import Identity
from lagunbus_node.registry import CommonAttribute, EndpointType, NodeClass
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


def key_endpoint() -> Endpoint:
    return Endpoint(
        number=4,
        type_id=EndpointType.KeyInput,
        revision=1,
        features=0,
        channel=1,
        attrs=[
            Attr(key.PRESS_COUNT, ValueType.U32, RO, 0),
            Attr(key.DEBOUNCE_TIME, ValueType.U16, P, 15),
            Attr(key.EVENT_MODE, ValueType.ENUM8, P, key.MODE_CLICK),
            Attr(key.MULTI_CLICK_WINDOW, ValueType.U16, P, 350),
            Attr(key.MAX_CLICKS, ValueType.U8, P, 3),
            Attr(key.HOLD_ENABLED, ValueType.BOOL, P, 1),
            Attr(key.HOLD_THRESHOLD, ValueType.U16, P, 500),
            Attr(key.HOLD_TICK_INTERVAL, ValueType.U16, P, 200),
            Attr(key.INVERT, ValueType.BOOL, P, 1),
        ],
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
