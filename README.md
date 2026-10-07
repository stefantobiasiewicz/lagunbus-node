# lagunbus-node

The LagunBus **node library**. The node is written in C — one implementation, the one
that goes onto a board and the one a simulator runs. Python has the tools around it: the
C node behind `ctypes`, the frame codec, a fault overlay and transports for a computer.
It implements [`lagunbus-spec`](../lagunbus-spec/) from the node's side. The hub
([`../hub/`](../hub/)) is the other side of the contract and shares no code with it.

The library knows nothing about hardware. Boards live in [`../devices/`](../devices/),
which takes the C library as a submodule and adds only the glue: the ESP32-C3 port, the
`key` and `output` applications, the ESP-IDF container and the flashing scripts.

Why this is a repository of its own, and what was decided along the way, is written in
[`zadania/zrobione/FEAT-03.md`](../zadania/zrobione/FEAT-03.md); why the node is in C
alone — [`zadania/zrobione/FEAT-02.md`](../zadania/zrobione/FEAT-02.md) and § Decisions below.

```
  lagunbus-spec ──── registry/*.yaml
        │
        ▼
  lagunbus-node/
  ├── c/          the node — ESP-IDF component, POSIX port, tests, lagun-node,
  │               and sim/: the catalogue of simulated devices behind a flat API
  ├── python/     the tools around the node — `sim` (the C node through ctypes),
  │               frame codec, faults, transports
  ├── vectors/    codec vectors and behaviour scenarios, as DATA
  └── tools/      one registry generator, two outputs (.h and .py)
        │                    │
        ▼                    ▼
  devices/ (submodule)       lagun-sim (zadania/zrobione/FEAT-02.md)
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
| `c/sim/` | the catalogue of simulated devices (`devices.c`) and a flat API over the node for `ctypes` (`sim.h` says why) — the port a simulator and the conformance tests use |
| `c/tests/` | unit tests with a clock driven from the test |
| `c/CMakeLists.txt` | the native build: static library, tests, `lagun-node`, `liblagunbus_sim` |

## Python

```sh
cd python
python3 -m venv .venv && .venv/bin/python -m pip install -e ".[dev]"
.venv/bin/python -m pytest -q                    # builds liblagunbus_sim itself (cmake)
.venv/bin/ruff check . && .venv/bin/ruff format --check .
.venv/bin/mypy lagunbus_node tests               # strict
```

The package `lagunbus_node` has **no node** (§ Decisions). It has what a simulator and
the tests need around one:

| module | what |
|---|---|
| `sim` | the C node through `ctypes`: `devices()` lists the catalogue, `SimDevice` is one device — clock, store, frames in and out, pins, outputs. Builds `liblagunbus_sim` with CMake on first use and whenever a source is newer; `LAGUNBUS_SIM=/path/to/lib` loads a given file instead. Needs the source tree (`pip install -e`), a C compiler and CMake |
| `frame` | the frame codec — identifiers, headers, joins. The traffic log and the fault overlay read frames with it, and it is the one piece checked against C |
| `faults` | an **overlay on the transport** that loses responses and segments, falls silent, and answers `BUSY` in the node's name. The node has no hook for it; every fault can be told from the frames alone |
| `ports` | transports for a computer: `socketcan` (Linux, `vcan0` or a real interface) and `udp` (multicast on this machine, the other end of `lagun-hub run --udp` — § Decisions) |
| `interfaces`, `memory` | the `Transport` protocol, and a `QueueTransport` whose two ends a test holds |
| `registry` | generated — see below |

A simulator pumps a `SimDevice` itself: frames from its `Transport` (through `faults`,
if it wants them) into `rx`, one `loop` per turn with the clock set, `take_tx` back out.

## Tools and types

The C library is split by what a piece IS:

| | `c/lagunbus/` | what |
|---|---|---|
| core | `include/lagunbus/*.h`, `src/*.c` | frame, values, endpoint, node, the two interfaces — every node needs them |
| `tools/` | `…/tools/debounce.h`, `led.h`, `service.h` | helpers a device MAY use; none belongs to a type (a bounce filter serves `KeyInput`, `Contact` and `ToggleInput` alike; the status LED and the service button serve every device) |
| `types/` | `…/types/key.h`, `…/types/output.h` | the pieces of one endpoint type each: attribute rows, endpoint hooks, behaviour, event encoders |

Include as `"lagunbus/types/key.h"`. A new type goes to `types/`, a helper that no type
owns to `tools/` (`zadania/zrobione/CR-01.md`, step 6).

## Tests

| level | what | where |
|---|---|---|
| unit, C | the library, a clock driven from the test, no bus | `c/tests/` — `ctest` |
| unit, Python | the tools: fault overlay (on the C node), transports, the `sim` binding, the registry check | `python/tests/` |
| **conformance** | `vectors/codec.json` and every `vectors/scenarios/*.json`, as DATA | `python/tests/conformance/` |

The conformance tests run **in one process**, through `lagunbus_node.sim`. No `vcan0`,
so they run on macOS, where development happens. The frame codec exists in both
languages, so the identifier, header and join vectors are parametrised `[python]` /
`[c]`; values, blobs, ShortId and every scenario run through C alone.

A **scenario** is a device from the catalogue (`c/sim/devices.c`), a starting store, and
steps. The catalogue is the simulator's too, so every device `lagun-sim` offers is one a
scenario can check: `key` (every feature, every timing tunable), `bell` (no features, no
attributes), `key_fixed` (`HOLD` and `MULTICLICK` with the device's own timings),
`panel3` (three keys), `key3` (as `devices/apps/key3`: `RAW` and `HOLD`, no multi-click),
`output` (as `devices/apps/output`), `output2` and `output8` (outputs that keep their own
state), `out8` (as `devices/apps/out8`). `key` and `output` also come in 1 … 8 channels
(`max_channels` in the table, `channels` in a scenario — `key_x4.json`, `output_x3.json`);
one, the board they copy, is the default. Each step sets the clock, hands the node frames,
sets the input pins (`pin` — one level, or a list for several inputs), and runs one turn
of the board's loop. What the node sent must match `expect` exactly, one extra frame
included:

```json
{"at": 100, "note": "Toggle -> on", "rx": ["06108402#01"],
 "expect": ["10108400#01", "0E108402#01"], "driven": 0}
```

Frames are written as `candump` writes them, `IDENTIFIER#DATA`. A scenario that fails
means the node is wrong or the spec can be read two ways; the second goes to a decision
straight away (`zadania/README.md`).

Adding behaviour: write the scenario first, see it fail, make it pass. Adding a device:
a function and a line in `c/sim/devices.c`, and a scenario **only for a case no scenario
covers yet** — a feature combination, a setting or a size nothing else exercises. A device
that only recombines what is already checked (eight outputs, a key with the known
features) needs none: the scenario would test the same library code a second time and
pin nothing new (zadania/zrobione/FEAT-09.md).

## Decisions

### `u32` is writable only up to `INT32_MAX` (2026-09-29)

The spec gives `u32` the range `0 … 0xFFFFFFFE`. This library accepts a **write** of a
`u32` attribute only up to `0x7FFFFFFF`; above that the node answers `INVALID_VALUE`.
A deliberate cut.

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
2³¹ − 1 (a meter preset, say). Then the `u32` entry in `vectors/codec.json` → `ranges`
changes with the library: it records the cap on purpose, so that lifting it is a
visible decision and not a side effect.

### `KeyInput` is a set of tools, not a key (2026-09-29)

The library gives a device the pieces of a key and leaves the decisions to it
(`zadania/zrobione/CR-01.md`, N6, K1–K5). `key.h` lists them: attribute rows `LB_KEY_ATTR_…` for
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

### The node is in C alone; Python is the tools around it (2026-09-30)

Until this date the node existed twice, in C and in Python, module for module, held
together by the conformance tests (`zadania/zrobione/FEAT-03.md`, N7). The Python node
was built for one user — the simulator. `zadania/zrobione/FEAT-02.md` (N10, N11) reversed that:
the simulator runs **the C library**, through `ctypes`, and the Python node went.

**Why.** A simulator is worth most when it runs the code that goes onto a board: then
running the hub against it checks the firmware too, not a twin of it. A second node
cost every change twice (`zadania/zrobione/CR-01.md` changed the key in both), for one
benefit — an independent second reading of the spec. That reading still exists: the hub
is written separately, from the other side of the contract, with a node double of its
own in its tests.

**What made it cheap.** The conformance harness already drove the C node with no
callbacks into Python — clock, store and queues on the C side, Python setting the clock
and moving frames. Generalised into `c/sim/`, it serves a simulator as it is, and the
fault overlay works on the C node unchanged, because every frame passes through Python.

**What stayed in Python** is what is not the node: the frame codec (kept in both, and
still checked against C), the fault overlay, the transports, the registry, and `sim`.

**The catalogue is shared.** `c/sim/devices.c` is both the simulator's list of devices
and the scenarios' profiles — one list, so that nothing the simulator offers goes
unchecked.

### UDP multicast as a bus for simulated nodes (2026-09-30)

`lagunbus_node.ports.udp` and the hub's `lagunhub/protocol/bus/udp.py` carry CAN frames
between processes on one machine: the hub and simulated nodes (`lagun-sim`,
`zadania/zrobione/FEAT-02.md`) where there is no CAN, macOS above all. **Test and development
only** — the hub opens it only with `lagun-hub run --udp`, never from `hub.yaml`.

The two sides are written separately and share no code, so this is the one description
of the format. Every datagram is one frame, 24 bytes:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `LBU1` — LagunBus over UDP, version 1 |
| 4 | 4 | sender — a random u32 per open port, little-endian |
| 8 | 16 | `struct can_frame` in the SocketCAN layout: id with `CAN_EFF_FLAG`, length, 3 bytes of padding, 8 bytes of data — all little-endian |

Group `239.255.76.66`, port `7607` by default; TTL 0 and the loopback interface, so
nothing leaves the machine. Anything else arriving in the group — a wrong size, a
foreign magic, a length past 8 — is dropped, not fatal.

**Why multicast.** Every process joins the group and hears every other one — a cable,
with no broker to start first and no order to start in. A Unix socket cannot multicast,
so it would need a broker process.

**Why `sender`.** A process hears its own datagrams back from the group. SocketCAN does
not give a socket its own frames, and the node must not see its own; `sender` is how
each side drops them.

**What it does not do: arbitrate.** Frames arrive in the order they were sent, not by
identifier. Enough for running things together; arbitration is tested on the hub's
`VirtualBus`, which does fake it.

**Pinned by** the same byte vector in `python/tests/test_ports.py` and
`hub/tests/test_udp_bus.py` — a change on one side alone fails the other.

### The node tells the device its state; the device shows it (2026-10-03)

Every device has a service button and a status LED (`zadania/zrobione/CR-05.md`). The library
says **what is happening**, the device decides **how to show it** — the library does not
know a GPIO exists, for the same reason it does not know `ESP_LOGI` does (`on_frame`).

**One state at a time.** `on_state` gets a single `lb_node_state_t`, the most important
by a fixed priority: `RESETTING` > `IDENTIFY` > `BUS_FAULT` > `NO_ADDRESS` > `READY`.
Several can be true at once (no address on a dead bus); deciding which one shows in the
library means every device shows the same thing in the same situation. The rejected
alternative — separate facts, each device picking — gives an RGB device more freedom
and every device its own priority. Called on change only, and once from
`lb_node_start`. There is no "joined" state (the assignment and `Booted` happen in the
same call, no tick sees the gap) and no "no hub" state (the node does not know the hub;
alone on the cable it gets no ACKs and goes error passive, which is `BUS_FAULT`).

**The bus is read at every tick**, not every 250 ms as first proposed: the ESP32 is out
of bus-off in about 11 ms, and a slower look missed bus-offs — `BusOffCount` counts
every entry, and until now nothing raised it at all.

**The button is `lb_node_factory_reset()`** — the hub's `FactoryReset` path without the
reply, so `RESETTING` reaches the LED before the restart. The project's guard — held
5 s, reset on the release, armed only once seen released — is `tools/service.h`, a
guideline for this project's devices, not a rule of the specification. The patterns
are `tools/led.h`, single colour or RGB, so all devices blink alike.

**`Identify` and `Reboot`** are mandatory in `Device` and were missing (the hub's view
sent `Identify` on `i` and got `UNSUPPORTED_ELEMENT`). `Reboot` takes the reset path
without the wipe, its delay never below `LB_RESET_GRACE_MS` — "0 = at once" would lose
the acknowledgement. Without a restart hook it does what a restart looks like on the
bus: `Booted` with `SOFTWARE`, the boot counted.

**`LastResetReason` comes from the platform** (`platform->reset_reason`, on the board
`esp_reset_reason()`), and the `LB_RESET_*` constants now have the specification's
values — they were shifted (`SOFTWARE` 1, `BROWNOUT` 3) and never used, so the node said
`POWER_ON` after every restart.

**`Diagnostics.BusState` has the specification's three values** (`LB_DIAG_BUS_*`):
the port's `LB_BUS_*` has a fourth, `WARNING`, which the attribute covers with
`ErrorThreshold`, so it goes out as `ACTIVE`. The node used to send the port's 0…3 as
they were, and the hub agreed — both against the specification, where PASSIVE is 1.

**Pinned by** `vectors/scenarios/state_join.json`, `identify.json`, `service_reset*.json`,
`bus_fault*.json`, `bus_state.json`, `reboot*.json`, `reset_reason.json`, the states added to
`factory_reset*.json`, and `c/tests/test_led.c`, `test_service.c`.

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
`types/key.h` and `types/output.h`. A type change means those, plus a scenario.

## What is not here yet

Missing in the node. Moved here from `devices/README.md`, because none of it is about
hardware.

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
  returns to its default (zadania/TASK-01.md);
- the firmware version in `Booted`: raising it should cause re-enumeration, a restart
  without a change should not.

**Common attributes and reporting** on functional endpoints: `Enabled`, `QueryEnabled`,
cyclic reports, `ReportHysteresis` (zadania/TASK-01.md). The scenario
`vectors/scenarios/output.json` records today's `UNSUPPORTED_ELEMENT` for `Enabled`.

**Groups:** the node declares `GroupCapacity = 0` and ignores broadcast frames.
