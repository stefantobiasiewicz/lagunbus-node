/* The node — the loop that receives frames and answers them.
 *
 * **There is no thread and no task here.** `lb_node_tick()` is an ordinary function;
 * who calls it, and from what, belongs to the port. On Linux a `while` loop does it,
 * on the ESP32 a FreeRTOS task — and the library does not need to know.
 */
#ifndef LAGUNBUS_NODE_H
#define LAGUNBUS_NODE_H

#include "lagunbus/endpoint.h"
#include "lagunbus/platform.h"
#include "lagunbus/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_UID_LEN 8

/** Reason for the last restart — `LastResetReason`, the table of types/0x0001-device.md
 * §2.4. Until 2026-10-03 these were 0…3 with `SOFTWARE` at 1 and `BROWNOUT` at 3, which
 * the specification calls `EXTERNAL` and `SOFTWARE`; nothing set them, so the node only
 * ever said `POWER_ON` and the mistake stayed invisible (zadania/zrobione/CR-05.md). Pinned by
 * vectors/scenarios/reset_reason.json. */
#define LB_RESET_POWER_ON  0x00u
#define LB_RESET_EXTERNAL  0x01u
#define LB_RESET_WATCHDOG  0x02u
#define LB_RESET_SOFTWARE  0x03u
#define LB_RESET_BROWNOUT  0x04u
#define LB_RESET_UNKNOWN   0x05u

/** `Device` (ep0) elements the hub needs while identifying a node. */
#define LB_DEV_UID               0x00u
#define LB_DEV_SHORT_ID          0x01u
#define LB_DEV_DEVICE_TYPE       0x02u
#define LB_DEV_PROTOCOL_VERSION  0x03u
#define LB_DEV_FIRMWARE_VERSION  0x04u
#define LB_DEV_HARDWARE_VERSION  0x05u
#define LB_DEV_VENDOR_ID         0x06u
#define LB_DEV_ENDPOINT_COUNT    0x07u
#define LB_DEV_UPTIME            0x10u
#define LB_DEV_BOOT_COUNT        0x11u
#define LB_DEV_LAST_RESET        0x12u
#define LB_DEV_EVENT_BOOTED      0x01u

/* `Device` commands (types/0x0001-device.md §3). */
#define LB_DEV_CMD_IDENTIFY      0x00u
#define LB_DEV_CMD_REBOOT        0x01u
#define LB_DEV_CMD_FACTORY_RESET 0x02u
/** The argument without which `FactoryReset` is refused — the characters `LAGN`. A single
 * flipped bit must not wipe a node in the wall (types/0x0001-device.md §3.3). */
#define LB_FACTORY_RESET_MAGIC   0x4C41474Eu
/** How long after acknowledging a reset the node carries it out. The acknowledgement is
 * only QUEUED when the handler returns; restarting at once would lose it, and the hub
 * would not know whether the reset happened. */
#define LB_RESET_GRACE_MS        50u

/** The deduplication buffer (04-interakcje.md §2.2): a command repeated with the same
 * `Tid`, endpoint and command number within `LB_DEDUP_WINDOW_MS` of being carried out is
 * answered from here and not carried out again.
 *
 * The specification asks for at least eight entries and enough for every command of the
 * window. Eight is NOT enough here: the hub counts `Tid` per endpoint and command, so an
 * `out8` whose eight outputs one key switches at once gets eight commands in one go, and
 * a ninth right after would push the first out while its retry may still come
 * (README § Decisions, zadania/zrobione/BUG-02.md). Sixteen holds two such series — "two per
 * endpoint that takes commands", as the specification suggests. A device with more
 * endpoints sets it at compile time. 8 bytes an entry. */
#ifndef LB_DEDUP_ENTRIES
#define LB_DEDUP_ENTRIES   16u
#endif
#define LB_DEDUP_WINDOW_MS 300u

/** `Diagnostics` (ep1) and `Groups` (ep2) elements.
 *
 * These two endpoints are served by **the node itself**, just like `Device`: they are
 * identical in every device, and their values come from the node's own counters
 * anyway. Were they to sit in the application's array, every application would be
 * rewriting the same thing.
 */
#define LB_DIAG_TX_ERROR_COUNT   0x00u
#define LB_DIAG_RX_ERROR_COUNT   0x01u
#define LB_DIAG_BUS_STATE        0x02u
#define LB_DIAG_BUS_OFF_COUNT    0x03u
#define LB_DIAG_TX_FRAMES        0x04u
#define LB_DIAG_RX_FRAMES        0x05u
#define LB_DIAG_TX_DROPPED       0x06u
#define LB_DIAG_ERROR_THRESHOLD  0x20u
#define LB_DIAG_CMD_RESET        0x00u

/** `Diagnostics.BusState` on the wire (types/0x0002-diagnostics.md §2.4) — not the
 * port's `LB_BUS_*`, which has a `WARNING` the attribute does not (transport.h). */
#define LB_DIAG_BUS_ACTIVE       0x00u
#define LB_DIAG_BUS_PASSIVE      0x01u
#define LB_DIAG_BUS_OFF          0x02u

#define LB_GROUPS_CAPACITY  0x00u
#define LB_GROUPS_COUNT     0x01u

/** System endpoint numbers — the same in every device (01-model-danych.md §2). */
#define LB_EP_DEVICE       0u
#define LB_EP_DIAGNOSTICS  1u
#define LB_EP_GROUPS       2u
#define LB_EP_FIRST_USER   4u

/** Node class — **nothing but the choice of address block** (03-adresacja.md §2.3).
 *
 * It is not a description of the device and not a permission. The hub reads it from
 * the ANNOUNCEMENT and only to know which pool to take an address from.
 */
#define LB_CLASS_UNSPECIFIED  0x00u  /* 0x400..0x7FD */
#define LB_CLASS_CONTROL      0x01u  /* 0x001..0x0FF */
#define LB_CLASS_SENSOR       0x02u  /* 0x100..0x3FF */

/** Segments (05-mapowanie-can.md §6). Byte [0] = LAST|Tid, [1] = number,
 * [2..7] = data. */
#define LB_SEGMENT_DATA  6u
#define LB_SEGMENT_LAST  0x80u

/** Identity and whatever the hub reads from `Device`. All of it fixed for the
 * node’s entire life. */
typedef struct {
    uint8_t  uid[LB_UID_LEN];
    uint8_t  node_class;
    uint16_t device_type;
    uint16_t vendor_id;
    uint32_t firmware;
    uint16_t hardware;
    uint16_t protocol;
} lb_identity_t;

/** Traffic watch — called for EVERY frame, outgoing and incoming alike.
 *
 * It is here because the library has no business knowing that `ESP_LOGI` or `printf`
 * exist: on the board the log goes over USB, on Linux to the screen, and in a test
 * nowhere at all. Who prints it, and how, belongs to the application.
 *
 * `outgoing` tells "we sent it" apart from "we heard it". Incoming frames are shown
 * ALL of them, other nodes' included — with three nodes on the bus that is exactly
 * what one wants to see during a first bring-up.
 */
typedef void (*lb_watch_fn)(void *ctx, const lb_frame_t *frame, bool outgoing);

/** What the node is doing, as a person standing next to it should see it — the status
 * LED every device has (zadania/zrobione/CR-05.md).
 *
 * **One state at a time**, the most important one, by a fixed priority from the top of
 * this list down. Several things can be true at once — no address and a dead bus, say —
 * and a single LED can show only one of them; deciding which in the library means every
 * device shows the same thing in the same situation. A device that wants more (an RGB
 * LED showing the bus colour AND blinking for `Identify`) can still read the node's
 * fields; nothing here stops it.
 *
 * There is no "joined" state between the two: `accept_assignment` stores the address
 * and sends `Booted` in the same call, so no tick ever sees it. Nor is there "no hub":
 * the node does not know the hub's address, and a node alone on the cable gets no ACKs
 * and goes error passive anyway — which `BUS_FAULT` shows.
 */
typedef enum {
    LB_STATE_RESETTING = 0, /**< a factory reset or `Reboot` accepted; the restart is next */
    LB_STATE_IDENTIFY,      /**< `Identify` running — "it's me" */
    LB_STATE_BUS_FAULT,     /**< the controller is error passive or bus-off */
    LB_STATE_NO_ADDRESS,    /**< no address — announcing itself */
    LB_STATE_READY,         /**< an address, `Booted` sent, working */
} lb_node_state_t;

/** State watch — called when the state CHANGES, and once from `lb_node_start` with the
 * state it starts in, so an LED does not wait for the first change.
 *
 * Called from inside `lb_node_tick` (or `lb_node_start`, `lb_node_factory_reset`), so it
 * **must not block**: set a pattern and return; the blinking is counted out by the
 * device's loop (`lagunbus/tools/led.h` does exactly that). NULL — nobody looks. */
typedef void (*lb_state_fn)(void *ctx, lb_node_state_t state);

/** The reporting settings of one system endpoint.
 *
 * A system endpoint has the full set of common attributes (01-model-danych.md §4.1).
 * `Channel` and `Enabled` are fixed there and `QueryEnabled` is 0, so these three are
 * all that can actually differ — and the silence threshold is worked out from the
 * `ReportInterval` of `Diagnostics` (06-cykl-zycia-urzadzenia.md §5.1).
 */
typedef struct {
    uint16_t report_interval;
    uint16_t report_min_gap;
    uint16_t report_hysteresis;
} lb_reporting_t;

/** One command carried out with `OK` — the key and when.
 *
 * No result: only `OK` goes in (README § Decisions), and a command reply carries no
 * data, so the remembered reply is always the bare `OK`. */
typedef struct {
    uint32_t at_ms;    /**< when it was CARRIED OUT; a retry does not move it */
    uint8_t  tid;
    uint8_t  endpoint;
    uint8_t  command;
    bool     used;
} lb_dedup_t;

typedef struct {
    lb_transport_t *bus;
    lb_platform_t  *platform;
    lb_identity_t   identity;

    /** **Functional** endpoints, from number 4 up. `Device` is served by the node. */
    lb_endpoint_t  *endpoints;
    uint8_t         endpoint_count;

    uint16_t address;
    uint8_t  reset_reason;
    uint16_t boot_count;

    /* Joining (03-adresacja.md §5). A node that has an address never announces. */
    uint32_t next_join_ms;
    uint16_t join_backoff_s; /**< the current window of the random delay: 2, 10 or 60 s */
    uint16_t join_attempts;

    uint32_t started_ms;
    bool     booted;

    /** A factory reset or a `Reboot` acknowledged and not yet carried out — see
     * `LB_RESET_GRACE_MS`. `reset_wipe` tells the two apart: only a factory reset
     * forgets the address. */
    bool     reset_pending;
    bool     reset_wipe;
    uint32_t reset_at_ms;

    /** Commands of the last `LB_DEDUP_WINDOW_MS` — see `LB_DEDUP_ENTRIES`. */
    lb_dedup_t dedup[LB_DEDUP_ENTRIES];

    /** `Identify` runs while this is in the future (modulo 2^32, like every deadline). */
    bool     identifying;
    uint32_t identify_until_ms;

    /** The controller's state, read through `transport->health` at every tick —
     * `LB_BUS_ACTIVE` when the port cannot tell. */
    uint8_t  bus_state;

    /* Counters — for Diagnostics, and for seeing whether anything works at all. */
    uint32_t received;
    uint32_t sent;
    uint32_t dropped;
    uint8_t  device_event_seq;

    /** `Diagnostics.ErrorThreshold` — the only setting of the system types themselves. */
    uint16_t error_threshold;
    /** Common reporting attributes of ep0..ep2 — indexed by endpoint number. */
    lb_reporting_t system_reporting[LB_EP_GROUPS + 1u];
    uint16_t bus_off_count;

    /** Traffic watch. NULL means "do not look" and costs nothing. */
    lb_watch_fn on_frame;
    void       *watch_ctx;

    /** State watch, for the status LED. NULL means "do not look". Set it before
     * `lb_node_start`, as `on_frame`. */
    lb_state_fn     on_state;
    void           *state_ctx;
    lb_node_state_t state;       /**< the last state reported */
    bool            state_known; /**< false until the first report */
} lb_node_t;

/** Set the node up. Does not touch the bus — the start-up order lives in
 * `lb_node_start`. */
void lb_node_init(lb_node_t *node, lb_transport_t *bus, lb_platform_t *platform,
                  const lb_identity_t *identity, lb_endpoint_t *endpoints, uint8_t count);

/** `ShortId` — CRC-32 of `Uid` (types/0x0001-device.md §2.1).
 *
 * A digest, not an identity: it serves to tell nodes apart where a full `Uid` does not
 * fit — above all while joining a node that has no address yet. */
uint32_t lb_node_short_id(const lb_node_t *node);

/** Start: load the address from the persistent store and announce `Booted` **only
 * then**.
 *
 * The order is not arbitrary (types/0x0001-device.md §5.1): configuration, endpoints,
 * bus, and `Booted` last. The hub treats it as a declaration of readiness — a node
 * that announces earlier will get a read it cannot answer.
 */
void lb_node_start(lb_node_t *node);

/** One turn of the loop: take in whatever is waiting, and answer. Never blocks. */
void lb_node_tick(lb_node_t *node);

/** Factory reset from the device itself — the service button (zadania/zrobione/CR-05.md).
 *
 * The same path as `FactoryReset` from the hub, minus the reply: it is only ACCEPTED here
 * and carried out in `lb_node_tick` `LB_RESET_GRACE_MS` later, so the state watch gets
 * `LB_STATE_RESETTING` before the restart. How long the button has to be held, and
 * whether it counts at start-up, is the device's decision — the library only hears
 * "reset". */
void lb_node_factory_reset(lb_node_t *node);

/** The state's name — "NO_ADDRESS", "READY", … — for logs and the simulator. */
const char *lb_node_state_name(lb_node_state_t state);

/** Endpoint by number, or NULL. */
lb_endpoint_t *lb_node_endpoint(lb_node_t *node, uint8_t number);

/** Send a value too large for one frame as a run of `LONG_RESPONSE` frames.
 *
 * To the hub this is **still a single read** — what changes is the frame count, not
 * the interaction. Called on its own after a `SEGMENTED` reply; the application does
 * not need to know.
 */
bool lb_node_send_segments(lb_node_t *node, uint8_t endpoint, uint8_t element,
                           uint8_t tid, const uint8_t *value, uint8_t len);

/** Send a REPORT — the bare attribute value, with no header byte.
 *
 * A report is not a transaction: nobody acknowledges it and nobody asked for it.
 * That is why it carries **no `Tid` and no result code** — only the value’s bytes.
 */
bool lb_node_report(lb_node_t *node, uint8_t endpoint, uint8_t element,
                    const uint8_t *value, uint8_t len);

/** Send an event with its sequence number. `params` may be NULL. */
bool lb_node_emit(lb_node_t *node, uint8_t endpoint, uint8_t event,
                  const uint8_t *params, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_NODE_H */
