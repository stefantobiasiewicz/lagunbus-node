"""`vectors/codec.json` through both libraries."""

from __future__ import annotations

import json
from typing import Any

import pytest

from lagunbus_node.registry import MessageKind, ResultCode
from lagunbus_node.values import ValueType
from tests.conformance.conftest import VECTORS
from tests.conformance.implementations import CCodec, Codec

DATA: dict[str, Any] = json.loads((VECTORS / "codec.json").read_text(encoding="utf-8"))


def num(text: str | int) -> int:
    return text if isinstance(text, int) else int(text, 0)


def kind(value: str | int) -> int:
    return value if isinstance(value, int) else MessageKind[value]


def ids(section: str) -> list[str]:
    return [str(entry.get("note", i)) for i, entry in enumerate(DATA[section])]


@pytest.mark.parametrize("v", DATA["identifiers"], ids=ids("identifiers"))
def test_identifier(codec: Codec, v: dict[str, Any]) -> None:
    fields = (kind(v["kind"]), num(v["address"]), v["endpoint"], num(v["element"]))
    assert codec.encode_id(*fields) == num(v["can_id"])
    assert codec.decode_id(num(v["can_id"])) == fields


@pytest.mark.parametrize("v", DATA["identifier_refusals"], ids=ids("identifier_refusals"))
def test_identifier_refused(codec: Codec, v: dict[str, Any]) -> None:
    assert codec.encode_id(v["kind"], num(v["address"]), v["endpoint"], num(v["element"])) is None


@pytest.mark.parametrize("v", DATA["joins"], ids=ids("joins"))
def test_join(codec: Codec, v: dict[str, Any]) -> None:
    assert codec.join_id(num(v["short_id"]), v["announce"]) == num(v["can_id"])


@pytest.mark.parametrize("v", DATA["headers"])
def test_header(codec: Codec, v: dict[str, Any]) -> None:
    result = ResultCode[v["result"]]
    assert codec.encode_header(v["tid"], result) == num(v["byte"])
    assert codec.decode_header(num(v["byte"])) == (v["tid"], result)


@pytest.mark.parametrize("v", DATA["values"], ids=ids("values"))
def test_value(codec: Codec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    raw = bytes.fromhex(v["bytes"])
    assert codec.encode(t, v["value"]) == raw
    assert codec.decode(t, raw) == (v["value"],)


@pytest.mark.parametrize("v", DATA["value_refusals"], ids=ids("value_refusals"))
def test_value_refused(codec: Codec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    if "bytes" in v:
        assert codec.decode(t, bytes.fromhex(v["bytes"])) is None
    else:
        assert codec.encode(t, v["value"]) is None


@pytest.mark.parametrize("v", DATA["ranges"], ids=[r["type"] for r in DATA["ranges"]])
def test_range(codec: Codec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    assert (codec.lo(t), codec.hi(t)) == (v["lo"], v["hi"])


@pytest.mark.parametrize("v", DATA["in_range"])
def test_in_range(codec: Codec, v: dict[str, Any]) -> None:
    assert codec.in_range(ValueType(v["type"]), v["value"]) is v["fits"]


@pytest.mark.parametrize("v", DATA["short_ids"], ids=ids("short_ids"))
def test_short_id(codec: Codec, v: dict[str, Any]) -> None:
    assert codec.short_id(bytes.fromhex(v["uid"])) == num(v["short_id"])


@pytest.mark.parametrize("v", DATA["blobs"])
def test_blob(codec: Codec, v: dict[str, Any]) -> None:
    data, raw = bytes.fromhex(v["data"]), bytes.fromhex(v["bytes"])
    assert codec.encode_blob(data) == raw
    assert codec.decode_blob(raw) == data


@pytest.mark.parametrize("v", DATA["blob_refusals"], ids=ids("blob_refusals"))
def test_blob_refused(codec: Codec, v: dict[str, Any]) -> None:
    assert codec.decode_blob(bytes.fromhex(v["bytes"])) is None


def test_value_types_are_declared_in_the_order_of_the_c_enum(codec: Codec) -> None:
    """`ctypes` passes `lb_type_t` as a number taken from the position in `ValueType`.
    A reordering on either side would send every value through the wrong type — the
    sizes are the cheapest thing that notices."""
    if not isinstance(codec, CCodec):
        pytest.skip("a property of the C binding")
    for t in ValueType:
        assert codec.size_of(t) == t.spec.size, t
