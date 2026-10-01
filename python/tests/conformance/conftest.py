"""Fixtures that hand a test the implementations.

`codec` is parametrised over `python` and `c` — the frame codec exists in both, so a
report shows which side failed. `c_codec` and `device` are the C library alone: values,
blobs and the node exist only there (zadania/zrobione/FEAT-02.md, N11).

The C side is `liblagunbus_sim`, loaded through `lagunbus_node.sim`, which **builds it**
with CMake when it is missing or older than the sources — a conformance run against a
stale library would compare the Python code with yesterday's C code and pass for the
wrong reason. `LAGUNBUS_SIM=/path/to/lib` skips the build and uses that file.
"""

from __future__ import annotations

import ctypes
from collections.abc import Callable
from pathlib import Path

import pytest

from lagunbus_node import sim
from tests.conformance.implementations import (
    CCodec,
    CDevice,
    Device,
    FrameCodec,
    PyCodec,
    declare_codec,
)

ROOT = Path(__file__).resolve().parents[3]
VECTORS = ROOT / "vectors"

IMPLEMENTATIONS = ["python", "c"]

DeviceFactory = Callable[[str, bytes, int, int], Device]
"""`(profile, uid, jitter, channels)` — channels 0 is the device's default."""


@pytest.fixture(scope="session")
def c_library() -> ctypes.CDLL:
    try:
        return declare_codec(sim.load())
    except sim.BuildError as exc:
        pytest.fail(str(exc), pytrace=False)


@pytest.fixture(params=IMPLEMENTATIONS)
def codec(request: pytest.FixtureRequest) -> FrameCodec:
    if request.param == "python":
        return PyCodec()
    return CCodec(request.getfixturevalue("c_library"))


@pytest.fixture
def c_codec(c_library: ctypes.CDLL) -> CCodec:
    return CCodec(c_library)


@pytest.fixture
def device(c_library: ctypes.CDLL) -> DeviceFactory:
    return CDevice
