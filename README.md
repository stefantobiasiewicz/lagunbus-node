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
| `tools/` | `…/tools/debounce.h`, `digits.h`, `epaper.h`, `led.h`, `service.h` | helpers a device MAY use; none belongs to a type (a bounce filter serves `KeyInput`, `Contact` and `ToggleInput` alike; the status LED and the service button serve every device; a number in segment places and the pacing of an e-paper serve any screen of that kind) |
| `types/` | `…/types/key.h`, `…/types/output.h`, `…/types/display.h`, `…/types/sense.h`, `…/types/value_input.h` | the pieces of one endpoint type each: attribute rows, endpoint hooks, behaviour, event encoders |

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
state), `out8` (as `devices/apps/out8`), `rgb8` (as `devices/apps/rgb8`: one light,
`LEVEL | COLOR_RGB`; `light` in a step is what its drive function was last handed,
`[level, r, g, b, w]` — `output_level.json`, `output_color.json`), `display` (three `Display` fields: a temperature,
a text line, a humidity that can show text), `epd` (as `devices/apps/epd`: two `Display`
fields, VALUE only, drawn in the e-paper's places and paced as its glass), `env` (as `devices/apps/env`: three `Sense`
— a temperature, a humidity, a light — with `MIN_MAX`, `THRESHOLD` and `Offset`, values set
by `lagun-sim`). `key` and `output` also come in 1 … 8 channels
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

### The deduplication buffer: sixteen entries, only `OK` (2026-10-09)

A command repeated with the same `Tid`, endpoint and command number within 300 ms of
being carried out gets the remembered reply and is not carried out again
(`04-interakcje.md` §2.2). Until now the node had no buffer, and a reply late by more
than 100 ms made a key's `Toggle` switch the light twice (zadania/zrobione/BUG-02.md).

**Sixteen entries, not eight** (`LB_DEDUP_ENTRIES`, overridable at compile time). The
specification said "eight, as many as there are `Tid` values" — true only if `Tid` were
counted per node. It is counted per endpoint and command (the hub's `TidAllocator`, and
the specification's own "the budget is not shared within a node"), so an `out8` whose
eight outputs one key switches gets eight commands at once, and a ninth right after
pushed the first out while its retry could still come. The specification now says "at
least eight, every command of the window; sensibly two per endpoint that takes
commands". A wider `Tid` was rejected there: the only room is the result's bits, and it
would not help — the shortage is the number of keys, not the width of the number.

**Only `OK` goes in.** A refusal in the buffer would answer a retry after `BUSY` with
`BUSY` for the whole window; any other refusal repeats itself anyway. A command reply
carries no data, so an entry holds no result at all — the key and when, 8 bytes.

**The window counts from the execution; a retry does not move it.** Otherwise the third
attempt (200 ms) would keep the entry until 500 ms and eat into the `Tid` reserve.

**A full buffer pushes out an expired entry first, then the oldest live one — never
`BUSY`.** The hub retries after `BUSY` at once, so three attempts would be gone in a few
milliseconds and the command lost. Sized as above, a live entry is not pushed out.

**Every addressed command goes through it, `Device` ones too** — the check stands before
the dispatch. A repeated `Reboot` would otherwise move the restart. Reads and writes do
not: repeated, they do the same again.

**Pinned by** `vectors/scenarios/command_dedup.json`: retries 100, 200, 299 ms later
answered from the buffer; 300 ms after the execution carried out again; the key is all
three; eight `Tid` in a row to one command none swallowed; nine commands in the window
and a retry of the first; a repeated `Reboot` that does not move the restart.

### The output drives the coil; the board's polarity is the application's (2026-10-13)

`Output.Invert` used to be both things at once: the polarity of the board's relay drive
(`devices/apps/output` switches on a LOW pin, so it set `Invert = 1`) and the receiver
wired to the normally-closed contact. The first is build — it never changes and the hub
has no reason to know it — yet it was writable from the bus: one wrong write, or a
`nodes.yaml` entry, and the relay ran backwards while every attribute looked right.
The specification now splits them (`types/0x0020-output.md` §5.6, zadania/zrobione/CR-04.md N7).

**`drive` gets the coil state.** The library still applies `Invert` when the endpoint has
it — that is installation, an optional attribute. A board whose relay is LOW-active inverts
the pin in its own `drive` callback, and does not declare `Invert` unless its terminals
offer the normally-closed contact. Rejected: a polarity flag in `lb_output_init` — it
would be one more thing the library decides for the board, and a single `!` in the
callback says the same.

**The simulator's `output` and `out8` profiles have no `Invert`**, so their `driven` is
the coil state, equal to `OnOff`; `output2`/`output8` keep it as a module with the
normally-closed contact. Pinned by `vectors/scenarios/output.json` (a write of `Invert`
is `UNSUPPORTED_ELEMENT` — `Invert` belongs to no feature) and `output_channels.json` (the attribute exists there);
the inverted path itself by `c/tests/test_output.c`.

**A write of `Invert` moves the coil at once** (`lb_output_write`, wired into the
endpoint's `on_write`; zadania/zrobione/BUG-07.md). `OnOff` describes the receiver, so when the
contact it hangs on changes, the coil has to follow to keep the receiver where it was;
until 2026-10-13 it waited for the next command, and the receiver sat switched while
`OnOff` said otherwise. Nothing is reported — the receiver did not change. An application
whose output has `Invert` has to wire `lb_output_write` in; today none does (`output2`
and `output8` of the simulator do). Pinned by `output_channels.json` and `test_output.c`.

### Common attributes, reporting and persistence belong to the node (2026-10-13)

Until zadania/zrobione/TASK-01.md a functional endpoint knew only `TypeId`, `TypeRevision`,
`Features` and `Channel`; the system ones kept their reporting settings in the node and
did nothing with them; `output.c` held back its own reports; and no persistent (`P`)
attribute was ever stored — a `PowerOnBehaviour = RESTORE` written by the hub was gone at
the very power cut it was for.

**One struct for the common settings** (`lb_common_t`, the endpoint's `.common`), on
system and functional endpoints alike, read and written by one code in `endpoint.c`; the
system ones differ only in what they refuse (`Enabled = 0`, `QueryEnabled ≠ 0`). ep0–ep2
are now `lb_endpoint_t`s inside the node (`node->system`). The field is `disabled`, not
`enabled`, because every board's endpoint table is a designated initializer and an
unnamed field is zero — zero has to be the default. A device gives its defaults there
(`.common = {.report_min_gap = 200}`); a row `LB_COMMON_*` in the attribute array is dead,
since common elements are answered before the array is searched.

**The reporting engine is the node's, and it is pushed, not polled** (N2). A type sets a
value with `lb_endpoint_set` and calls `lb_node_flush` once for everything it changed
together; the node decides whether that is a change (`LB_HYST` and `ReportHysteresis`,
"absent" coming or going always is), sends it now or holds it back by `ReportMinGap`, and
runs the `ReportInterval` cycles in `lb_node_tick`. Rejected: the node scanning the arrays
for changes — it would report every step of a fade, which `Output` forbids. Whether an
attribute is reported is the TYPE's contract, so the type marks it (`LB_REPORTED`) in its
init; a board cannot get it wrong.

- **The gap is between rounds** (N3, `lagunbus-spec` `01-model-danych.md` §6): everything
  owed at that moment goes out together. A held-back round carries the current values.
- **A cycle counts from the last cycle**, in whole periods, from `Booted` or from the
  write of `ReportInterval`; a change report does not reset it. A disabled endpoint's
  cycle keeps its beat and sends nothing.
- **Nothing before `Booted`, nothing owed across it** (N8): `Booted` starts the cycles
  and drops what was owed; the hub reads the node then anyway. An attribute with no value
  since start is not reported by a cycle, and the state an endpoint starts in is not a
  change (Diagnostics settles its first reading silently).
- **The state lives in the endpoint** (`reports[LB_REPORTED_MAX]`, 8 slots), not in
  `lb_attr_t`: every board writes its rows as positional `{id, type, access, value}`, and a
  fifth field fails them all under `-Wextra -Werror`.

**`Enabled = 0`** (N4, `01-model-danych.md` §4.2): reads and writes work, a command gets
`INVALID_STATE`, no events (and no sequence number used), no reports.

**Persistence** (N5–N7): one key per attribute, `a<endpoint>.<element in hex>` — "a4.13",
"a1.f8" — holding the value's bytes on the wire; stored after every successful write,
before the reply (LB-NODE-027); loaded in `lb_node_start` before `Booted` through the
type's checks (`lb_endpoint_restore`: no access check, no `on_write`), so a value new
firmware no longer accepts is dropped and the default stays. Rejected: one blob per
endpoint, which new firmware with one attribute more would read as garbage. The factory
reset erases the **settings** — every writable persistent attribute — through the new
`platform->erase`; read-only persistent values are counters (`PressCount`, stored every
64 presses) and stay, as the output's remembered state does. `QueryEnabled` is stored and
never acted on (N9): no type here has inputs.

Pinned by `vectors/scenarios/reporting.json`, `persist.json`, `enabled.json` and
`press_count.json`; `bus_state.json` and `bus_fault.json` now also expect the `BusState`
reports.

### Strings and `LONG_WRITE`: a hook per endpoint, one buffer per node (2026-10-13)

`Display.Text` (zadania/zrobione/FEAT-12.md) is the first attribute that is both a string and
writable, so the node had to learn to receive `LONG_WRITE` and to keep an attribute that
is not a number.

**A string is a row with `LB_T_STRING` and two endpoint hooks**, `on_read_long` and
`on_write_long`; its bytes are the type's (`lb_display_t.text`). Rejected: bytes in
`lb_attr_t` — a row is twelve bytes for every attribute of every board and a text needs
dozens. `LB_T_STRING` is unknown to every value helper on purpose: an ordinary `WRITE`
of it fails to decode (`INVALID_VALUE`), a report of it encodes to nothing. A read
answers `SEGMENTED` and the node sends the bytes as `LONG_RESPONSE` — no longer only for
`Uid`.

**One assembly buffer per node**, `LB_LONG_WRITE_MAX` = 64 bytes with the prefix (N9):
the spec lets a node assemble one value at a time (`05-mapowanie-can.md` §6.7), and
`BUSY` answers segment 0 of a second one. A longer value is still taken to its LAST
segment — the hub waits for one reply — and refused with `INVALID_VALUE`. The reply goes
out once, after the LAST segment, with the Tid of segment 0. A gap or 100 ms of silence
drops the value without a word: the hub's transaction times out and repeats the whole
write, and a reply would only race it. The silence is measured when the next segment
comes, not by the tick — a dropped value nobody continues changes nothing.

**A scalar never travels in segments** (`02-typy-danych.md` §2.2, added with FEAT-12):
the form is decided by the attribute's type, not by the value's length, so a long write
of a number is `INVALID_VALUE` after the usual checks (endpoint, element, feature,
access). The system endpoints have no writable string: `Uid` is `READ_ONLY`, `Address`
and the common settings `INVALID_VALUE`.

**`Value` is typed by `Quantity` in `lb_display_init`**, not by the board's row
(`LB_DSP_ATTR_VALUE()` has no type): a temperature field cannot be given a `u16`.

Pinned by `vectors/scenarios/long_write.json` (the mechanics, on `output`) and
`display.json`.

### `Sense`: defaults from the quantity, the type's rules on the endpoint (2026-10-13)

`types/sense.h` is the whole type: `Value` and `Quantity` (zadania/zrobione/FEAT-13.md),
`MIN_MAX`, `THRESHOLD` and `Offset` (zadania/zrobione/DEVICE-01.md). The application hands
in what it read — `lb_sense_update`, `lb_sense_fail` — and the reporting engine decides
when it goes out. What was decided on the way:

- **`lb_sense_defaults` runs before `lb_node_start()` and types every `⟨Q⟩` row.** The
  node restores stored values at start, before the type's init, and a stored threshold is
  two bytes for a temperature and four for light — an untyped row restores nothing. It
  also sets the defaults of §5.3 into `.common`, so stored settings win and a factory
  reset brings them back. The table is hand-copied from `registry/quantities.yaml`.
- **Light's relative hysteresis is a hook on the endpoint** (`lb_hysteresis_fn`,
  `endpoint.h`), not a rule in the engine: the engine knows no quantities and the type
  does not hold the baseline. `Sense` lends it for `ILLUMINANCE` only: `ReportHysteresis`
  is the floor (50 lx), 10 % of the last reported value above it, 0 still "any change"
  (lagunbus-spec/DECYZJE.md, "Sense: histereza światła względna"). Rejected: a quantity
  switch in `endpoint.c` — the engine would grow a type's rule for every type after it.
- **The first good reading sets the zone without a `Threshold`.** No crossing happened,
  and an event at every start would be the false alarm §5.5 forbids; a hub that needs
  the zone at start compares `Value` with the thresholds itself. A failed reading keeps
  the zone. Into a zone past the threshold (at it is not above it), out at the threshold
  minus the hysteresis, at it included; a threshold written absent is inactive from the
  next reading. The event goes out before the report of the same reading.
- **A device without `ThresholdHysteresis` uses the quantity's report hysteresis**, and
  the row's default is the same: the sensor's noise is one, whoever listens to it.
- **`MinMeasured`/`MaxMeasured` are stored at most every 10 minutes, counted from the
  start, and at once after `ResetMinMax`** (DEVICE-01 N10; types/0x0030-sense.md §5.7).
  Checked at each reading — a board measures every second or two — so no tick is needed.
  They are `RO,P`: a factory reset leaves them, as `PressCount`.
- **`Offset` is added and the result held to the type's range**; a new `Offset` applies
  from the next reading and recomputes no extremes (DEVICE-01 N11).
- **`lb_sense_init` puts the type's hooks on the endpoint** — `on_command`, `on_check`,
  `feature_of`, `hysteresis`, and `ctx` as the sense — unlike `Display`, whose hooks the
  device wires: a sensor has no callback of the device's to carry.

A measurement taken before the node has an address is not reported when the address
comes — the engine drops what was owed from before (TASK-01) — so a freshly joined sensor
is first heard at its cycle, within `ReportInterval`. Pinned by `sense.json`,
`sense_minmax.json`, `sense_threshold.json`, `sense_light.json`, `sense_restore.json`.

### A screen's number and its refreshes are tools, the glass is the board's (2026-10-13)

The first board with `Display` is an e-paper of segment digits (`devices/apps/epd`,
`zadania/zrobione/DEVICE-04.md`). Two things it decides are not hardware and are worth a
test on a computer, so they are tools here (N12) — and the simulator's `epd` uses them,
so `lagun-sim` shows what the glass would, not the raw number:

- **`tools/digits.h` — the number in its places.** Rounded to the layout's decimals,
  halves away from zero; a minus takes a whole place, a half digit shows only "1"; what
  does not fit is dashes, and so is "absent" (`zadania/zrobione/FEAT-12.md`, N12). The answer
  is characters, not segments: which segments light is the panel's driver's
  (`devices/components/epd1in9`).
- **`tools/epaper.h` — when to refresh, and how.** The first refresh is full and at once
  (the glass still shows the picture from before the power went — FEAT-12 N13); none
  while the picture equals the glass; at most one per `min_gap_ms`, **except while the
  glass shows no number**, so a value after a start or an expiry is never late;
  a full one after `full_every` partial ones (DEVICE-04, N3). `epd` runs with no gap
  (2026-10-14: a minute behind the sensor was too slow to live with) — the gap stays a
  parameter for a screen that needs one.

Rejected: all of it in the board's component — simpler, but not testable on a computer,
against the rule that logic lives here and boards only touch hardware. The pacer knows
nothing of the panel being busy: the board does not ask while it is (N10), and the
simulator's refresh takes no time.

### `ValueInput`: the number is the library's, the detents the board's (2026-10-15)

The first board with `ValueInput` is a knob, an EC11 encoder with a key in its shaft
(`devices/apps/dial`, `zadania/zrobione/DEVICE-02.md`). The split follows `KeyInput`
(CR-01): the library has no hardware, not even a quadrature decoder (N10) — the ESP32-C3
has no pulse counter, so the board decodes the Gray code in an interrupt, divides by its
transitions per detent and hands in "n steps". What the library keeps is what the hub
sees and a driven clock can pin (`vectors/scenarios/value_input*.json`):

- **Several steps at once are one change** (N11): detents counted between two loops go
  out as ONE `Changed` with the summed `delta`. `value` is absolute, so nothing is lost,
  and a fast turn costs fewer frames.
- **Limits** (S2): a step past a limit reaches it; at the limit nothing changes and
  nothing is said; with `Wrap` only a step FROM a limit wraps; `delta` is arithmetic
  (1000 → 0 is −1000).
- **A narrowed range clips `Value`** (S1) — reported, stored, no `Changed`. A written
  `Value` outside the range, `Min > Max`, `StepSize 0` are `INVALID_VALUE`.
- **Stored 5 s after the last step**, `Value` and `ChangeCount` together; a write from the
  hub is stored at once by the node, as every write.
- **A write of a reported attribute is reported** — by the node, for every type, next to
  the reply and under `ReportMinGap` (04-interakcje.md §4.2, N13). Before 2026-10-16 the
  node waited for the cycle; `Value` is the first reported attribute one can write, and
  the type's text said "with the next cycle" against the general rule. `ChangeCount` survives a factory
  reset like `PressCount`; `Value` comes back as `MinValue` — the row says 0 and
  `lb_value_input_init` clips it, so a board does not repeat its `MinValue` twice.
- **The hooks come at init, after the restore**, as in `Sense`: a stored `Value` is
  clipped there rather than refused against a range the node has not restored yet.

Rejected: press/release with hold in the library — hold left the type in CR-04, and no
device needs it; a pair of keys that scrolls on hold calls `lb_value_input_step` from its
own timer.

### `Output` as a light: the library fades, the board turns intent into current (2026-10-16)

The first light is eight WS2812 as one endpoint, `LEVEL | COLOR_RGB`
(`devices/apps/rgb8`, `zadania/zrobione/DEVICE-03.md`). The specification settled the
colour on the way (`types/0x0020-output.md` §5.8, K1–K5): colour at full brightness,
independent of `Level`, persistent, white after a factory reset, one transition for both;
a component is 0–254, as every `u8` (0xFF is "absent"), so `SetColor` refuses 0xFF. Also
settled there: `Step` up from dark lights at the step (at least `MinLevel`), down from dark
does nothing; `On`, `Off` and `Toggle` fade over `DefaultFadeTime`.

- **Two inits, one type.** `lb_output_init` drives a coil (`bool`),
  `lb_output_init_light` a light — `(level ‰, rgbw)`, `rgbw` NULL without `COLOR_RGB`.
  A relay keeps its exact old path; nothing in `output`/`out8` moved.
- **The drive function gets intent.** The level in per mille and the colour at full
  brightness — not PWM duty, not GRB bytes. The brightness curve, the channel order, a
  white channel the LEDs lack, the current cap are the board's (N3, H1): the same split as
  the coil's polarity, and the library stays testable without knowing an LED.
- **Transitions here, not in hardware** (LEDC fade, RMT): one source of truth, checked on
  a computer. Recomputed every `LB_OUT_STEP_MS` (10 ms, N4); a command first works out
  where the light IS, so an interrupted transition starts from there.
- **Attributes written quietly during a transition** — straight into the array, so a read
  sees where the light is, and nothing owed; the end goes through the engine, which
  compares with what was last REPORTED. The light settles its report baselines at init, as
  the node does for `Diagnostics`: otherwise the first `SetLevel` would report the colour
  and `Transitioning` too.
- **`lb_output_set` (a direct state) does not fade** — unlike `On`/`Off`, the caller
  asked for a state, not a command.
  `Toggle` looks at the current `Level`, as the type's table says.
- **Stored together, 5 s after the last change** of the level (for `RESTORE`), `LastLevel`
  or the colour — where the light is going, not each step. `RESTORE` keeps the level in
  `output<ep>` as two bytes; a relay keeps one.
- **Commands are gated by feature** like attributes: `Stop` on a relay is
  `UNSUPPORTED_FEATURE` (04-interakcje.md §9), not `UNSUPPORTED_ELEMENT` as before. A
  separate table, because attributes and commands share numbers, not meanings.

Pinned by `vectors/scenarios/output_level.json`, `output_color.json`,
`output_level_restore.json`, and `output.json` for the relay's refusals.

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
`types/key.h`, `types/output.h`, `types/display.h`, `types/sense.h` and `types/value_input.h`. A type change means those, plus a
scenario. So are the quantities (`lb_quantity_type` in `values.c`, from
`registry/quantities.yaml`, which the generator does not read): `vectors/codec.json`
§ quantities pins every row.

## What is not here yet

Missing in the node. Moved here from `devices/README.md`, because none of it is about
hardware.

**Transactions on the node side:**

- `BUSY` when the node cannot act right now, with the right to retry — the node says it
  only to segment 0 of a second `LONG_WRITE` while one is open (the fault overlay can
  answer `BUSY` on the node's behalf for anything else);
- a prioritised transmit queue: on overflow the oldest `REPORT` goes, never a
  `RESPONSE` or an `EVENT`.

**Life cycle:** the firmware version in `Booted` — raising it should cause
re-enumeration, a restart without a change should not.

**Reports from a port without `health`:** `Diagnostics` reports only what the controller
tells, so on SocketCAN over a plain socket (`lagun-node`) and in the simulator without
`bus` it reports nothing, and the hub's silence threshold falls back to polling.

**Queries:** `QueryEnabled` is stored, but the node never sends a QUERY. `Display` is the
first type with an input (`Value`, types/0x0022-display.md §4a); without an answer a field
shows `NONE`, which the type allows. The hub does not answer queries either — left to
the bindings epic (zadania/zrobione/FEAT-13.md N10).

**Strings in the store:** `lb_node_persist` encodes scalars only; a persistent `string`
attribute would not be stored. None exists (`Display.Text` is `V`).

**Groups:** the node declares `GroupCapacity = 0` and ignores broadcast frames.
