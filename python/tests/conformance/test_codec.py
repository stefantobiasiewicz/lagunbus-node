"""`vectors/codec.json` — the frame codec through both languages, the rest through C.

Identifiers, headers and joins run through `codec`, parametrised over Python and C.
Values, ranges, blobs and ShortId run through `c_codec` only: since zadania/zrobione/FEAT-02.md
N11 the Python package has no value codec and no node.
"""

from __future__ import annotations

import json
from typing import Any

import pytest

from lagunbus_node.registry import MessageKind, ResultCode
from tests.conformance.conftest import VECTORS
from tests.conformance.implementations import CCodec, FrameCodec, ValueType

DATA: dict[str, Any] = json.loads((VECTORS / "codec.json").read_text(encoding="utf-8"))


def num(text: str | int) -> int:
    return text if isinstance(text, int) else int(text, 0)


def kind(value: str | int) -> int:
    return value if isinstance(value, int) else MessageKind[value]


def ids(section: str) -> list[str]:
    return [str(entry.get("note", i)) for i, entry in enumerate(DATA[section])]


@pytest.mark.parametrize("v", DATA["identifiers"], ids=ids("identifiers"))
def test_identifier(codec: FrameCodec, v: dict[str, Any]) -> None:
    fields = (kind(v["kind"]), num(v["address"]), v["endpoint"], num(v["element"]))
    assert codec.encode_id(*fields) == num(v["can_id"])
    assert codec.decode_id(num(v["can_id"])) == fields


@pytest.mark.parametrize("v", DATA["identifier_refusals"], ids=ids("identifier_refusals"))
def test_identifier_refused(codec: FrameCodec, v: dict[str, Any]) -> None:
    assert codec.encode_id(v["kind"], num(v["address"]), v["endpoint"], num(v["element"])) is None


@pytest.mark.parametrize("v", DATA["joins"], ids=ids("joins"))
def test_join(codec: FrameCodec, v: dict[str, Any]) -> None:
    assert codec.join_id(num(v["short_id"]), v["announce"]) == num(v["can_id"])


@pytest.mark.parametrize("v", DATA["headers"])
def test_header(codec: FrameCodec, v: dict[str, Any]) -> None:
    result = ResultCode[v["result"]]
    assert codec.encode_header(v["tid"], result) == num(v["byte"])
    assert codec.decode_header(num(v["byte"])) == (v["tid"], result)


@pytest.mark.parametrize("v", DATA["values"], ids=ids("values"))
def test_value(c_codec: CCodec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    raw = bytes.fromhex(v["bytes"])
    assert c_codec.encode(t, v["value"]) == raw
    assert c_codec.decode(t, raw) == (v["value"],)


@pytest.mark.parametrize("v", DATA["value_refusals"], ids=ids("value_refusals"))
def test_value_refused(c_codec: CCodec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    if "bytes" in v:
        assert c_codec.decode(t, bytes.fromhex(v["bytes"])) is None
    else:
        assert c_codec.encode(t, v["value"]) is None


@pytest.mark.parametrize("v", DATA["ranges"], ids=[r["type"] for r in DATA["ranges"]])
def test_range(c_codec: CCodec, v: dict[str, Any]) -> None:
    t = ValueType(v["type"])
    assert (c_codec.lo(t), c_codec.hi(t)) == (v["lo"], v["hi"])


@pytest.mark.parametrize("v", DATA["in_range"])
def test_in_range(c_codec: CCodec, v: dict[str, Any]) -> None:
    assert c_codec.in_range(ValueType(v["type"]), v["value"]) is v["fits"]


@pytest.mark.parametrize("v", DATA["short_ids"], ids=ids("short_ids"))
def test_short_id(c_codec: CCodec, v: dict[str, Any]) -> None:
    assert c_codec.short_id(bytes.fromhex(v["uid"])) == num(v["short_id"])


@pytest.mark.parametrize("v", DATA["blobs"])
def test_blob(c_codec: CCodec, v: dict[str, Any]) -> None:
    data, raw = bytes.fromhex(v["data"]), bytes.fromhex(v["bytes"])
    assert c_codec.encode_blob(data) == raw
    assert c_codec.decode_blob(raw) == data


@pytest.mark.parametrize("v", DATA["blob_refusals"], ids=ids("blob_refusals"))
def test_blob_refused(c_codec: CCodec, v: dict[str, Any]) -> None:
    assert c_codec.decode_blob(bytes.fromhex(v["bytes"])) is None


def test_value_types_are_declared_in_the_order_of_the_c_enum(c_codec: CCodec) -> None:
    """`ctypes` passes `lb_type_t` as a number taken from the position in `ValueType`.
    A reordering on either side would send every value through the wrong type — the
    sizes are the cheapest thing that notices."""
    for t in ValueType:
        assert c_codec.size_of(t) == t.size, t
