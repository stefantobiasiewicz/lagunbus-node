"""Transports for a node that runs on a computer — the Python side of `c/ports/posix`.

A board has TWAI; a simulator (`lagun-sim`, zadania/zrobione/FEAT-02.md) has these two:

- `socketcan` — a real CAN interface or `vcan0`, Linux only;
- `udp` — UDP multicast on this machine, for running with a hub started with
  `lagun-hub run --udp`. On macOS, the only way.

Both satisfy `interfaces.Transport` and nothing more; the node cannot tell them apart.
"""
