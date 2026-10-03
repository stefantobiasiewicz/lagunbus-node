/* The bus — the only place in the library that knows hardware exists.
 *
 * The counterpart of `Bus` on the hub side (lagunhub/protocol/bus/base.py), and it
 * exists for the same reason: development happens where there is no bus. There it
 * was macOS, here it is the machine the firmware is built on.
 *
 * Two implementations from day one: `ports/posix` over SocketCAN and `ports/esp32c3`
 * over TWAI. The first is not a toy — it is a node that can be plugged into `vcan0`
 * and used to check the protocol BEFORE anything reaches a board (README.md).
 */
#ifndef LAGUNBUS_TRANSPORT_H
#define LAGUNBUS_TRANSPORT_H

#include "lagunbus/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bus state as the controller sees it.
 *
 * It lives here rather than with the node because it describes the BUS — and because
 * it is filled in by the port, which knows nothing about the node.
 *
 * **These are not the values of `Diagnostics.BusState`.** The attribute has three
 * (`LB_DIAG_BUS_*` in node.h); the port has a fourth, `WARNING`, which the specification
 * covers with `ErrorThreshold` instead, and the node maps it to `ACTIVE` on the way out.
 * Until 2026-10-03 the node sent these as they were, so PASSIVE arrived as 2 — `BUS_OFF`
 * to anyone reading the specification (zadania/CR-05.md, vectors/scenarios/bus_state.json).
 */
#define LB_BUS_ACTIVE   0x00u
#define LB_BUS_WARNING  0x01u
#define LB_BUS_PASSIVE  0x02u
#define LB_BUS_OFF      0x03u

/** The bus. The port sets these pointers; the library only calls them. */
typedef struct lb_transport {
    /** Send a frame. `false` means "queue full" — **this is not an error**.
     *
     * The node is then supposed to drop the frame and count it, not wait: blocking on
     * a send would stall the handling of whatever is arriving right now. Retries are
     * part of a transaction and are the layer above's business
     * (04-interakcje.md §2.1).
     */
    bool (*send)(void *ctx, const lb_frame_t *frame);

    /** Receive one frame. `false` means "nothing waiting".
     *
     * **Never blocks.** The node loop has to get back to `lb_node_tick()` even when
     * the bus is silent — otherwise deadlines stand still.
     */
    bool (*recv)(void *ctx, lb_frame_t *out);

    /** Bus health for `Diagnostics` — **optional**, may be left NULL.
     *
     * It is separate because only the controller holds this knowledge, and only some
     * ports can get at it: TWAI on the ESP32 reports error counters and state,
     * SocketCAN over a plain socket does not. A port that cannot leaves NULL, and the
     * node then answers "I don't have it" instead of an invented number.
     */
    bool (*health)(void *ctx, uint16_t *tec, uint16_t *rec, uint8_t *state);

    void *ctx;
} lb_transport_t;

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_TRANSPORT_H */
