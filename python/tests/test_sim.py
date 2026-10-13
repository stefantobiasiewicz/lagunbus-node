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
        "display",
        "epd",
        "env",
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


def test_display_fields_show_what_was_written() -> None:
    """Through the long write too: the text reaches `display_fields` whole, in UTF-8."""
    screen = SimDevice("display", UID)
    screen.store_put("address", bytes.fromhex("4200"))
    screen.start(0)
    drawn = screen.drawn
    assert [(f.endpoint, f.content, f.quantity) for f in screen.display_fields] == [
        (4, "NONE", 0),
        (5, "NONE", None),
        (6, "NONE", 1),
    ]
    text = "żółw".encode()
    value = bytes([len(text)]) + text
    screen.rx(Frame(0x0A108402, bytes([1]) + (2137).to_bytes(2, "little")))
    screen.rx(Frame(0x14108504, bytes([0x02, 0]) + value[:6]))
    screen.rx(Frame(0x14108504, bytes([0x82, 1]) + value[6:]))
    screen.loop(0)
    fields = screen.display_fields
    assert (fields[0].content, fields[0].value) == ("VALUE", 2137)
    assert (fields[1].content, fields[1].text) == ("TEXT", "żółw")
    assert screen.drawn == drawn + 2


def test_epd_shows_on_the_glass_what_the_board_would() -> None:
    """`epd` (zadania/zrobione/DEVICE-04.md, N3, N9): the fields drawn in the e-paper's
    places and refreshed as the board refreshes — the clear at start, a value at once while
    the glass shows none, the next change only after the gap."""
    screen = SimDevice("epd", UID)
    screen.store_put("address", bytes.fromhex("4200"))
    screen.start(0)
    screen.loop(0)

    def glass() -> list[str | None]:
        return [f.glass for f in screen.display_fields]

    def pending() -> bool:
        # A function, not the property: mypy would keep the property narrowed by the
        # first assert and call the later ones unreachable.
        return screen.display_pending

    assert [(f.endpoint, f.quantity) for f in screen.display_fields] == [(4, 0), (5, 1)]
    assert glass() == [" --.-", "--.-"], "cleared at start: empty fields are dashes"
    assert screen.display_refreshes == 1 and not pending()

    screen.rx(Frame(0x0A108402, bytes([1]) + (2137).to_bytes(2, "little", signed=True)))
    screen.set_clock(1000)
    screen.loop(0)
    assert glass() == [" 21.4", "--.-"], "at once — the glass had no number"

    screen.rx(Frame(0x0A108502, bytes([2]) + (4550).to_bytes(2, "little")))
    screen.set_clock(2000)
    screen.loop(0)
    assert glass() == [" 21.4", "--.-"] and pending(), "waits for the gap"

    screen.set_clock(61000)
    screen.loop(0)
    assert glass() == [" 21.4", "45.5"] and not pending()
    assert screen.display_refreshes == 3


def test_a_display_without_segments_has_no_glass() -> None:
    screen = SimDevice("display", UID)
    assert [f.glass for f in screen.display_fields] == [None, None, None]
    assert not screen.display_pending


def test_a_sensor_holds_what_it_was_handed() -> None:
    """`sense` is what `lagun-sim` calls from its keys; `None` is a failed reading.
    What goes onto the bus is `vectors/scenarios/sense.json`'s."""
    env = SimDevice("env", UID)
    env.store_put("address", bytes.fromhex("4200"))
    env.start(0)
    absent = [(4, 0, None), (5, 1, None), (6, 2, None)]
    assert env.sensors == absent, "absent before any measurement"
    env.sense(4, 2150)
    env.sense(5, 4500)
    assert [s.value for s in env.sensors] == [2150, 4500, None]
    env.sense(4, None)
    assert env.sensors[0].value is None
    with pytest.raises(ValueError, match="ep7"):
        env.sense(7, 1)


def test_sensors_can_be_listed_before_the_start() -> None:
    """`lagun-sim --headless` prints them before it powers the node on — the endpoint has
    to be there from the device's definition, not from `lb_sense_init`."""
    env = SimDevice("env", UID)
    assert [(s.endpoint, s.quantity) for s in env.sensors] == [(4, 0), (5, 1), (6, 2)]
