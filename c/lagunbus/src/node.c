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
static void begin_reporting(lb_node_t *node);

void lb_node_init(lb_node_t *node, lb_transport_t *bus, lb_platform_t *platform,
                  const lb_identity_t *identity, lb_endpoint_t *endpoints, uint8_t count)
{
    memset(node, 0, sizeof *node);
    node->bus = bus;
    node->platform = platform;
    node->identity = *identity;
    node->endpoints = endpoints;
    node->endpoint_count = count;
    /* A new node knows nothing it has reported. The arrays are the device's and outlive
     * a node set up again on them — the tests do exactly that, a host could too. */
    for (uint8_t i = 0; i < count; i++) {
        memset(endpoints[i].reports, 0, sizeof endpoints[i].reports);
        endpoints[i].rounded = false;
    }
    node->reset_reason = LB_RESET_POWER_ON;

    static const uint16_t type_ids[] = {LB_TYPE_DEVICE, LB_TYPE_DIAGNOSTICS, LB_TYPE_GROUPS};
    for (uint8_t i = 0; i <= LB_EP_GROUPS; i++) {
        node->system[i] = (lb_endpoint_t){.number = i, .type_id = type_ids[i], .revision = 1};
    }
    /* Reported, by types/0x0002-diagnostics.md §2.1; the counters with hysteresis, the
     * state without — it is an enum. Absent until the port first tells: a port that
     * cannot (SocketCAN over a plain socket) never reports them, just as it answers
     * their reads with UNAVAILABLE. */
    node->diagnostics[0] = (lb_attr_t){LB_DIAG_TX_ERROR_COUNT, LB_T_U16,
                                       LB_RO | LB_REPORTED | LB_HYST, LB_ABSENT};
    node->diagnostics[1] = (lb_attr_t){LB_DIAG_RX_ERROR_COUNT, LB_T_U16,
                                       LB_RO | LB_REPORTED | LB_HYST, LB_ABSENT};
    node->diagnostics[2] = (lb_attr_t){LB_DIAG_BUS_STATE, LB_T_ENUM8,
                                       LB_RO | LB_REPORTED, LB_ABSENT};
    node->system[LB_EP_DIAGNOSTICS].attrs = node->diagnostics;
    node->system[LB_EP_DIAGNOSTICS].attr_count = 3;
    system_defaults(node);
}

/** The settings of the system types themselves — at start, and again at a factory
 * reset. */
static void system_defaults(lb_node_t *node)
{
    for (uint8_t i = 0; i <= LB_EP_GROUPS; i++) {
        memset(&node->system[i].common, 0, sizeof node->system[i].common);
    }
    node->error_threshold = 96; /* default from the Diagnostics type definition */
    /* Defaults from types/0x0002-diagnostics.md §5.4: counters jitter at every frame,
     * so a report every change would load the bus the diagnostics is meant to watch. */
    node->system[LB_EP_DIAGNOSTICS].common.report_interval = 300;
    node->system[LB_EP_DIAGNOSTICS].common.report_hysteresis = 16;
}

/* --- the persistent store ---------------------------------------------------------- */

/** The key of a persistent attribute: "a", the endpoint number, ".", the element in two
 * hex digits — "a4.13" for `PowerOnBehaviour` of ep4, "a1.f8" for `ReportInterval` of
 * `Diagnostics`. At most "a255.ff", seven characters, well inside the 15 NVS allows.
 *
 * One key per attribute and not one blob per endpoint (zadania/zrobione/TASK-01.md N5): a blob
 * would have to be laid out, and firmware that adds or drops an attribute would read the
 * old layout as garbage. A key per attribute that new firmware does not know is simply
 * never loaded. The cost is NVS entries — some eighty on an eight-output board, a small
 * part of the partition. The value is the attribute's bytes on the wire, so a stored
 * value is checked exactly as a write from the bus is when it is loaded. */
#define ATTR_KEY_LEN sizeof "a255.ff"

static void attr_key(uint8_t endpoint, uint8_t element, char out[ATTR_KEY_LEN])
{
    static const char hex[] = "0123456789abcdef";
    size_t i = 0;
    out[i++] = 'a';
    if (endpoint >= 100u) {
        out[i++] = (char)('0' + endpoint / 100u);
    }
    if (endpoint >= 10u) {
        out[i++] = (char)('0' + endpoint / 10u % 10u);
    }
    out[i++] = (char)('0' + endpoint % 10u);
    out[i++] = '.';
    out[i++] = hex[element >> 4];
    out[i++] = hex[element & 0x0Fu];
    out[i] = '\0';
}

/** The common settings — all `RW,P` (01-model-danych.md §4) — with their wire types. */
static const struct {
    uint8_t   element;
    lb_type_t type;
} common_settings[] = {
    {LB_COMMON_ENABLED,          LB_T_BOOL},
    {LB_COMMON_QUERYENABLED,     LB_T_MAP16},
    {LB_COMMON_REPORTINTERVAL,   LB_T_U16},
    {LB_COMMON_REPORTMINGAP,     LB_T_U16},
    {LB_COMMON_REPORTHYSTERESIS, LB_T_U16},
};
#define COMMON_SETTINGS (sizeof common_settings / sizeof common_settings[0])

static uint8_t total_endpoints(const lb_node_t *node)
{
    return (uint8_t)(LB_EP_GROUPS + 1u + node->endpoint_count);
}

/** System ones first, then the functional — every endpoint the node answers for. */
static lb_endpoint_t *endpoint_at(lb_node_t *node, uint8_t i)
{
    return i <= LB_EP_GROUPS ? &node->system[i]
                             : &node->endpoints[i - (LB_EP_GROUPS + 1u)];
}

bool lb_node_persist(lb_node_t *node, lb_endpoint_t *endpoint, uint8_t element)
{
    lb_platform_t *pl = node->platform;
    if (pl->store == NULL) {
        return false;
    }
    uint8_t bytes[LB_MAX_PAYLOAD];
    uint8_t len = 0;
    if (lb_endpoint_common_setting(element)) {
        if (lb_endpoint_read(endpoint, element, bytes, sizeof bytes, &len) != LB_RESULT_OK) {
            return false;
        }
    } else if (endpoint == &node->system[LB_EP_DIAGNOSTICS]
               && element == LB_DIAG_ERROR_THRESHOLD) {
        len = lb_encode(LB_T_U16, LB_NUM(node->error_threshold), bytes, sizeof bytes);
    } else {
        const lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
        if (attr == NULL || !(attr->access & LB_PERSIST)) {
            return false;
        }
        len = lb_encode(attr->type, attr->value, bytes, sizeof bytes);
    }
    char key[ATTR_KEY_LEN];
    attr_key(endpoint->number, element, key);
    return len > 0 && pl->store(pl->ctx, key, bytes, len);
}

/** Load one stored value and put it back through the type's checks. Nothing stored, or
 * a value that no longer passes, leaves the default. */
static void restore_one(lb_node_t *node, lb_endpoint_t *endpoint, uint8_t element,
                        lb_type_t type)
{
    uint8_t bytes[LB_MAX_PAYLOAD];
    uint8_t len = lb_size_of(type);
    char key[ATTR_KEY_LEN];
    attr_key(endpoint->number, element, key);
    if (len == 0 || len > sizeof bytes
        || !node->platform->load(node->platform->ctx, key, bytes, len)) {
        return;
    }
    if (endpoint == &node->system[LB_EP_DIAGNOSTICS] && element == LB_DIAG_ERROR_THRESHOLD) {
        lb_value_t v;
        if (lb_decode(LB_T_U16, bytes, len, &v) && !v.absent && v.value >= 16
            && v.value <= 255) {
            node->error_threshold = (uint16_t)v.value;
        }
        return;
    }
    lb_endpoint_restore(endpoint, element, bytes, len);
}

/** Every stored setting back, before `Booted` — the start-up order of
 * types/0x0001-device.md §5.1 puts configuration first, and the types read their
 * settings in their init, after `lb_node_start`. Until TASK-01 nothing came back: a
 * `PowerOnBehaviour = RESTORE` the hub had written was gone at exactly the power cut it
 * was meant for. */
static void restore_settings(lb_node_t *node)
{
    if (node->platform->load == NULL) {
        return;
    }
    for (uint8_t i = 0; i < total_endpoints(node); i++) {
        lb_endpoint_t *ep = endpoint_at(node, i);
        for (size_t c = 0; c < COMMON_SETTINGS; c++) {
            restore_one(node, ep, common_settings[c].element, common_settings[c].type);
        }
        for (uint8_t a = 0; a < ep->attr_count; a++) {
            if (ep->attrs[a].access & LB_PERSIST) {
                restore_one(node, ep, ep->attrs[a].id, ep->attrs[a].type);
            }
        }
    }
    restore_one(node, &node->system[LB_EP_DIAGNOSTICS], LB_DIAG_ERROR_THRESHOLD, LB_T_U16);
}

/** Forget every stored SETTING — what a factory reset means (types/0x0001-device.md
 * §3.3). Settings are what can be written: a persistent read-only value is a counter
 * (`PressCount`, wear over the device's life) and stays, as `BootCount` does. So does
 * an output's remembered state — it is state, not configuration. */
static void erase_settings(lb_node_t *node)
{
    lb_platform_t *pl = node->platform;
    if (pl->erase == NULL) {
        return;
    }
    char key[ATTR_KEY_LEN];
    for (uint8_t i = 0; i < total_endpoints(node); i++) {
        lb_endpoint_t *ep = endpoint_at(node, i);
        for (size_t c = 0; c < COMMON_SETTINGS; c++) {
            attr_key(ep->number, common_settings[c].element, key);
            pl->erase(pl->ctx, key);
        }
        for (uint8_t a = 0; a < ep->attr_count; a++) {
            if ((ep->attrs[a].access & LB_PERSIST) && (ep->attrs[a].access & LB_WO)) {
                attr_key(ep->number, ep->attrs[a].id, key);
                pl->erase(pl->ctx, key);
            }
        }
    }
    attr_key(LB_EP_DIAGNOSTICS, LB_DIAG_ERROR_THRESHOLD, key);
    pl->erase(pl->ctx, key);
}

/* --- reporting (01-model-danych.md §6) ----------------------------------------------
 *
 * The engine is the node's, not the types' (zadania/zrobione/TASK-01.md N2). A type only says
 * what the value is now (`lb_endpoint_set`) and when it is done changing things
 * (`lb_node_flush`); whether that is a change worth a report, and when it may go out,
 * is decided here, once for every type. Until TASK-01 `output.c` held back its own
 * reports and nothing else reported at all.
 *
 * The node never scans the arrays for changes: a scan would see every step of a fade
 * and report it, while the type has to say "not during the transition"
 * (types/0x0020-output.md §5.3). A type that does not set, does not report. */

static void clear_due(lb_endpoint_t *endpoint)
{
    for (unsigned i = 0; i < LB_REPORTED_MAX; i++) {
        endpoint->reports[i].due = false;
    }
}

/** At `Booted` — at start, after an assignment, after a `Reboot` without a restart:
 * the cycles start now, the gap starts afresh, and nothing owed from before goes out.
 * The hub reads the node after `Booted` anyway, and an output reports nothing before it
 * (types/0x0020-output.md §5.7). */
static void begin_reporting(lb_node_t *node)
{
    uint32_t now = node->platform->millis(node->platform->ctx);
    for (uint8_t i = 0; i < total_endpoints(node); i++) {
        lb_endpoint_t *ep = endpoint_at(node, i);
        ep->rounded = false;
        ep->cycle_ms = now;
        clear_due(ep);
    }
}

void lb_node_flush(lb_node_t *node, lb_endpoint_t *endpoint)
{
    if (!node->booted || node->address == LB_ADDRESS_UNASSIGNED) {
        return;
    }
    if (endpoint->common.disabled) {
        clear_due(endpoint); /* "does not report" (01-model-danych.md §4) */
        return;
    }
    bool owed = false;
    for (unsigned i = 0; i < LB_REPORTED_MAX; i++) {
        owed = owed || endpoint->reports[i].due;
    }
    if (!owed) {
        return;
    }
    /* The gap is between ROUNDS, not frames (zadania/zrobione/TASK-01.md N3): `Transitioning = 0`
     * goes out with the level it reached, and a cycle with every reported attribute at
     * once — reading the gap per frame would pull them `ReportMinGap` apart. Held back,
     * the round waits for the tick and then carries the CURRENT values. */
    uint32_t now = node->platform->millis(node->platform->ctx);
    if (endpoint->rounded && now - endpoint->round_ms < endpoint->common.report_min_gap) {
        return;
    }
    /* In the order of the attribute array — the device's order, the same every time. */
    for (uint8_t a = 0; a < endpoint->attr_count; a++) {
        lb_attr_t *attr = &endpoint->attrs[a];
        lb_report_t *slot = lb_endpoint_report(endpoint, attr->id, false);
        if (slot == NULL || !slot->due) {
            continue;
        }
        uint8_t bytes[LB_MAX_PAYLOAD];
        uint8_t len = lb_encode(attr->type, attr->value, bytes, sizeof bytes);
        if (len > 0) {
            lb_node_report(node, endpoint->number, attr->id, bytes, len);
        }
        slot->reported = attr->value;
        slot->baseline = true;
        slot->due = false;
    }
    endpoint->round_ms = now;
    endpoint->rounded = true;
}

/** The cycles, then whatever is owed. Both triggers at once, neither replacing the
 * other (01-model-danych.md §6.1).
 *
 * A cycle counts from the last one, not from the last report: `ReportInterval` is the
 * floor of freshness, and a change report in between does not reset the hub's view of
 * the rest. It steps by whole periods so it does not drift with the loop; a node that
 * fell a whole period behind starts again from now rather than firing a burst. A
 * disabled endpoint's cycle keeps counting and sends nothing — switched back on, it
 * resumes on its old beat instead of reporting at once. An attribute with no value since
 * start is not reported by the cycle either: "absent" would tell the hub nothing it
 * would not learn from the first real value (zadania/zrobione/TASK-01.md N8). */
static void reporting_tick(lb_node_t *node, uint32_t now)
{
    for (uint8_t i = 0; i < total_endpoints(node); i++) {
        lb_endpoint_t *ep = endpoint_at(node, i);
        uint32_t period = (uint32_t)ep->common.report_interval * 1000u;
        if (period > 0 && now - ep->cycle_ms >= period) {
            ep->cycle_ms += period;
            if (now - ep->cycle_ms >= period) {
                ep->cycle_ms = now;
            }
            for (uint8_t a = 0; a < ep->attr_count && !ep->common.disabled; a++) {
                lb_attr_t *attr = &ep->attrs[a];
                if (!(attr->access & LB_REPORTED)) {
                    continue;
                }
                lb_report_t *slot = lb_endpoint_report(ep, attr->id, true);
                if (slot != NULL && (slot->baseline || !attr->value.absent)) {
                    slot->due = true;
                }
            }
        }
        lb_node_flush(node, ep);
    }
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
    lb_endpoint_t *ep = endpoint == 0 ? NULL : lb_node_endpoint(node, endpoint);
    uint8_t *seq = endpoint == 0 ? &node->device_event_seq
                 : ep != NULL    ? &ep->event_seq
                                 : NULL;
    if (seq == NULL) {
        return false;
    }
    if (ep != NULL && ep->common.disabled) {
        /* "Does not react" (01-model-danych.md §4). Before the sequence number: nothing
         * went out, so there is no gap for the hub to read as a lost event. */
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
    begin_reporting(node);
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
    restore_settings(node);

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

    /* 5. What the controller says at start is where Diagnostics stands, not a change:
     *    no report for it, and hysteresis counts from here (zadania/zrobione/TASK-01.md N8). */
    for (uint8_t a = 0; a < 3; a++) {
        const lb_attr_t *attr = &node->diagnostics[a];
        lb_report_t *slot = lb_endpoint_report(&node->system[LB_EP_DIAGNOSTICS], attr->id, true);
        if (slot != NULL) {
            slot->reported = attr->value;
            slot->baseline = !attr->value.absent;
            slot->due = false;
        }
    }
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
    case LB_DEV_ADDRESS:
        *len = lb_encode(LB_T_U16, LB_NUM(node->address), out, cap);
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

static uint8_t system_read(lb_node_t *node, uint8_t endpoint, uint8_t element,
                           uint8_t *out, uint8_t cap, uint8_t *len)
{
    *len = 0;
    if (endpoint > LB_EP_GROUPS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    if (element >= LB_COMMON_BASE) {
        return lb_endpoint_read(&node->system[endpoint], element, out, cap, len);
    }
    if (endpoint == LB_EP_DIAGNOSTICS) {
        return diagnostics_read(node, element, out, cap, len);
    }
    return groups_read(node, element, out, cap, len);
}

/** A write of `Device.Address` — re-addressing (06-cykl-zycia-urzadzenia.md §4.2).
 *
 * The new address goes to the store FIRST and only then is the write acknowledged
 * (COMPLIANCE.md LB-NODE-027, -028): if the reply is lost, the hub finds the node under
 * the new address, never under neither. The switch itself waits for the reply, which
 * still leaves from the old address — `handle_frame` takes `readdress_to` after it.
 *
 * Only node addresses, 0x001–0x7FD (03-adresacja.md §2). 0x000 is refused although it is
 * a valid `u16`: forgetting the address is what `FactoryReset` does, with the restart that
 * goes with it, and a node left without an address by a write would be one the hub just
 * lost. 0x7FE is the hub, 0x7FF broadcast. The address the node already has is `OK` and
 * touches nothing.
 *
 * Missing from the C library until 2026-10-13 — a read was `UNSUPPORTED_ELEMENT`, a write
 * `READ_ONLY`, and the hub could not move a node (vectors/scenarios/address.json). */
static uint8_t readdress(lb_node_t *node, const uint8_t *data, uint8_t len)
{
    lb_value_t v;
    if (!lb_decode(LB_T_U16, data, len, &v) || v.absent
        || v.value < (int32_t)LB_ADDRESS_NODE_MIN || v.value > (int32_t)LB_ADDRESS_NODE_MAX) {
        return LB_RESULT_INVALID_VALUE;
    }
    uint16_t fresh = (uint16_t)v.value;
    if (fresh == node->address) {
        return LB_RESULT_OK;
    }
    if (node->platform->store == NULL
        || !node->platform->store(node->platform->ctx, NVS_KEY_ADDRESS, &fresh,
                                  sizeof fresh)) {
        return LB_RESULT_FAILURE; /* not stored, not taken — never `OK` (LB-NODE-028) */
    }
    node->readdress_to = fresh;
    return LB_RESULT_OK;
}

static uint8_t system_write(lb_node_t *node, uint8_t endpoint, uint8_t element,
                            const uint8_t *data, uint8_t len)
{
    if (endpoint > LB_EP_GROUPS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    if (element >= LB_COMMON_BASE) {
        return lb_endpoint_write(&node->system[endpoint], element, data, len);
    }
    if (endpoint == LB_EP_DEVICE && element == LB_DEV_ADDRESS) {
        return readdress(node, data, len);
    }
    /* The only setting of the system types themselves besides `Address`. The rest are
     * counters and description — those are read. */
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
        erase_settings(node);
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
    } else {
        /* The reported attributes of Diagnostics — the engine decides whether the change
         * is worth a report (hysteresis 16 by default, types/0x0002-diagnostics.md §5.4). */
        lb_endpoint_t *diag = &node->system[LB_EP_DIAGNOSTICS];
        lb_endpoint_set(diag, LB_DIAG_TX_ERROR_COUNT, LB_NUM(tec));
        lb_endpoint_set(diag, LB_DIAG_RX_ERROR_COUNT, LB_NUM(rec));
        lb_endpoint_set(diag, LB_DIAG_BUS_STATE,
                        LB_NUM(state == LB_BUS_OFF       ? LB_DIAG_BUS_OFF
                               : state == LB_BUS_PASSIVE ? LB_DIAG_BUS_PASSIVE
                                                         : LB_DIAG_BUS_ACTIVE));
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

/** Was this command carried out within the window? (04-interakcje.md §2.2)
 *
 * The age is `now - at_ms` modulo 2^32, like every deadline here; `dedup_expire` empties
 * old entries at every tick, so an entry 49 days old cannot come back to life when
 * `millis()` wraps. */
static bool dedup_seen(const lb_node_t *node, uint8_t tid, uint8_t endpoint,
                       uint8_t command, uint32_t now)
{
    for (unsigned i = 0; i < LB_DEDUP_ENTRIES; i++) {
        const lb_dedup_t *e = &node->dedup[i];
        if (e->used && e->tid == tid && e->endpoint == endpoint && e->command == command
            && now - e->at_ms < LB_DEDUP_WINDOW_MS) {
            return true;
        }
    }
    return false;
}

/** Remember a command carried out with `OK`.
 *
 * Into a free or expired entry first; only when every entry is still inside the window
 * does the oldest go — the one whose retries are most likely over. The alternative,
 * `BUSY` until an entry expires, was rejected: the hub retries after `BUSY` at once, so
 * three attempts would be gone in a few milliseconds and the command lost
 * (README § Decisions, zadania/zrobione/BUG-02.md). With `LB_DEDUP_ENTRIES` sized as the
 * specification asks, eviction of a live entry does not happen. */
static void dedup_remember(lb_node_t *node, uint8_t tid, uint8_t endpoint,
                           uint8_t command, uint32_t now)
{
    lb_dedup_t *slot = &node->dedup[0];
    for (unsigned i = 0; i < LB_DEDUP_ENTRIES; i++) {
        lb_dedup_t *e = &node->dedup[i];
        if (!e->used || now - e->at_ms >= LB_DEDUP_WINDOW_MS) {
            slot = e;
            break;
        }
        if (now - e->at_ms > now - slot->at_ms) {
            slot = e;
        }
    }
    slot->used = true;
    slot->at_ms = now;
    slot->tid = tid;
    slot->endpoint = endpoint;
    slot->command = command;
}

static void dedup_expire(lb_node_t *node, uint32_t now)
{
    for (unsigned i = 0; i < LB_DEDUP_ENTRIES; i++) {
        if (node->dedup[i].used && now - node->dedup[i].at_ms >= LB_DEDUP_WINDOW_MS) {
            node->dedup[i].used = false;
        }
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

/* --- LONG_WRITE (05-mapowanie-can.md §6) -------------------------------------------
 *
 * The first user is the text of a display field (types/0x0022-display.md; zadania/zrobione/FEAT-12.md).
 * Segments carry no header byte: [0] is LAST and the Tid, [1] the segment number. The node
 * answers ONCE, after the LAST segment, with an ordinary RESPONSE and the Tid of segment 0
 * — to the hub it is one write that took several frames (04-interakcje.md §4). */

/** A long write to `Device`, `Diagnostics` or `Groups`. None of them has a writable
 * string: what exists is read-only (`Uid`) or a scalar that never travels in segments
 * (`Address`, `ErrorThreshold`, the common settings — 02-typy-danych.md §2.2). */
static uint8_t system_long_write(lb_node_t *node, uint8_t endpoint, uint8_t element,
                                 const uint8_t *value, uint8_t len)
{
    if (endpoint > LB_EP_GROUPS) {
        return LB_RESULT_UNSUPPORTED_ENDPOINT; /* ep3 Scenes — not in this device */
    }
    if (element >= LB_COMMON_BASE) {
        return lb_endpoint_write_long(&node->system[endpoint], element, value, len);
    }
    if ((endpoint == LB_EP_DEVICE && element == LB_DEV_ADDRESS)
        || (endpoint == LB_EP_DIAGNOSTICS && element == LB_DIAG_ERROR_THRESHOLD)) {
        return LB_RESULT_INVALID_VALUE;
    }
    /* Whether the element exists at all is what a read knows; reads have no side effects. */
    uint8_t scratch[LB_MAX_PAYLOAD];
    uint8_t n = 0;
    uint8_t exists = endpoint == LB_EP_DEVICE
                   ? device_read(node, element, scratch, sizeof scratch, &n)
                   : system_read(node, endpoint, element, scratch, sizeof scratch, &n);
    return exists == LB_RESULT_UNSUPPORTED_ELEMENT ? exists : LB_RESULT_READ_ONLY;
}

static uint8_t long_write(lb_node_t *node, const lb_long_write_t *lw)
{
    if (lw->overflow) {
        return LB_RESULT_INVALID_VALUE; /* longer than any attribute this node has */
    }
    if (lw->endpoint < LB_EP_FIRST_USER) {
        return system_long_write(node, lw->endpoint, lw->element, lw->data, lw->len);
    }
    lb_endpoint_t *ep = lb_node_endpoint(node, lw->endpoint);
    return ep == NULL ? LB_RESULT_UNSUPPORTED_ENDPOINT
                      : lb_endpoint_write_long(ep, lw->element, lw->data, lw->len);
}

/** One segment of a `LONG_WRITE`.
 *
 * Segment 0 opens a value — and starts afresh when the same value is already open: a
 * retry has the same Tid, so only the number tells it from the first try (§6.5). Segment
 * 0 of a DIFFERENT value while one is open gets `BUSY` at once: one value at a time
 * (§6.7), and `BUSY` is the code worth retrying on. A gap, or 100 ms without the next
 * segment, drops the value whole and says nothing (§6.6) — the hub's transaction times
 * out and repeats the whole write; a reply here would only race it. The timeout is
 * checked when the next segment comes, not by the tick: a dropped value that nobody
 * continues changes nothing, and checking here also settles a segment 0 of another value
 * after the silence, which then opens instead of getting `BUSY`. */
static void take_segment(lb_node_t *node, const lb_fields_t *f, const lb_frame_t *frame)
{
    if (frame->len < 2u) {
        return; /* not even its two header bytes — broken */
    }
    lb_long_write_t *lw = &node->long_write;
    uint32_t now = node->platform->millis(node->platform->ctx);
    if (lw->open && now - lw->last_ms >= LB_SEGMENT_TIMEOUT_MS) {
        lw->open = false;
    }
    uint8_t tid = (uint8_t)(frame->data[0] & 0x07u);
    uint8_t number = frame->data[1];
    bool same = lw->open && lw->endpoint == f->endpoint && lw->element == f->element;

    if (number == 0u) {
        if (lw->open && !same) {
            respond(node, f, tid, LB_RESULT_BUSY, NULL, 0);
            return;
        }
        lw->open = true;
        lw->overflow = false;
        lw->endpoint = f->endpoint;
        lw->element = f->element;
        lw->tid = tid;
        lw->next = 0;
        lw->len = 0;
    } else if (!same) {
        return; /* a segment of a value we are not assembling */
    } else if (number != lw->next) {
        lw->open = false; /* a gap */
        return;
    }

    uint8_t chunk = (uint8_t)(frame->len - 2u);
    if (!lw->overflow && (size_t)lw->len + chunk > LB_LONG_WRITE_MAX) {
        lw->overflow = true;
    }
    if (!lw->overflow) {
        memcpy(lw->data + lw->len, frame->data + 2, chunk);
        lw->len = (uint8_t)(lw->len + chunk);
    }
    lw->next++;
    lw->last_ms = now;
    if (!(frame->data[0] & LB_SEGMENT_LAST)) {
        return;
    }
    lw->open = false;
    respond(node, f, lw->tid, long_write(node, lw), NULL, 0);
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

    if (f.kind == LB_KIND_LONG_WRITE) {
        take_segment(node, &f, frame);
        return;
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
        if (result == LB_RESULT_OK) {
            lb_endpoint_t *target = f.endpoint <= LB_EP_GROUPS ? &node->system[f.endpoint] : ep;
            /* Stored BEFORE the reply (COMPLIANCE.md LB-NODE-027): a hub that got `OK`
             * may cut the power the next moment. Anything not persistent is skipped by
             * `lb_node_persist` itself. */
            lb_node_persist(node, target, f.element);
            if (f.element == LB_COMMON_REPORTINTERVAL) {
                /* A new interval counts from now — not from a cycle the old one began. */
                target->cycle_ms = node->platform->millis(node->platform->ctx);
            }
        }
    } else if (f.kind == LB_KIND_COMMAND) {
        /* A retry of a command already carried out gets the remembered reply and nothing
         * else — a second `Toggle` would put the light back where it was
         * (04-interakcje.md §2.2). Every addressed command goes through here, `Device`
         * ones included: a repeated `Reboot` would otherwise move the restart. Reads and
         * writes do not: repeated, they do the same thing again. */
        uint32_t now = node->platform->millis(node->platform->ctx);
        lb_endpoint_t *ep = lb_node_endpoint(node, f.endpoint);
        bool repeated = dedup_seen(node, tid, f.endpoint, f.element, now);
        if (repeated) {
            result = LB_RESULT_OK;
        } else if (f.endpoint < LB_EP_FIRST_USER) {
            result = f.endpoint == LB_EP_DEVICE
                  ? device_command(node, f.element, frame->data + 1,
                                   (uint8_t)(frame->len - 1u))
                  : system_command(node, f.endpoint, f.element);
        } else if (ep == NULL) {
            result = LB_RESULT_UNSUPPORTED_ENDPOINT;
        } else if (ep->common.disabled) {
            /* A disabled endpoint "does not react" (01-model-danych.md §4), but a command
             * always gets an answer: silence would read as a lost frame and be retried.
             * Reads and writes still work — otherwise the hub could never switch it back
             * on (zadania/zrobione/TASK-01.md N4). */
            result = LB_RESULT_INVALID_STATE;
        } else if (ep->on_command == NULL) {
            result = LB_RESULT_UNSUPPORTED_ELEMENT;
        } else {
            result = ep->on_command(ep, f.element, frame->data + 1,
                                   (uint8_t)(frame->len - 1u));
        }
        /* Only `OK` is remembered (README § Decisions): a refusal in the buffer would
         * answer a retry after `BUSY` with `BUSY` for the whole window, and every other
         * refusal repeats itself anyway. A repeat is not stored again: that would
         * stretch its window from the first execution to the last retry. */
        if (!repeated && result == LB_RESULT_OK) {
            dedup_remember(node, tid, f.endpoint, f.element, now);
        }
    } else {
        return; /* not for us: reports, events, other nodes' replies */
    }

    respond(node, &f, tid, result, data, len);

    /* Re-addressing: the reply above left from the old address, everything from here on
     * leaves from the new one. */
    if (node->readdress_to != LB_ADDRESS_UNASSIGNED) {
        node->address = node->readdress_to;
        node->readdress_to = LB_ADDRESS_UNASSIGNED;
    }

    /* SEGMENTED is an ANNOUNCEMENT, not a result: the hub keeps waiting on the same
     * transaction, and the value arrives in separate frames right after it. */
    if (result == LB_RESULT_SEGMENTED && f.endpoint == LB_EP_DEVICE
        && f.element == LB_DEV_UID) {
        uint8_t blob[2 + LB_UID_LEN];
        uint8_t length = (uint8_t)lb_encode_blob(node->identity.uid, LB_UID_LEN,
                                                  blob, sizeof blob);
        lb_node_send_segments(node, f.endpoint, f.element, tid, blob, length);
    } else if (result == LB_RESULT_SEGMENTED && f.endpoint >= LB_EP_FIRST_USER) {
        /* A string attribute of a functional endpoint. The largest string is 255 bytes
         * of text with its prefix (`TextLength` is a `u8`); on the stack only for this
         * read, which a display field gets once in a while. */
        uint8_t text[255];
        uint8_t length = 0;
        if (lb_endpoint_read_long(lb_node_endpoint(node, f.endpoint), f.element, text,
                                  sizeof text, &length) == LB_RESULT_OK) {
            lb_node_send_segments(node, f.endpoint, f.element, tid, text, length);
        }
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
    dedup_expire(node, now);

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
    reporting_tick(node, now);
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
