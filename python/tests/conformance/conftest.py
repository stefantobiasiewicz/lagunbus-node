"""Fixtures that hand a test each implementation in turn.

`codec` and `device` are parametrised over `python` and `c`, so every conformance test
runs twice and a report shows which side failed.

The C side needs `liblagunbus_harness` from `c/build`. The fixture **builds it** with
CMake when it is missing or older than the sources — a conformance run against a stale
library would compare the Python code with yesterday's C code and pass for the wrong
reason. `LAGUNBUS_HARNESS=/path/to/lib` skips the build and uses that file.
"""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import sys
from collections.abc import Callable, Iterator
from pathlib import Path

import pytest

from tests.conformance.implementations import (
    CCodec,
    CDevice,
    Codec,
    Device,
    PyCodec,
    PyDevice,
    load_library,
)

ROOT = Path(__file__).resolve().parents[3]
C_DIR = ROOT / "c"
BUILD = C_DIR / "build"
VECTORS = ROOT / "vectors"

IMPLEMENTATIONS = ["python", "c"]

DeviceFactory = Callable[[str, bytes, int], Device]


def _library_name() -> str:
    # A lookup, not an `if` on `sys.platform`: mypy evaluates those for the platform
    # it runs on and calls the other branch unreachable.
    suffix = {"darwin": "dylib"}.get(sys.platform, "so")
    return f"liblagunbus_harness.{suffix}"


def _build() -> Path:
    override = os.environ.get("LAGUNBUS_HARNESS")
    if override:
        return Path(override)
    cmake = shutil.which("cmake")
    if cmake is None:
        pytest.fail("the C side needs cmake to build liblagunbus_harness", pytrace=False)
    if not (BUILD / "CMakeCache.txt").exists():
        subprocess.run([cmake, "-S", str(C_DIR), "-B", str(BUILD)], check=True, capture_output=True)
    # `cmake --build` is incremental: a no-op when nothing changed, a rebuild when a
    # source did. That is exactly the staleness check the docstring promises.
    result = subprocess.run(
        [cmake, "--build", str(BUILD), "--target", "lagunbus_harness"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        pytest.fail(f"building liblagunbus_harness failed:\n{result.stdout}{result.stderr}")
    return BUILD / _library_name()


@pytest.fixture(scope="session")
def c_library() -> ctypes.CDLL:
    return load_library(_build())


@pytest.fixture(params=IMPLEMENTATIONS)
def codec(request: pytest.FixtureRequest) -> Codec:
    if request.param == "python":
        return PyCodec()
    return CCodec(request.getfixturevalue("c_library"))


@pytest.fixture(params=IMPLEMENTATIONS)
def device(request: pytest.FixtureRequest) -> Iterator[DeviceFactory]:
    if request.param == "python":
        yield PyDevice
        return
    lib = request.getfixturevalue("c_library")
    yield lambda profile, uid, jitter: CDevice(lib, profile, uid, jitter)
