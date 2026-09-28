# lagunbus-node

The LagunBus **node library**, in two languages: C for firmware, Python for simulators
and tools. It implements [`lagunbus-spec`](../lagunbus-spec/) from the node's side. The
hub ([`../hub/`](../hub/)) is the other side of the contract and shares no code with it.

The library knows nothing about hardware. Boards live in [`../devices/`](../devices/),
which takes the C library as a submodule and adds only the glue: the ESP32-C3 port, the
`key` and `output` applications, the ESP-IDF container and the flashing scripts.

Why this is a repository of its own, and what was decided along the way, is written in
[`zadania/FEAT-03.md`](../zadania/FEAT-03.md).

```
  lagunbus-spec ──── registry/*.yaml
        │
        ▼
  lagunbus-node/
  ├── c/          the C library — ESP-IDF component, POSIX port, tests, lagun-node
  ├── python/     the Python library — same concepts, same two interfaces
  ├── vectors/    codec vectors and behaviour scenarios, as DATA, read by both
  └── tools/      one registry generator, two outputs (.h and .py)
        │                    │
        ▼                    ▼
  devices/ (submodule)       simulator (zadania/FEAT-02.md)
```

The spec repository has to sit next to this one (`../lagunbus-spec`) for the registry
generator and its check.

---

## C

```sh
cd c
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure      # `-R key` for one test
./build/lagun-node --bus can0 [--sniff]          # Linux only; the same code as a board
```

`-Wall -Wextra -Werror -Wconversion`. No threads (`lb_node_tick()` is the whole loop), no
`malloc`, no `printf`. The library touches the world only through
[`transport.h`](c/lagunbus/include/lagunbus/transport.h) and
[`platform.h`](c/lagunbus/include/lagunbus/platform.h).

| path | what |
|---|---|
| `c/lagunbus/` | the library, **and** an ESP-IDF component (`CMakeLists.txt` with `idf_component_register`). A firmware project points `EXTRA_COMPONENT_DIRS` at this directory |
| `c/ports/posix/` | the reference port: SocketCAN, a clock, files. Builds everywhere; opens a bus only on Linux |
| `c/lagun-node/` | a node over SocketCAN, plus `--sniff` to watch traffic through our own codec |
| `c/tests/` | unit tests with a clock driven from the test, and `harness.c` for the conformance tests |
| `c/CMakeLists.txt` | the native build: static library, tests, `lagun-node`, `liblagunbus_harness` |

## Python

```sh
cd python
python3 -m venv .venv && .venv/bin/python -m pip install -e ".[dev]"
.venv/bin/python -m pytest -q                    # unit + conformance; builds the C harness itself
.venv/bin/ruff check . && .venv/bin/ruff format --check .
.venv/bin/mypy lagunbus_node tests               # strict
```

The package `lagunbus_node` is a port of the C library, **module for module**: `frame`,
`values`, `endpoint`, `node`, `key`, `output`, the two interfaces in `interfaces`, and
`registry` generated. It has exactly the C library's scope. A feature the C side lacks is
missing here too, and a new one goes into both in one change. On top of that it has two
things a simulator needs and firmware does not:

- `memory` — an in-memory bus and platform with a clock set by hand;
- `faults` — an **overlay on the transport** that loses responses and segments, falls
  silent, and answers `BUSY` in the node's name. The node has no hook for it; every
  fault can be told from the frames alone.

The Python code keeps C's fixed widths on purpose: counters wrap at their C width, and
time is arithmetic modulo 2³² as `millis()` is in C. Where C does something because of its
types, Python copies it, and says so. One such case is `u32` capped at `INT32_MAX`.

## Holding the two together

| level | what | where |
|---|---|---|
| unit, C | the library, a clock driven from the test, no bus | `c/tests/` — `ctest` |
| unit, Python | what only Python has: the fault overlay, the registry check | `python/tests/` |
| **conformance** | the SAME data through both: `vectors/codec.json` and every `vectors/scenarios/*.json` | `python/tests/conformance/` |

The conformance tests run **in one process**. The C library is built a second time as
`liblagunbus_harness`, a shared library with a small flat API (`c/tests/harness.c`) that
Python calls through `ctypes`. No `vcan0`, so they run on macOS, where development
happens. Every test is parametrised `[python]` / `[c]`, so a failure names the side.

A **scenario** is a device profile (`key` or `output`, attribute for attribute as in
`devices/apps/`), a starting store, and steps. Each step sets the clock, hands the node
frames, and runs one turn of the application loop. What the node sent must match
`expect` exactly, one extra frame included:

```json
{"at": 100, "note": "Toggle -> on", "rx": ["06108402#01"],
 "expect": ["10108400#01", "0E108402#01"], "driven": 0}
```

Frames are written as `candump` writes them, `IDENTIFIER#DATA`. A divergence between the
two libraries is worth having. It means one of them is wrong, or the spec can be read two
ways, and it goes to a decision straight away (`zadania/README.md`).

Adding behaviour: write the scenario first, see it fail in both, make both pass.

## The registry

```sh
python3 tools/generate_registry.py [--check]
```

One generator writes `c/lagunbus/include/lagunbus/registry.h` and
`python/lagunbus_node/registry.py` from `lagunbus-spec/registry/*.yaml`. Never edit either
by hand. `--check` runs in `ctest -R registry` and in the Python tests. The registry
still carries Polish names, and the generator holds the translation table. It must match
the one in `hub/tools/generate_registry.py`, and an unknown name fails on purpose.

The endpoint catalogue (`types/*.md`) is prose, so attribute numbers are hand-copied in
`key.h`/`key.py` and `output.h`/`output.py`. A type change means both, plus a scenario.

## What is not here yet

Missing in **both** languages. Moved here from `devices/README.md`, because none of it is
about hardware.

**Transactions on the node side:**

- `Tid` from the header byte and a deduplication buffer: **8 entries, 300 ms**. Only
  commands carried out with `OK` go into it. A refusal in the buffer would make a retry
  after `BUSY` get `BUSY` forever;
- `BUSY` when the node cannot act right now, with the right to retry (the fault overlay
  can answer `BUSY` on the node's behalf, but the node itself never does);
- a prioritised transmit queue: on overflow the oldest `REPORT` goes, never a
  `RESPONSE` or an `EVENT`.

**Persistence and life cycle** (the address, the boot counter and the output state are
already persistent):

- persistent attributes. Settings are meant to survive a power cut, and today every one
  returns to its default (zadania/CR-01.md);
- `Reboot` (today `UNSUPPORTED_ELEMENT`) and `Identify` ("which box is it").
  `FactoryReset` is done: acknowledged from the old address and carried out 50 ms later.
  The address is stored as none, then `restart` runs (on the board `esp_restart()`).
  Without a restart hook (host, tests, the Python default) the node only drops the
  address and the system settings and rejoins;
- the firmware version in `Booted`: raising it should cause re-enumeration, a restart
  without a change should not.

**Common attributes and reporting** on functional endpoints: `Enabled`, `QueryEnabled`,
cyclic reports, `ReportHysteresis` (zadania/TASK-01.md). The scenario
`vectors/scenarios/output.json` records today's `UNSUPPORTED_ELEMENT` for `Enabled`.

**Groups:** the node declares `GroupCapacity = 0` and ignores broadcast frames.
