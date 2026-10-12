"""`lagunbus_node.sim` — what the simulator uses beyond what a scenario checks: the
catalogue, the whole store (saved between runs), the version that says when to save,
and how the device looks from outside (inputs, outputs)."""

from __future__ import annotations

import pytest

from lagunbus_node.frame import Frame
from lagunbus_node.sim import SimDevice, devices

UID = bytes.fromhex("A1B2C3D400010203")


def test_the_catalogue_lists_every_device_with_a_description() -> None:
    names = [d.name for d in devices()]
    assert names == [
        "key",
        "bell",
        "key_fixed",
        "panel3",
        "key3",
        "output",
        "output2",
        "output8",
        "out8",
    ]
    assert all(d.about for d in devices())


def test_key_and_output_come_in_sizes_and_the_rest_as_they_are() -> None:
    sizes = {d.name: d.max_channels for d in devices()}
    assert sizes["key"] == sizes["output"] == 8
    assert sizes["bell"] == sizes["panel3"] == sizes["output8"] == 0
    assert SimDevice("key", UID).input_count == 1, "one key by default — the board"
    assert SimDevice("key", UID, channels=4).input_count == 4
    assert SimDevice("output", UID, channels=8).output_count == 8


@pytest.mark.parametrize(("name", "channels"), [("key", 9), ("output8", 2), ("bell", 1)])
def test_a_size_a_device_does_not_come_in_is_refused(name: str, channels: int) -> None:
    with pytest.raises(ValueError, match="channels"):
        SimDevice(name, UID, channels=channels)


def test_an_unknown_device_is_refused_with_the_list() -> None:
    with pytest.raises(ValueError, match="output8"):
        SimDevice("toaster", UID)


def test_inputs_and_outputs() -> None:
    panel = SimDevice("panel3", UID)
    assert (panel.input_count, panel.output_count) == (3, 0)
    assert panel.inputs_idle == 0b111, "a LOW pin is a press, so idle is all HIGH"
    relays = SimDevice("output8", UID)
    assert (relays.input_count, relays.output_count) == (0, 8)
    assert relays.inputs_idle == 0


def test_the_store_comes_out_whole_and_says_when_it_changed() -> None:
    """The simulator saves the store to files only when the version moves."""
    device = SimDevice("output2", UID)
    device.store_put("address", b"\x42\x00")
    device.start(0)
    version = device.store_version
    assert device.store_items()["address"] == b"\x42\x00"

    device.rx(Frame(0x06108501, b"\x01"))  # On, endpoint 5
    device.loop(0)
    assert device.output_on(1) and not device.output_on(0)
    assert device.store_version > version
    assert device.store_items()["output5"] == b"\x01"


def test_output_on_is_the_state_and_driven_the_coil() -> None:
    """`output` has no Invert (zadania/zrobione/CR-04.md, N7): its LOW-active relay is the
    application's affair, so the library drives the coil state — the same as `OnOff`."""
    device = SimDevice("output", UID)
    device.store_put("address", b"\x42\x00")
    device.start(0)
    assert (device.output_on(0), device.driven(0)) == (False, 0)
    device.rx(Frame(0x06108401, b"\x01"))
    device.loop(0)
    assert (device.output_on(0), device.driven(0)) == (True, 1)
