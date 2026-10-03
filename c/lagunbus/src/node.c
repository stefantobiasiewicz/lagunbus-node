#include "lagunbus/node.h"

#include "lagunbus/registry.h"

#include <string.h>

/* The key under which the assigned address lives in persistent storage. Renamed from
 * the Polish "adres" before version 0.5: a node flashed earlier does not find its old
 * address, comes back without one and announces itself anew — the hub readmits a
 * known ShortId by itself. */
#define NVS_KEY_ADDRESS "address"

static void schedule_announce(lb_node_t *node);
static void announce_booted(lb_node_t *node);
static void system_defaults(lb_node_t *node);
static void read_health(lb_node_t *node);
static void report_state(lb_node_t *node);

void lb_node_init(lb_node_t *node, lb_transport_t *bus, lb_platform_t *platform,
                  const lb_identity_t *identity, lb_endpoint_t *endpoints, uint8_t count)
{
    memset(node, 0, sizeof *node);
    node->bus = bus;
    node->platform = platform;
    node->identity = *identity;
    node->endpoints = endpoints;
    node->endpoint_count = count;
    node->reset_reason = LB_RESET_POWER_ON;
    system_defaults(node);
}

/** The settings of the system types themselves — at start, and again at a factory
 * reset. */
static void system_defaults(lb_node_t *node)
{
    memset(node->system_reporting, 0, sizeof node->system_reporting);
    node->error_threshold = 96; /* default from the Diagnostics type definition */
    /* Defaults from types/0x0002-diagnostics.md §5.4: counters jitter at every frame,
     * so a report every change would load the bus the diagnostics is meant to watch. */
    node->system_reporting[LB_EP_DIAGNOSTICS].report_interval = 300;
    node->system_reporting[LB_EP_DIAGNOSTICS].report_hysteresis = 16;
}

uint32_t lb_node_short_id(const lb_node_t *node)
{
    /* CRC-32 over the whole Uid — that is what types/0x0001-device.md §2.1 lays down,
     * where ShortId is a MANDATORY attribute with exactly this definition.
     *
     * This used to be "the four high bytes of Uid". It worked, but only because the
     * ESP32 port reverses the MAC so that the unique bytes land at the front — which
     * made the correctness of ShortId depend on how ONE port assembles a Uid. The next
     * port (STM32) would assemble it differently, end up with an identical ShortId in
     * every node, and then announcements destroy each other in arbitration instead of
     * queueing up (03-adresacja.md §5.4). CRC-32 mixes every byte, so the layout of a
     * Uid stops mattering.
     *
     * The table-free variant: 8 bytes x 8 rounds is nothing, while a table would cost
     * a kilobyte of flash for something computed once per announcement and once per
     * read. Reversed polynomial 0xEDB88320 — the same CRC-32 as in zlib and in the
     * hub. */
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < LB_UID_LEN; i++) {
        crc ^= node->identity.uid[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

lb_endpoint_t *lb_node_endpoint(lb_node_t *node, uint8_t number)
{
    for (uint8_t i = 0; i < node->endpoint_count; i++) {
        if (node->endpoints[i].number == number) {
            return &node->endpoints[i];
        }
    }
    return NULL;
}

static bool send_frame(lb_node_t *node, uint32_t can_id, const uint8_t *data, uint8_t len)
{
    lb_frame_t frame = {0};
    frame.can_id = can_id;
    frame.len = len;
    if (len > 0) {
        memcpy(frame.data, data, len);
    }
    if (node->bus->send(node->bus->ctx, &frame)) {
        node->sent++;
        if (node->on_frame != NULL) {
            node->on_frame(node->watch_ctx, &frame, true);
        }
        return true;
    }
    /* Queue full. We drop it and count — waiting would stall reception, and a late
     * frame is worse than a missing one. */
    node->dropped++;
    return false;
}

bool lb_node_send_segments(lb_node_t *node, uint8_t endpoint, uint8_t element,
                           uint8_t tid, const uint8_t *value, uint8_t len)
{
    uint32_t can_id = 0;
    if (!lb_encode_id(LB_KIND_LONG_RESPONSE, node->address, endpoint, element, &can_id)) {
        return false;
    }

    uint8_t num = 0;
    uint8_t sent_frames = 0;
    while (sent_frames < len) {
        uint8_t chunk = (uint8_t)(len - sent_frames);
        if (chunk > LB_SEGMENT_DATA) {
            chunk = LB_SEGMENT_DATA;
        }
        bool last_raw = (uint8_t)(sent_frames + chunk) >= len;

        uint8_t payload[LB_MAX_PAYLOAD];
        payload[0] = (uint8_t)((last_raw ? LB_SEGMENT_LAST : 0u) | (tid & 0x07u));
        payload[1] = num;
        memcpy(payload + 2, value + sent_frames, chunk);

        if (!send_frame(node, can_id, payload, (uint8_t)(chunk + 2u))) {
            /* To the hub a broken run is the same as a lost segment: it discards the
             * value entirely and repeats the WHOLE request (05-mapowanie-can.md §6.6).
             * Sending the remainder after a gap would be worse than silence. */
            return false;
        }
        sent_frames = (uint8_t)(sent_frames + chunk);
        num++;
    }
    return true;
}

bool lb_node_report(lb_node_t *node, uint8_t endpoint, uint8_t element,
                    const uint8_t *value, uint8_t len)
{
    uint32_t can_id = 0;
    if (!node->booted || node->address == LB_ADDRESS_UNASSIGNED
        || !lb_encode_id(LB_KIND_REPORT, node->address, endpoint, element, &can_id)) {
        return false;
    }
    return send_frame(node, can_id, value, len);
}

bool lb_node_emit(lb_node_t *node, uint8_t endpoint, uint8_t event,
                  const uint8_t *params, uint8_t len)
{
    if (!node->booted || len + 1u > LB_MAX_PAYLOAD) {
        return false;
    }
    uint8_t *seq = endpoint == 0
                 ? &node->device_event_seq
                 : (lb_node_endpoint(node, endpoint) != NULL
                        ? &lb_node_endpoint(node, endpoint)->event_seq
                        : NULL);
    if (seq == NULL) {
        return false;
    }

    uint8_t payload[LB_MAX_PAYLOAD];
    payload[0] = (*seq)++;  /* the sequence number goes FIRST (01 §3.3) */
    if (len > 0) {
        memcpy(payload + 1, params, len);
    }

    uint32_t can_id = 0;
    if (!lb_encode_id(LB_KIND_EVENT, node->address, endpoint, event, &can_id)) {
        return false;
    }
    return send_frame(node, can_id, payload, (uint8_t)(len + 1u));
}

static void announce_booted(lb_node_t *node)
{
    uint8_t params[5];
    params[0] = node->reset_reason;
    lb_encode(LB_T_U32, LB_NUM((int32_t)node->identity.firmware), params + 1, 4);
    lb_node_emit(node, 0, LB_DEV_EVENT_BOOTED, params, sizeof params);
}

void lb_node_start(lb_node_t *node)
{
    node->started_ms = node->platform->millis(node->platform->ctx);
    /* Only the chip knows why it started; without the hook POWER_ON is the honest guess
     * on a host, where every start is one. */
    if (node->platform->reset_reason != NULL) {
        node->reset_reason = node->platform->reset_reason(node->platform->ctx);
    }

    /* 1. Configuration: the address from the persistent store. Its absence is the
     *    normal state of a first boot — the node will then announce itself. */
    uint16_t saved = 0;
    if (node->platform->load != NULL
        && node->platform->load(node->platform->ctx, NVS_KEY_ADDRESS, &saved,
                                sizeof saved)) {
        node->address = saved;
    }
    if (node->platform->load != NULL) {
        node->platform->load(node->platform->ctx, "boot", &node->boot_count,
                             sizeof node->boot_count);
    }
    node->boot_count++;
    if (node->platform->store != NULL) {
        node->platform->store(node->platform->ctx, "boot", &node->boot_count,
                              sizeof node->boot_count);
    }

    /* 2. Only now may we answer anything. */
    node->booted = true;

    /* 3. And only now do we announce ourselves to the hub — not a moment earlier.
     *
     *    A node with no address has nothing to announce with and must ask for an
     *    address first. A node that has one does NOT announce at all: changing a fuse
     *    must not require a ceremony (06-cykl-zycia-urzadzenia.md §2.1). */
    if (node->address != LB_ADDRESS_UNASSIGNED) {
        announce_booted(node);
    } else {
        schedule_announce(node);
    }

    /* 4. The first state report, so the LED does not wait for the first change — and the
     *    bus read first, so a node alone on a dead cable starts as BUS_FAULT rather than
     *    flashing NO_ADDRESS for one tick. */
    read_health(node);
    report_state(node);
}

/** How long a node waits for an ASSIGNMENT before it may try again (03-adresacja.md
 * §5.3, phase 2). */
#define LB_JOIN_WAIT_MS 500u

/** The window of the random delay before attempt `attempt` (1, 2, …), in seconds —
 * the table of 03-adresacja.md §5.6: 2 s for 1–5, 10 s for 6–10, 60 s from then on. */
static uint16_t join_window_s(uint16_t attempt)
{
    return attempt <= 5u ? 2u : attempt <= 10u ? 10u : 60u;
}

/** Schedule the next announcement.
 *
 * The delay is **random**, and that is not decoration: after power comes back every
 * node starts within the same second. Without the spread they announce simultaneously,
 * collide on the bus and retry — just as much in unison (03-adresacja.md §5.3).
 *
 * The windows grow by the table of §5.6, because the hub may be off for hours, and a
 * node asking every second occupies the bus all that time to no purpose whatsoever.
 *
 * After an announcement the node first waits `LB_JOIN_WAIT_MS` for its assignment, and
 * only then draws the delay — so two announcements are never closer than that,
 * whatever the draw. Until 2026-10-01 this wait was missing and the windows were
 * 1, 2, 4 … 60 s: with an unlucky draw (and in the simulator, whose `random` is fixed,
 * with a draw of 0) a node announced on every turn of its loop (zadania/zrobione/FEAT-02.md,
 * F8). Pinned by vectors/scenarios/join.json and join_backoff.json.
 *
 * `join_backoff_s` keeps the current window for whoever logs it (the boards do).
 */
static void schedule_announce(lb_node_t *node)
{
    uint32_t now = node->platform->millis(node->platform->ctx);
    uint32_t jitter = node->platform->random != NULL
                    ? node->platform->random(node->platform->ctx)
                    : 0u;

    node->join_backoff_s = join_window_s((uint16_t)(node->join_attempts + 1u));
    uint32_t window = (uint32_t)node->join_backoff_s * 1000u;
    uint32_t wait = node->join_attempts == 0 ? 0u : LB_JOIN_WAIT_MS;
    node->next_join_ms = now + wait + (jitter % window);
}

/** ANNOUNCEMENT: ShortId in the identifier and in the payload, plus the node class.
 *
 * The hub compares the **full 32 bits from the payload** against the 24 bits from the
 * identifier and rejects the frame when they disagree — the identifier only carries
 * the three low bytes.
 */
static void announce(lb_node_t *node)
{
    uint32_t short_id = lb_node_short_id(node);
    uint8_t payload[5];
    lb_encode(LB_T_U32, LB_NUM((int32_t)short_id), payload, 4);
    payload[4] = node->identity.node_class;

    send_frame(node, lb_join_id(short_id, true), payload, sizeof payload);
    node->join_attempts++;
    schedule_announce(node);
}

/** An ASSIGNMENT from the hub: ShortId (4 B) + address (u16). */
static void accept_assignment(lb_node_t *node, const lb_frame_t *frame)
{
    if (node->address != LB_ADDRESS_UNASSIGNED || frame->len < 6) {
        return; /* we have an address, or the frame is too short to be believed */
    }
    lb_value_t who, address;
    if (!lb_decode(LB_T_U32, frame->data, 4, &who)
        || !lb_decode(LB_T_U16, frame->data + 4, 2, &address)) {
        return;
    }
    if ((uint32_t)who.value != lb_node_short_id(node)) {
        return; /* somebody else’s assignment — the bus carries them all */
    }

    node->address = (uint16_t)address.value;
    if (node->platform->store != NULL) {
        node->platform->store(node->platform->ctx, NVS_KEY_ADDRESS, &node->address,
                              sizeof node->address);
    }
    /* Only with an address can the node announce itself — Booted travels on an
     * ordinary identifier, so without an address there is nothing to address it with. */
    announce_booted(node);
}

/** `Device` attributes — from the node's identity, not from the array.
 *
 * `Uid` is the exception: eight bytes plus a length prefix, so it **does not fit into
 * a frame**. The answer is `SEGMENTED`, an announcement after which the segments follow —
 * the node sends them right after the reply.
 */
static uint8_t device_read(lb_node_t *node, uint8_t element,
                           uint8_t *out, uint8_t cap, uint8_t *len)
{
    *len = 0;
    switch (element) {
    case LB_DEV_UID:
        return LB_RESULT_SEGMENTED;
    case LB_DEV_SHORT_ID:
        *len = lb_encode(LB_T_U32, LB_NUM((int32_t)lb_node_short_id(node)), out, cap);
        break;
    case LB_DEV_DEVICE_TYPE:
        *len = lb_encode(LB_T_U16, LB_NUM(node->identity.device_type), out, cap);
        break;
    case LB_DEV_PROTOCOL_VERSION:
        *len = lb_encode(LB_T_U16, LB_NUM(node->identity.protocol), out, cap);
        break;
    case LB_DEV_FIRMWARE_VERSION:
        *len = lb_encode(LB_T_U32, LB_NUM((int32_t)node->identity.firmware), out, cap);
        break;
    case LB_DEV_HARDWARE_VERSION:
        *len = lb_encode(LB_T_U16, LB_NUM(node->identity.hardware), out, cap);
        break;
    case LB_DEV_VENDOR_ID:
        *len = lb_encode(LB_T_U16, LB_NUM(node->identity.vendor_id), out, cap);
        break;
    case LB_DEV_ENDPOINT_COUNT:
        *len = lb_encode(LB_T_U8, LB_NUM(node->endpoint_count), out, cap);
        break;
    case LB_DEV_UPTIME: {
        uint32_t now = node->platform->millis(node->platform->ctx);
        *len = lb_encode(LB_T_U32, LB_NUM((int32_t)((now - node->started_ms) / 1000u)),
                         out, cap);
        break;
    }
    case LB_DEV_BOOT_COUNT:
        *len = lb_encode(LB_T_U16, LB_NUM(node->boot_count), out, cap);
        break;
    case LB_DEV_LAST_RESET:
        *len = lb_encode(LB_T_ENUM8, LB_NUM(node->reset_reason), out, cap);
        break;
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

/** The attributes of `Diagnostics` (ep1).
 *
 * The frame counters are real — the node keeps them anyway. The controller's error
 * counters (`TxErrorCount`, `RxErrorCount`, `BusState`) are known only to the
 * controller itself, so they come from `transport->health()`: TWAI on the ESP32
 * reports them, SocketCAN over a plain socket does not. When a port cannot, the answer
 * is `UNAVAILABLE` — "I support this, but right now I do not know" — rather than a
 * zero pretending the bus is healthy. A zero drawn on a chart looks like an absence of
 * errors and is worse than an absence of data.
 */
static uint8_t diagnostics_read(lb_node_t *node, uint8_t element,
                                uint8_t *out, uint8_t cap, uint8_t *len)
{
    uint16_t tec = 0, rec = 0;
    uint8_t state = LB_BUS_ACTIVE;
    bool known = node->bus->health != NULL
              && node->bus->health(node->bus->ctx, &tec, &rec, &state);

    *len = 0;
    switch (element) {
    case LB_DIAG_TX_ERROR_COUNT:
        if (!known) {
            return LB_RESULT_UNAVAILABLE;
        }
        *len = lb_encode(LB_T_U16, LB_NUM(tec), out, cap);
        break;
    case LB_DIAG_RX_ERROR_COUNT:
        if (!known) {
            return LB_RESULT_UNAVAILABLE;
        }
        *len = lb_encode(LB_T_U16, LB_NUM(rec), out, cap);
        break;
    case LB_DIAG_BUS_STATE:
        if (!known) {
            return LB_RESULT_UNAVAILABLE;
        }
        *len = lb_encode(LB_T_ENUM8,
                         LB_NUM(state == LB_BUS_OFF       ? LB_DIAG_BUS_OFF
                                : state == LB_BUS_PASSIVE ? LB_DIAG_BUS_PASSIVE
                                                          : LB_DIAG_BUS_ACTIVE),
                         out, cap);
        break;
    case LB_DIAG_BUS_OFF_COUNT:
        *len = lb_encode(LB_T_U16, LB_NUM(node->bus_off_count), out, cap);
        break;
    case LB_DIAG_TX_FRAMES:
        *len = lb_encode(LB_T_U32, LB_NUM((int32_t)node->sent), out, cap);
        break;
    case LB_DIAG_RX_FRAMES:
        *len = lb_encode(LB_T_U32, LB_NUM((int32_t)node->received), out, cap);
        break;
    case LB_DIAG_TX_DROPPED:
        *len = lb_encode(LB_T_U16, LB_NUM((int32_t)(node->dropped & 0xFFFFu)), out, cap);
        break;
    case LB_DIAG_ERROR_THRESHOLD:
        *len = lb_encode(LB_T_U16, LB_NUM(node->error_threshold), out, cap);
        break;
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

/** The attributes of `Groups` (ep2).
 *
 * `GroupCapacity = 0` means "this device does not do groups" and is a **valid
 * answer**, not a gap. The hub reads the capacity before it assigns anything, so zero
 * settles the matter without pretending. Groups are not supported yet.
 */
static uint8_t groups_read(lb_node_t *node, uint8_t element,
                           uint8_t *out, uint8_t cap, uint8_t *len)
{
    (void)node;
    *len = 0;
    if (element != LB_GROUPS_CAPACITY && element != LB_GROUPS_COUNT) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    *len = lb_encode(LB_T_U8, LB_NUM(0), out, cap);
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

/** Common attributes of the system endpoints — the same set as in `lb_endpoint_read`
 * (01-model-danych.md §4.1), except there is no endpoint description to take them
 * from, because the system ones are not in the array.
 *
 * `Channel` is always 0 and `Enabled` always 1: a system endpoint is not a function of
 * any channel, and a disabled `Device` or `Diagnostics` would cut the hub off from the
 * node's identity or from noticing that it has gone quiet.
 */
static uint8_t system_common_read(lb_node_t *node, uint8_t endpoint, uint8_t element,
                                  uint8_t *out, uint8_t cap, uint8_t *len)
{
    static const uint16_t type_ids[] = {LB_TYPE_DEVICE, LB_TYPE_DIAGNOSTICS, LB_TYPE_GROUPS};
    const lb_reporting_t *reporting = &node->system_reporting[endpoint];
    *len = 0;
    switch (element) {
    case LB_COMMON_TYPEID:
        *len = lb_encode(LB_T_U16, LB_NUM(type_ids[endpoint]), out, cap);
        break;
    case LB_COMMON_TYPEREVISION:
        *len = lb_encode(LB_T_U8, LB_NUM(1), out, cap);
        break;
    case LB_COMMON_FEATURES:
    case LB_COMMON_QUERYENABLED:
        *len = lb_encode(LB_T_MAP16, LB_NUM(0), out, cap);
        break;
    case LB_COMMON_ENABLED:
        *len = lb_encode(LB_T_BOOL, LB_NUM(1), out, cap);
        break;
    case LB_COMMON_CHANNEL:
        *len = lb_encode(LB_T_U8, LB_NUM(0), out, cap);
        break;
    case LB_COMMON_REPORTINTERVAL:
        *len = lb_encode(LB_T_U16, LB_NUM(reporting->report_interval), out, cap);
        break;
    case LB_COMMON_REPORTMINGAP:
        *len = lb_encode(LB_T_U16, LB_NUM(reporting->report_min_gap), out, cap);
        break;
    case LB_COMMON_REPORTHYSTERESIS:
        *len = lb_encode(LB_T_U16, LB_NUM(reporting->report_hysteresis), out, cap);
        break;
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

/** Writing the common attributes of a system endpoint.
 *
 * Only the reporting settings take a new value. `Enabled` accepts 1, which changes
 * nothing, and refuses 0 with `INVALID_VALUE` — the attribute is writable, just not
 * to that value. `QueryEnabled` is the same: the system types declare no inputs, so
 * the only valid map is 0.
 */
static uint8_t system_common_write(lb_node_t *node, uint8_t endpoint, uint8_t element,
                                   const uint8_t *data, uint8_t len)
{
    lb_reporting_t *reporting = &node->system_reporting[endpoint];
    lb_value_t v;
    switch (element) {
    case LB_COMMON_TYPEID:
    case LB_COMMON_TYPEREVISION:
    case LB_COMMON_FEATURES:
    case LB_COMMON_CHANNEL:
        return LB_RESULT_READ_ONLY;
    case LB_COMMON_ENABLED:
        if (!lb_decode(LB_T_BOOL, data, len, &v) || v.absent || v.value == 0) {
            return LB_RESULT_INVALID_VALUE;
        }
        return LB_RESULT_OK;
    case LB_COMMON_QUERYENABLED:
        if (!lb_decode(LB_T_MAP16, data, len, &v) || v.absent || v.value != 0) {
            return LB_RESULT_INVALID_VALUE;
        }
        return LB_RESULT_OK;
    case LB_COMMON_REPORTINTERVAL:
    case LB_COMMON_REPORTMINGAP:
    case LB_COMMON_REPORTHYSTERESIS:
        if (!lb_decode(LB_T_U16, data, len, &v) || v.absent) {
            return LB_RESULT_INVALID_VALUE;
        }
        break;
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    uint16_t value = (uint16_t)v.value;
    if (element == LB_COMMON_REPORTINTERVAL) {
        reporting->report_interval = value;
    } else if (element == LB_COMMON_REPORTMINGAP) {
        reporting->report_min_gap = value;
    } else {
        reporting->report_hysteresis = value;
    }
    return LB_RESULT_OK;
}

static uint8_t system_read(lb_node_t *node, uint8_t endpoint, uint8_t element,
                           uint8_t *out, uint8_t cap, uint8_t *len)
{
    *len = 0;
    if (endpoint > LB_EP_GROUPS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    if (element >= LB_COMMON_BASE) {
        return system_common_read(node, endpoint, element, out, cap, len);
    }
    if (endpoint == LB_EP_DIAGNOSTICS) {
        return diagnostics_read(node, element, out, cap, len);
    }
    return groups_read(node, element, out, cap, len);
}

static uint8_t system_write(lb_node_t *node, uint8_t endpoint, uint8_t element,
                            const uint8_t *data, uint8_t len)
{
    if (endpoint > LB_EP_GROUPS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    if (element >= LB_COMMON_BASE) {
        return system_common_write(node, endpoint, element, data, len);
    }
    /* The only setting of the system types themselves. The rest are counters and
     * description — those are read. `Device` describes the device and has none. */
    if (endpoint != LB_EP_DIAGNOSTICS || element != LB_DIAG_ERROR_THRESHOLD) {
        return LB_RESULT_READ_ONLY;
    }
    lb_value_t v;
    /* The 16..255 bounds come from the type definition, not from the width of a u16 —
     * which is why they are checked here rather than through `lb_in_range`. A threshold
     * below 16 would mean the node announces bus trouble at the first crackle on the
     * line. */
    if (!lb_decode(LB_T_U16, data, len, &v) || v.absent
        || v.value < 16 || v.value > 255) {
        return LB_RESULT_INVALID_VALUE;
    }
    node->error_threshold = (uint16_t)v.value;
    return LB_RESULT_OK;
}

static uint8_t system_command(lb_node_t *node, uint8_t endpoint, uint8_t element)
{
    if (endpoint == LB_EP_DIAGNOSTICS && element == LB_DIAG_CMD_RESET) {
        node->received = 0;
        node->sent = 0;
        node->dropped = 0;
        return LB_RESULT_OK;
    }
    /* With a capacity of 0 the `Groups` commands have nowhere to store an assignment.
     * `INSUFFICIENT_SPACE` says exactly that — unlike "no such command", which would
     * have the hub believe this is an old firmware version. */
    if (endpoint == LB_EP_GROUPS) {
        return element <= 0x03u ? LB_RESULT_INSUFFICIENT_SPACE
                                : LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (endpoint != LB_EP_DIAGNOSTICS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    return LB_RESULT_UNSUPPORTED_ELEMENT;
}

/** `Device` commands — all three are mandatory (types/0x0001-device.md §3).
 *
 * `Identify` only sets a deadline: what the person sees is the device's business, told
 * through the state watch (`LB_STATE_IDENTIFY`). A device without an LED still accepts it
 * (COMPLIANCE.md LB-NODE-055). Until 2026-10-03 this answered `UNSUPPORTED_ELEMENT` while
 * the hub's view sent it on `i` (zadania/zrobione/CR-05.md); pinned by vectors/scenarios/identify.json.
 *
 * `Reboot` takes the factory reset's path without the wipe. Its `delay` is honoured, but
 * never below `LB_RESET_GRACE_MS`: "0 = at once" is allowed by the type, yet a restart
 * before the acknowledgement leaves the controller loses it, and the type itself says the
 * delay exists so the sender learns the command arrived.
 *
 * `FactoryReset` is what the operator reaches for when the hub finds a node at an address
 * it does not know (06-cykl-zycia-urzadzenia.md §2.3): the node has to forget the
 * address and come back as a new device. It is only ACKNOWLEDGED here — the reply still
 * has to leave from the old address, as with a write to `Address`
 * (06-cykl-zycia-urzadzenia.md §4.2) — and carried out in `lb_node_tick`.
 */
static uint8_t device_command(lb_node_t *node, uint8_t element,
                              const uint8_t *args, uint8_t len)
{
    uint32_t now = node->platform->millis(node->platform->ctx);
    lb_value_t v;

    if (element == LB_DEV_CMD_IDENTIFY) {
        if (len < 2 || !lb_decode(LB_T_U16, args, 2, &v) || v.absent) {
            return LB_RESULT_INVALID_VALUE;
        }
        node->identifying = v.value != 0;
        node->identify_until_ms = now + (uint32_t)v.value * 1000u;
        return LB_RESULT_OK;
    }
    if (element == LB_DEV_CMD_REBOOT) {
        if (len < 2 || !lb_decode(LB_T_U16, args, 2, &v) || v.absent) {
            return LB_RESULT_INVALID_VALUE;
        }
        if (node->reset_pending && node->reset_wipe) {
            return LB_RESULT_OK; /* a factory reset already on its way restarts anyway */
        }
        uint32_t delay = (uint32_t)v.value < LB_RESET_GRACE_MS ? LB_RESET_GRACE_MS
                                                              : (uint32_t)v.value;
        node->reset_pending = true;
        node->reset_wipe = false;
        node->reset_at_ms = now + delay;
        return LB_RESULT_OK;
    }
    if (element != LB_DEV_CMD_FACTORY_RESET) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (len < 4 || !lb_decode(LB_T_U32, args, 4, &v)
        || (uint32_t)v.value != LB_FACTORY_RESET_MAGIC) {
        return LB_RESULT_INVALID_VALUE;
    }
    lb_node_factory_reset(node);
    return LB_RESULT_OK;
}

void lb_node_factory_reset(lb_node_t *node)
{
    /* A second press, or the button during a hub's reset, changes nothing: the first
     * deadline stands. A pending Reboot is upgraded — the wipe is what was asked for. */
    if (!node->reset_pending) {
        node->reset_at_ms = node->platform->millis(node->platform->ctx) + LB_RESET_GRACE_MS;
    }
    node->reset_pending = true;
    node->reset_wipe = true;
    /* Reported at once: from the hub this runs inside a tick that reports anyway, but the
     * button calls it from the device's loop, and the LED should not wait for the tick. */
    if (node->booted) {
        report_state(node);
    }
}

/** Carry out an acknowledged factory reset or `Reboot`.
 *
 * For a factory reset the address goes first and into the persistent store: whatever
 * happens next — a restart, a power cut half-way — the node comes back without it and
 * announces itself. Only then the restart, which returns everything held in RAM to its
 * defaults. A `Reboot` keeps the address and only restarts.
 *
 * Returns true when the platform restarted — on a board it never returns at all; on a
 * host (the simulator) it does, and that boot is over: the caller sends nothing more.
 */
static bool carry_out_reset(lb_node_t *node)
{
    bool wipe = node->reset_wipe;
    node->reset_pending = false;
    node->reset_wipe = false;
    if (wipe) {
        node->address = LB_ADDRESS_UNASSIGNED;
        if (node->platform->store != NULL) {
            node->platform->store(node->platform->ctx, NVS_KEY_ADDRESS, &node->address,
                                  sizeof node->address);
        }
    }
    if (node->platform->restart != NULL) {
        node->platform->restart(node->platform->ctx);
        return true; /* on the board: never reached */
    }
    /* Without a restart the node does what it can of one. What lives in RAM here goes
     * back (an Identify in progress with it); the endpoints' defaults live in the
     * application's arrays, not here. */
    node->identifying = false;
    if (wipe) {
        system_defaults(node);
        node->join_attempts = 0;
        schedule_announce(node);
        return false;
    }
    /* A Reboot as the bus sees one: a new boot, counted, announced with SOFTWARE. */
    node->reset_reason = LB_RESET_SOFTWARE;
    node->started_ms = node->platform->millis(node->platform->ctx);
    node->boot_count++;
    if (node->platform->store != NULL) {
        node->platform->store(node->platform->ctx, "boot", &node->boot_count,
                              sizeof node->boot_count);
    }
    if (node->address != LB_ADDRESS_UNASSIGNED) {
        announce_booted(node);
    } else {
        node->join_attempts = 0;
        schedule_announce(node);
    }
    return false;
}

/** Ask the controller how the bus is, and count every entry into bus-off.
 *
 * At every tick, not on a timer: the ESP32 is out of bus-off in about 11 ms (128 x 11
 * bits at 125 kbit/s), so anything slower would miss bus-offs and `BusOffCount` would
 * undercount — and on the ESP32 the question is a copy of a struct the port reads every
 * loop anyway. A port without `health` is taken as a healthy bus: there is nothing to
 * show, and showing a fault without evidence would be worse. */
static void read_health(lb_node_t *node)
{
    uint16_t tec = 0, rec = 0;
    uint8_t state = LB_BUS_ACTIVE;
    if (node->bus->health == NULL || !node->bus->health(node->bus->ctx, &tec, &rec, &state)) {
        state = LB_BUS_ACTIVE;
    }
    if (state == LB_BUS_OFF && node->bus_state != LB_BUS_OFF) {
        node->bus_off_count++;
    }
    node->bus_state = state;
}

/** The one state to show, by the priority of `lb_node_state_t` (node.h says why one). */
static lb_node_state_t current_state(const lb_node_t *node)
{
    if (node->reset_pending) {
        return LB_STATE_RESETTING;
    }
    if (node->identifying) {
        return LB_STATE_IDENTIFY;
    }
    /* Warning is not a fault: the controller still works, it only counts errors. */
    if (node->bus_state == LB_BUS_PASSIVE || node->bus_state == LB_BUS_OFF) {
        return LB_STATE_BUS_FAULT;
    }
    if (node->address == LB_ADDRESS_UNASSIGNED) {
        return LB_STATE_NO_ADDRESS;
    }
    return LB_STATE_READY;
}

/** Tell the state watch — on change only, and the first time always. */
static void report_state(lb_node_t *node)
{
    lb_node_state_t state = current_state(node);
    if (node->state_known && state == node->state) {
        return;
    }
    node->state = state;
    node->state_known = true;
    if (node->on_state != NULL) {
        node->on_state(node->state_ctx, state);
    }
}

static void respond(lb_node_t *node, const lb_fields_t *f, uint8_t tid,
                      uint8_t result, const uint8_t *data, uint8_t len)
{
    uint8_t payload[LB_MAX_PAYLOAD];
    payload[0] = lb_encode_header(tid, result);
    if (len > 0) {
        memcpy(payload + 1, data, len);
    }
    uint32_t can_id = 0;
    if (lb_encode_id(LB_KIND_RESPONSE, node->address, f->endpoint, f->element, &can_id)) {
        send_frame(node, can_id, payload, (uint8_t)(len + 1u));
    }
}

static void handle_frame(lb_node_t *node, const lb_frame_t *frame)
{
    if (lb_is_join(frame->can_id)) {
        /* K=1 is somebody else's announcement — not our business. K=0 is an
         * assignment, but the hub broadcasts those to everyone, so the ShortId in the
         * payload decides who it is for. */
        if (((frame->can_id >> LB_JOIN_DIRECTION_SHIFT) & 1u) == 0u) {
            accept_assignment(node, frame);
        }
        return;
    }
    lb_fields_t f = lb_decode_id(frame->can_id);

    /* Somebody else's address is not our business. Broadcast exists, but we do not
     * handle it yet — there are no group commands in the node. */
    if (f.address != node->address || node->address == LB_ADDRESS_UNASSIGNED) {
        return;
    }
    if (frame->len == 0) {
        return; /* a transactional message with no header byte is broken */
    }

    uint8_t tid = 0;
    lb_decode_header(frame->data[0], &tid, NULL);

    uint8_t data[LB_MAX_PAYLOAD - 1];
    uint8_t len = 0;
    uint8_t result;

    if (f.kind == LB_KIND_READ) {
        if (f.endpoint == LB_EP_DEVICE && f.element < LB_COMMON_BASE) {
            result = device_read(node, f.element, data, sizeof data, &len);
        } else if (f.endpoint < LB_EP_FIRST_USER) {
            result = system_read(node, f.endpoint, f.element, data, sizeof data, &len);
        } else {
            lb_endpoint_t *ep = lb_node_endpoint(node, f.endpoint);
            result = ep == NULL
                  ? LB_RESULT_UNSUPPORTED_ENDPOINT
                  : lb_endpoint_read(ep, f.element, data, sizeof data, &len);
        }
    } else if (f.kind == LB_KIND_WRITE) {
        lb_endpoint_t *ep = lb_node_endpoint(node, f.endpoint);
        if (f.endpoint < LB_EP_FIRST_USER) {
            result = system_write(node, f.endpoint, f.element, frame->data + 1,
                                 (uint8_t)(frame->len - 1u));
        } else if (ep == NULL) {
            result = LB_RESULT_UNSUPPORTED_ENDPOINT;
        } else {
            result = lb_endpoint_write(ep, f.element, frame->data + 1,
                                      (uint8_t)(frame->len - 1u));
        }
    } else if (f.kind == LB_KIND_COMMAND) {
        lb_endpoint_t *ep = lb_node_endpoint(node, f.endpoint);
        if (f.endpoint < LB_EP_FIRST_USER) {
            result = f.endpoint == LB_EP_DEVICE
                  ? device_command(node, f.element, frame->data + 1,
                                   (uint8_t)(frame->len - 1u))
                  : system_command(node, f.endpoint, f.element);
        } else if (ep == NULL) {
            result = LB_RESULT_UNSUPPORTED_ENDPOINT;
        } else if (ep->on_command == NULL) {
            result = LB_RESULT_UNSUPPORTED_ELEMENT;
        } else {
            result = ep->on_command(ep, f.element, frame->data + 1,
                                   (uint8_t)(frame->len - 1u));
        }
    } else {
        return; /* not for us: reports, events, other nodes' replies */
    }

    respond(node, &f, tid, result, data, len);

    /* SEGMENTED is an ANNOUNCEMENT, not a result: the hub keeps waiting on the same
     * transaction, and the value arrives in separate frames right after it. */
    if (result == LB_RESULT_SEGMENTED && f.endpoint == LB_EP_DEVICE
        && f.element == LB_DEV_UID) {
        uint8_t blob[2 + LB_UID_LEN];
        uint8_t length = (uint8_t)lb_encode_blob(node->identity.uid, LB_UID_LEN,
                                                  blob, sizeof blob);
        lb_node_send_segments(node, f.endpoint, f.element, tid, blob, length);
    }
}

void lb_node_tick(lb_node_t *node)
{
    if (!node->booted) {
        return; /* before Booted a node ANSWERS NOTHING (types/0x0001-device.md §5.1) */
    }

    uint32_t now = node->platform->millis(node->platform->ctx);
    if (node->reset_pending && (int32_t)(now - node->reset_at_ms) >= 0
        && carry_out_reset(node)) {
        return; /* restarted: an announcement now would come from a boot that is over —
                 * and the LED keeps RESETTING, the last thing it was told */
    }
    if (node->identifying && (int32_t)(now - node->identify_until_ms) >= 0) {
        node->identifying = false; /* Identify stops by itself (types/0x0001-device.md §3.1) */
    }
    read_health(node);

    /* Having no address is the only condition: a node that receives one stops meeting
     * it and thereby stops announcing. There is no separate "should I announce" flag,
     * because it would be a second description of the same state. */
    /* Modulo 2^32, like the reset deadline above: `millis()` wraps after 49 days, and a
     * plain `>=` would then stop a node without an address from ever announcing. */
    if (node->address == LB_ADDRESS_UNASSIGNED
        && (int32_t)(node->platform->millis(node->platform->ctx) - node->next_join_ms) >= 0) {
        announce(node);
    }
    lb_frame_t frame;
    /* We take whatever is waiting and return. Receiving in an unbounded loop would
     * starve the deadlines on a flooded bus — hence the limit. */
    for (int i = 0; i < 16; i++) {
        if (!node->bus->recv(node->bus->ctx, &frame)) {
            break;
        }
        node->received++;
        if (node->on_frame != NULL) {
            node->on_frame(node->watch_ctx, &frame, false);
        }
        handle_frame(node, &frame);
    }
    report_state(node);
}

const char *lb_node_state_name(lb_node_state_t state)
{
    switch (state) {
    case LB_STATE_RESETTING:  return "RESETTING";
    case LB_STATE_IDENTIFY:   return "IDENTIFY";
    case LB_STATE_BUS_FAULT:  return "BUS_FAULT";
    case LB_STATE_NO_ADDRESS: return "NO_ADDRESS";
    case LB_STATE_READY:      return "READY";
    }
    return "?";
}
