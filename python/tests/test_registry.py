"""The generated `registry.py` agrees with lagunbus-spec — the same check `ctest -R
registry` runs for the C header, from the same generator."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_registry_is_in_sync_with_the_spec() -> None:
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "generate_registry.py"), "--check"],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
