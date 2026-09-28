"""LagunBus identifiers — A GENERATED FILE, do not edit it by hand.

Source:    the .yaml files in lagunbus-spec/registry/
Generator: tools/generate_registry.py

The twin of `c/lagunbus/include/lagunbus/registry.h`, from the same generator.
"""

from __future__ import annotations

from enum import IntEnum


class MessageKind(IntEnum):
    """The KIND field. THE NUMBER IS THE PRIORITY (05-mapowanie-can.md §2)."""

    ALARM = 0x00
    EVENT = 0x01
    COMMAND = 0x03
    GROUP_COMMAND = 0x04
    WRITE = 0x05
    READ = 0x06
    RESPONSE = 0x07
    REPORT = 0x08
    LONG_RESPONSE = 0x09
    LONG_WRITE = 0x0A
    QUERY = 0x0B
    HUB_RESPONSE = 0x0C
    JOIN = 0x0F


class ResultCode(IntEnum):
    """Result codes of a reply, the RESULT field, 5 bits (04-interakcje.md §8)."""

    OK = 0x00
    UNSUPPORTED_ENDPOINT = 0x01
    UNSUPPORTED_ELEMENT = 0x02
    UNSUPPORTED_FEATURE = 0x03
    READ_ONLY = 0x04
    WRITE_ONLY = 0x05
    INVALID_VALUE = 0x06
    INVALID_STATE = 0x07
    BUSY = 0x08
    FAILURE = 0x09
    UNAVAILABLE = 0x0C
    SEGMENTED = 0x0B
    INSUFFICIENT_SPACE = 0x0A


class NodeClass(IntEnum):
    """The NodeClass field in an ANNOUNCEMENT. Chooses the address block (03-adresacja.md §2.3)."""

    UNSPECIFIED = 0x00
    CONTROL = 0x01
    SENSOR = 0x02


class EndpointType(IntEnum):
    """Endpoint types (registry/endpoint-types.yaml)."""

    Device = 0x01
    Diagnostics = 0x02
    Scenes = 0x03
    Groups = 0x04
    KeyInput = 0x10
    ValueInput = 0x11
    Contact = 0x12
    ToggleInput = 0x13
    Output = 0x20
    Cover = 0x21
    Sense = 0x30
    Thermostat = 0x40
    Meter = 0x31


class CommonAttribute(IntEnum):
    """Common attributes, range 0xF0-0xFF (01-model-danych.md §4)."""

    TypeId = 0xF0
    TypeRevision = 0xF1
    Features = 0xF2
    Enabled = 0xF3
    Channel = 0xF5
    QueryEnabled = 0xF6
    ReportInterval = 0xF8
    ReportMinGap = 0xF9
    ReportHysteresis = 0xFA
