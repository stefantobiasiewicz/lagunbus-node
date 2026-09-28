"""LagunBus node library — the Python twin of the C library in `../c`.

Same concepts, same split: the protocol here, the world behind two interfaces
(`interfaces.Transport`, `interfaces.Platform`), the clock driven from outside. It has
no line in common with the hub (zadania/FEAT-03.md). The two languages are held together
by the conformance tests in `tests/conformance/`, which run the same vectors and
scenarios through both.
"""
