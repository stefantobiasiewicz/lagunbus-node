"""LagunBus node library, the Python side — the tools AROUND a node, not a node.

The node is the C library in `../c`, the one that goes onto a board. Python reaches it
through `sim` (the C library behind `ctypes`, with its catalogue of devices), and has
what a simulator and the tests need around it: the frame codec (`frame`), the registry,
the fault overlay (`faults`) and transports for a computer (`ports`). Why the split is
here: zadania/zrobione/FEAT-02.md, N10–N11; README.md § Decisions.

It has no line in common with the hub (zadania/zrobione/FEAT-03.md).
"""
