# lagunbus-node

The LagunBus **node library**, in two languages: C for firmware, Python for simulators
and tools. It implements [`lagunbus-spec`](../lagunbus-spec/) from the node's side. The
hub ([`../hub/`](../hub/)) is the other side of the contract and shares no code with it.

The library knows nothing about hardware. Boards live in [`../devices/`](../devices/),
which takes the C library as a submodule and adds only the glue: the ESP32-C3 port, the
`key` and `output` applications, the ESP-IDF container and the flashing scripts.

Why this is a repository of its own, and what was decided along the way, is written in
[`zadania/zrobione/FEAT-03.md`](../zadania/zrobione/FEAT-03.md).

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

> **TODO:** this repository exists only locally — there is no GitHub repository yet.
> `devices` already records it as a submodule at
> `git@github.com:stefantobiasiewicz/lagunbus-node.git`; until that exists and `main` is
> pushed there, a fresh clone of `devices` cannot fetch the library. Once created:
> `git remote add origin <url> && git push -u origin main` here, and if the name differs,
> `git submodule set-url lib/lagunbus-node <url>` in `devices`.

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
`values`, `endpoint`, `node`, `tools.debounce`, `types.key`, `types.output`, the two interfaces in `interfaces`, and
`registry` generated. It has exactly the C library's scope. A feature the C side lacks is
missing here too, and a new one goes into both in one change. On top of that it has two
things a simulator needs and firmware does not:

- `memory` — an in-memory bus and platform with a clock set by hand;
- `faults` — an **overlay on the transport** that loses responses and segments, falls
  silent, and answers `BUSY` in the node's name. The node has no hook for it; every
  fault can be told from the frames alone.

The Python code keeps C's fixed widths on purpose: counters wrap at their C width, and
time is arithmetic modulo 2³² as `millis()` is in C. Where C does something because of its
types, Python copies it, and says so. One such case is `u32` capped at `INT32_MAX` — see below.

## Tools and types

The library is split by what a piece IS, the same way in both languages:

| | C (`c/lagunbus/`) | Python (`python/lagunbus_node/`) | what |
|---|---|---|---|
| core | `include/lagunbus/*.h`, `src/*.c` | `*.py` | frame, values, endpoint, node, the two interfaces — every node needs them |
| `tools/` | `…/tools/debounce.h` | `tools/debounce.py` | helpers a device MAY use; none belongs to a type (a bounce filter serves `KeyInput`, `Contact` and `ToggleInput` alike) |
| `types/` | `…/types/key.h`, `…/types/output.h` | `types/key.py`, `types/output.py` | the pieces of one endpoint type each: attribute rows, endpoint hooks, behaviour, event encoders |

Include as `"lagunbus/types/key.h"`, import as `from lagunbus_node.types import key`.
A new type goes to `types/`, a helper that no type owns to `tools/`
(`zadania/CR-01.md`, step 6).

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

A **scenario** is a device profile, a starting store, and steps. The profiles live in
`c/tests/harness.c` and `python/tests/conformance/profiles.py`: `output` (as
`devices/apps/output`), and three keys — `key` (every feature, every timing tunable),
`bell` (no features, no attributes) and `key_fixed` (`HOLD` and `MULTICLICK` with the
device's own timings). Each step sets the clock, hands the node
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

## Decisions

### `u32` is writable only up to `INT32_MAX` (2026-09-29)

The spec gives `u32` the range `0 … 0xFFFFFFFE`. This library accepts a **write** of a
`u32` attribute only up to `0x7FFFFFFF`; above that the node answers `INVALID_VALUE`.
A deliberate cut, in both languages alike.

**Why.** A value lives in `lb_value_t`, an `int32_t` with an "absent" flag — one type for
every scalar, so an attribute array is uniform and `lb_in_range` is a single comparison.
The upper half of `u32` does not fit in it. The alternatives cost more than the range is
worth today:

- an `int64_t` value doubles every attribute in RAM, and makes every comparison a
  multi-instruction 64-bit operation on the ESP32-C3's 32-bit RISC-V core — paid by
  every attribute of every device for a range none of them uses;
- an unsigned raw value with the sign kept per type turns every range check into a
  branch on the type, in the one place that must stay obviously correct.

**What it does not affect.** Only the range check on a write. Reading and reporting go
through the raw bits: a `u32` counter (`Uptime`, `PressCount`, `TxFrames`) goes past
2³¹ on the wire correctly. And today **no writable `u32` attribute exists**, so no real
write is refused.

**When to revisit.** With the first writable `u32` attribute whose values can exceed
2³¹ − 1 (a meter preset, say). Then both libraries change together, and the `u32` entry
in `vectors/codec.json` → `ranges` changes with them: it records the cap on purpose,
so that the conformance tests fail the moment one side lifts it alone.

### `KeyInput` is a set of tools, not a key (2026-09-29)

The library gives a device the pieces of a key and leaves the decisions to it
(`zadania/CR-01.md`, N6, K1–K5). `key.h` lists them: attribute rows `LB_KEY_ATTR_…` for
the attributes the device chooses to expose, the endpoint hooks `lb_key_check` and
`lb_key_feature_of`, the event state machine `lb_key_t`, and `lb_key_emit_…` for a device
with its own logic. Bounce is a separate, type-free tool (`debounce.h`); polarity is the
device's alone.

**Why.** The spec made bounce and polarity matters of how a device is built, and made
every timing optional (`types/0x0010-key-input.md` §1, §2.4). A library that read them
from attributes could not serve a device that does not expose them; one that hid the
state machine behind a single call could not serve a device with its own. What the
device does not expose comes from its `lb_key_config_t`.

**Two hooks in `lb_endpoint_t`**, both optional, for every type to use:

- `on_check` runs **before** a write lands. Until now a write checked only the value
  type — `HoldThreshold = 10` fitted a `u16` and was taken, although the type allows
  200–3000 ms — and a refusal in `on_write` came after the value had landed, so the
  write was not whole (LB-NODE-026). `lb_key_check` holds the type's ranges and the rule
  that `EventMode` takes only a mode whose feature is declared.
- `feature_of` maps an element of the type to its features, so that one outside the
  declared features answers `UNSUPPORTED_FEATURE`, not `UNSUPPORTED_ELEMENT`
  (LB-NODE-061). The attribute array cannot tell: a device simply does not list what it
  lacks. The gate comes before the array, so the declaration decides, not the listing.

**One choice of this implementation, not of the type:** a press already on at start
produces nothing, its release included (`silent_start_press`, on by default). The spec
leaves such cases to the device (CR-01 N1b).

**Pinned by** `vectors/scenarios/key_checks.json`, `key_bell.json`, `key_fixed.json`,
next to the behaviour scenarios `key.json`, `key_raw.json`, `key_held_at_start.json`.

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
`types/key.h`/`types/key.py` and `types/output.h`/`types/output.py`. A type change means
both, plus a scenario.

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
