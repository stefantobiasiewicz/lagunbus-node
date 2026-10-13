/* The node loop: reads, writes, result codes, start-up order.
 *
 * The bus is a mock, so everything happens at once and deterministically —
 * the same trick as in the hub's tests with a clock driven from the test. On a real
 * bus the same cases would mean waiting and guessing.
 */
#include "lagunbus/node.h"
#include "lagunbus/registry.h"
#include "lb_test.h"

#include <string.h>

#define ADDRESS 0x042u

/* --- the bus mock ---------------------------------------------------------- */

typedef struct {
    lb_frame_t to_node[8];
    uint8_t    pending;
    lb_frame_t from_node[16];
    uint8_t    sent;
    uint32_t   clock;
    uint32_t   jitter;
} bus_t;

static bool m_send(void *ctx, const lb_frame_t *frame)
{
    bus_t *m = (bus_t *)ctx;
    if (m->sent >= 16) {
        return false;
    }
    m->from_node[m->sent++] = *frame;
    return true;
}

static bool m_recv(void *ctx, lb_frame_t *out)
{
    bus_t *m = (bus_t *)ctx;
    if (m->pending == 0) {
        return false;
    }
    *out = m->to_node[0];
    memmove(m->to_node, m->to_node + 1, sizeof(lb_frame_t) * (size_t)(--m->pending));
    return true;
}

static uint32_t m_millis(void *ctx)
{
    return ((bus_t *)ctx)->clock;
}

static uint32_t m_random(void *ctx)
{
    return ((bus_t *)ctx)->jitter;
}

/* A board restart as the test sees it: counted, and the last address the node stored
 * before it. The real one does not return; this one does, so the test can look. */
static int restarts = 0;
static uint16_t stored_address = 0xFFFFu;

/* The state watch as an LED sees it: every call counted, the last state kept. */
static int state_calls = 0;
static lb_node_state_t last_state = LB_STATE_READY;

static void m_state(void *ctx, lb_node_state_t state)
{
    (void)ctx;
    state_calls++;
    last_state = state;
}

static void m_restart(void *ctx)
{
    (void)ctx;
    restarts++;
}

static bool m_store(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    if (strcmp(key, "address") == 0 && len == sizeof stored_address) {
        memcpy(&stored_address, data, len);
    }
    return true;
}

/** Push in a frame "from the hub".
 *
 * It also clears whatever the node sent earlier, so that each test looks at ITS OWN
 * reply. Without that the buffer overflowed after a dozen-odd cases, the mock silently
 * dropped new frames, and the tests were checking a reply from ten cases ago — which
 * looked like a fault in the node.
 */
static void push_frame(bus_t *m, uint8_t kind, uint16_t address, uint8_t ep,
                 uint8_t element, uint8_t tid, const uint8_t *data, uint8_t len)
{
    m->sent = 0;
    lb_frame_t frame = {0};
    lb_encode_id(kind, address, ep, element, &frame.can_id);
    frame.data[0] = lb_encode_header(tid, LB_RESULT_OK);
    if (len > 0) {
        memcpy(frame.data + 1, data, len);
    }
    frame.len = (uint8_t)(len + 1u);
    m->to_node[m->pending++] = frame;
}

/** The last reply: result code and `Tid`. */
static bool last_result(const bus_t *m, uint8_t *result, uint8_t *tid,
                     const uint8_t **data, uint8_t *len)
{
    if (m->sent == 0) {
        return false;
    }
    const lb_frame_t *r = &m->from_node[m->sent - 1];
    lb_decode_header(r->data[0], tid, result);
    *data = r->data + 1;
    *len = (uint8_t)(r->len - 1u);
    return true;
}

/* --- an endpoint to experiment on ----------------------------------------------- */

static uint8_t writes = 0;

static uint8_t on_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    (void)element;
    (void)value;
    writes++;
    return LB_RESULT_OK;
}

static uint8_t on_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                          uint8_t len)
{
    (void)self;
    (void)args;
    (void)len;
    return element == 0x02u ? LB_RESULT_OK : LB_RESULT_UNSUPPORTED_ELEMENT;
}

int main(void)
{
    bus_t m = {0};
    lb_transport_t bus = {.send = m_send, .recv = m_recv, .ctx = &m};
    lb_platform_t plat = {.millis = m_millis, .ctx = &m};

    lb_attr_t attributes[] = {
        {0x00u, LB_T_BOOL, LB_RO, LB_NUM(0)},               /* OnOff — read-only      */
        {0x10u, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(25)},  /* DebounceTime          */
        {0x20u, LB_T_U8, LB_WO, LB_NUM(0)},                 /* something write-only  */
    };
    lb_endpoint_t endpoints[] = {{
        .number = 4, .type_id = LB_TYPE_OUTPUT, .revision = 1, .features = 0,
        .channel = 1, .attrs = attributes, .attr_count = 3,
        .on_command = on_command, .on_write = on_write,
    }};

    lb_identity_t identity_cfg = {
        .uid = {0xA1, 0xB2, 0xC3, 0xD4, 0, 1, 2, 3},
        .device_type = 0x02F0, .vendor_id = 0x0000,
        .firmware = 0x00010000, .hardware = 1, .protocol = 1,
    };

    lb_node_t node;
    lb_node_init(&node, &bus, &plat, &identity_cfg, endpoints, 1);
    node.address = ADDRESS;

    uint8_t result = 0, tid = 0, len = 0;
    const uint8_t *data = NULL;

    printf("start-up order (types/0x0001-device.md §5.1)\n");

    LB_TEST("before Booted the node ANSWERS NOTHING");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_DEV_DEVICE_TYPE, 1, NULL, 0);
        lb_node_tick(&node);
        LB_ASSERT(m.sent == 0);
        LB_ASSERT(m.pending == 1); /* the frame was not even taken in */
    }

    LB_TEST("Booted carries a sequence number, a reset reason and a firmware version");
    {
        lb_node_start(&node);
        LB_ASSERT(m.sent == 1);
        const lb_frame_t *b = &m.from_node[0];
        lb_fields_t f = lb_decode_id(b->can_id);
        LB_ASSERT(f.kind == LB_KIND_EVENT && f.endpoint == 0);
        LB_ASSERT(f.element == LB_DEV_EVENT_BOOTED);
        LB_ASSERT(b->len == 6);
        LB_ASSERT(b->data[0] == 0);                    /* seq — PIERWSZY (01 §3.3) */
        LB_ASSERT(b->data[1] == LB_RESET_POWER_ON);
        lb_value_t fw;
        LB_ASSERT(lb_decode(LB_T_U32, b->data + 2, 4, &fw));
        LB_ASSERT((uint32_t)fw.value == 0x00010000u);
    }

    printf("\nreading Device (ep0)\n");
    m.sent = 0;

    LB_TEST("DeviceType comes back as it was given");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_DEV_DEVICE_TYPE, 3, NULL, 0);
        lb_node_tick(&node);
        LB_ASSERT(last_result(&m, &result, &tid, &data, &len));
        LB_ASSERT(result == LB_RESULT_OK && tid == 3 && len == 2);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(v.value == 0x02F0);
    }

    LB_TEST("EndpointCount counts the FUNCTIONAL endpoints");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_DEV_ENDPOINT_COUNT, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && data[0] == 1);
    }

    LB_TEST("Uid: SEGMENTED is an ANNOUNCEMENT, with the segments following it");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_DEV_UID, 2, NULL, 0);
        lb_node_tick(&node);

        /* The blob is 2 length bytes + 8 Uid bytes = 10, that is 6 + 4. */
        LB_ASSERT(m.sent == 3);

        uint8_t w = 0, tt = 0;
        lb_decode_header(m.from_node[0].data[0], &tt, &w);
        LB_ASSERT(w == LB_RESULT_SEGMENTED && tt == 2);
        LB_ASSERT(m.from_node[0].len == 1); /* the announcement alone, no data */

        /* The segments travel as LONG_RESPONSE to the same address, endpoint and
         * element — the hub matches them to the request by EVERYTHING BUT the frame
         * kind. */
        for (int i = 1; i <= 2; i++) {
            lb_fields_t f = lb_decode_id(m.from_node[i].can_id);
            LB_ASSERT(f.kind == LB_KIND_LONG_RESPONSE);
            LB_ASSERT(f.address == ADDRESS && f.endpoint == 0 && f.element == LB_DEV_UID);
            LB_ASSERT((m.from_node[i].data[0] & 0x07u) == 2);       /* the same Tid */
            LB_ASSERT(m.from_node[i].data[1] == (uint8_t)(i - 1));  /* segment number */
        }
        LB_ASSERT((m.from_node[1].data[0] & LB_SEGMENT_LAST) == 0);
        LB_ASSERT((m.from_node[2].data[0] & LB_SEGMENT_LAST) != 0);

        /* The assembled value: a length prefix, then eight bytes of Uid. */
        uint8_t blob[10];
        memcpy(blob, m.from_node[1].data + 2, 6);
        memcpy(blob + 6, m.from_node[2].data + 2, 4);
        LB_ASSERT(blob[0] == 8 && blob[1] == 0);
        LB_ASSERT(memcmp(blob + 2, identity_cfg.uid, 8) == 0);
    }

    LB_TEST("ShortId is the CRC-32 of the Uid");
    {
        /* A value computed INDEPENDENTLY of this code — Python's `zlib.crc32` over the
         * same Uid. An outside vector is the whole point of this test: the node double in
         * the hub computes ShortId with zlib, and a test comparing the firmware against
         * itself would let a divergence between them through without a word. */
        LB_EQ_U32(lb_node_short_id(&node), 0xF5DB8A7Eu);
    }

    printf("\nresult codes — each means something different (04-interakcje.md §8)\n");

    LB_TEST("there is no such endpoint");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 9, 0x00, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ENDPOINT);
    }

    LB_TEST("the endpoint is there, the element is not");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 4, 0x7F, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ELEMENT);
    }

    LB_TEST("reading a write-only attribute is WRITE_ONLY, not \"there is none\"");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 4, 0x20, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_WRITE_ONLY);
    }

    LB_TEST("writing a read-only attribute is READ_ONLY");
    {
        uint8_t one[1] = {1};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 4, 0x00, 0, one, 1);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_READ_ONLY);
    }

    LB_TEST("Device describes the device — nothing in it can be written");
    {
        uint8_t two[2] = {1, 0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 0, LB_DEV_DEVICE_TYPE, 0, two, 2);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_READ_ONLY);
    }

    LB_TEST("a wrong length is a wrong value — the node does not guess");
    {
        uint8_t one[1] = {5};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 4, 0x10, 0, one, 1); /* a u16 was given 1 byte */
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_INVALID_VALUE);
    }

    printf("\na write that succeeds\n");

    LB_TEST("the value lands in the attribute and wakes the endpoint reaction");
    {
        uint8_t fresh_one[2] = {0x2C, 0x01}; /* 300 */
        writes = 0;
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 4, 0x10, 6, fresh_one, 2);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && tid == 6);
        LB_ASSERT(attributes[1].value.value == 300);
        LB_ASSERT(writes == 1);
    }

    LB_TEST("a read returns what was just written");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 4, 0x10, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(v.value == 300);
    }

    LB_TEST("common attributes are read from the endpoint description, not the array");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 4, LB_COMMON_TYPEID, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(result == LB_RESULT_OK && v.value == LB_TYPE_OUTPUT);

        push_frame(&m, LB_KIND_READ, ADDRESS, 4, LB_COMMON_CHANNEL, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(data[0] == 1);
    }

    printf("\ncommands and other nodes' traffic\n");

    LB_TEST("a known command runs, an unknown one says so");
    {
        push_frame(&m, LB_KIND_COMMAND, ADDRESS, 4, 0x02, 1, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK);

        push_frame(&m, LB_KIND_COMMAND, ADDRESS, 4, 0x77, 1, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ELEMENT);
    }

    LB_TEST("a frame to another address is passed over in silence");
    {
        push_frame(&m, LB_KIND_READ, 0x100, 0, LB_DEV_DEVICE_TYPE, 0, NULL, 0);
        lb_node_tick(&node);
        LB_ASSERT(m.sent == 0);
    }

    LB_TEST("a report draws no reply — one answers questions");
    {
        push_frame(&m, LB_KIND_REPORT, ADDRESS, 4, 0x00, 0, NULL, 0);
        lb_node_tick(&node);
        LB_ASSERT(m.sent == 0);
    }

    printf("\nsystem endpoints (ep1 Diagnostics, ep2 Groups)\n");

    LB_TEST("Diagnostics counts the frames the node really saw");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 1, LB_DIAG_RX_FRAMES, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        LB_ASSERT(result == LB_RESULT_OK);
        lb_decode(LB_T_U32, data, 4, &v);
        LB_ASSERT(v.value > 0);
    }

    LB_TEST("what we do not know we do not invent — SupplyVoltage says it has none");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 1, 0x10, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ELEMENT);
    }

    LB_TEST("ErrorThreshold is the only setting — with bounds of its own, not a u16 range");
    {
        uint8_t too_small[2] = {8, 0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_DIAG_ERROR_THRESHOLD, 0, too_small, 2);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_INVALID_VALUE);

        uint8_t good[2] = {120, 0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_DIAG_ERROR_THRESHOLD, 0, good, 2);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && node.error_threshold == 120);
    }

    LB_TEST("a Diagnostics counter is for reading — a write is READ_ONLY");
    {
        uint8_t four[4] = {0, 0, 0, 0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_DIAG_RX_FRAMES, 0, four, 4);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_READ_ONLY);
    }

    LB_TEST("ResetCounters really does zero them");
    {
        push_frame(&m, LB_KIND_COMMAND, ADDRESS, 1, LB_DIAG_CMD_RESET, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && node.received == 0);
    }

    LB_TEST("GroupCapacity = 0 is a RESPONSE, not an absence");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 2, LB_GROUPS_CAPACITY, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && len == 1 && data[0] == 0);
    }

    LB_TEST("AddGroup at capacity 0 is INSUFFICIENT SPACE, not \"no such command\"");
    {
        uint8_t args[3] = {4, 5, 0};
        push_frame(&m, LB_KIND_COMMAND, ADDRESS, 2, 0x00, 0, args, 3);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_INSUFFICIENT_SPACE);
    }

    LB_TEST("the system endpoints have a TypeId too");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 1, LB_COMMON_TYPEID, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(result == LB_RESULT_OK && v.value == LB_TYPE_DIAGNOSTICS);
    }

    LB_TEST("Device has the common attributes as well — ep0 is no exception");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_COMMON_TYPEID, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(result == LB_RESULT_OK && v.value == LB_TYPE_DEVICE);
    }

    LB_TEST("a system endpoint: Channel 0, Enabled 1");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 2, LB_COMMON_CHANNEL, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && len == 1 && data[0] == 0);

        push_frame(&m, LB_KIND_READ, ADDRESS, 0, LB_COMMON_ENABLED, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && len == 1 && data[0] == 1);
    }

    LB_TEST("a system endpoint cannot be disabled — INVALID_VALUE, not READ_ONLY");
    {
        uint8_t off[1] = {0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_COMMON_ENABLED, 0, off, 1);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_INVALID_VALUE);

        uint8_t on[1] = {1};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_COMMON_ENABLED, 0, on, 1);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK);
    }

    LB_TEST("Diagnostics reports every 300 s by default, and that can be changed");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 1, LB_COMMON_REPORTINTERVAL, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        lb_value_t v;
        lb_decode(LB_T_U16, data, 2, &v);
        LB_ASSERT(result == LB_RESULT_OK && v.value == 300);

        uint8_t minute[2] = {60, 0};
        push_frame(&m, LB_KIND_WRITE, ADDRESS, 1, LB_COMMON_REPORTINTERVAL, 0, minute, 2);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK
                  && node.system[LB_EP_DIAGNOSTICS].common.report_interval == 60);
    }

    LB_TEST("a common attribute of the absent ep3 is still UNSUPPORTED_ENDPOINT");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 3, LB_COMMON_REPORTINTERVAL, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ENDPOINT);
    }

    LB_TEST("ep3 Scenes does not exist in this device");
    {
        push_frame(&m, LB_KIND_READ, ADDRESS, 3, 0x00, 0, NULL, 0);
        lb_node_tick(&node);
        last_result(&m, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_UNSUPPORTED_ENDPOINT);
    }

    printf("\nevents\n");

    LB_TEST("the sequence number goes up SEPARATELY for each endpoint");
    {
        m.sent = 0;
        lb_node_emit(&node, 4, 0x03, NULL, 0);
        lb_node_emit(&node, 4, 0x03, NULL, 0);
        lb_node_emit(&node, 0, LB_DEV_EVENT_BOOTED, NULL, 0);
        LB_ASSERT(m.sent == 3);
        LB_ASSERT(m.from_node[0].data[0] == 0);
        LB_ASSERT(m.from_node[1].data[0] == 1);
        LB_ASSERT(m.from_node[2].data[0] == 1); /* ep0 has a counter of its own */
    }

    printf("\njoining (03-adresacja.md §5)\n");

    /* A new node, with no address and no persistent store — this is what a device
     * out of the box looks like. */
    bus_t m2 = {0};
    lb_transport_t bus2 = {.send = m_send, .recv = m_recv, .ctx = &m2};
    lb_platform_t plat2 = {.millis = m_millis, .random = m_random, .ctx = &m2};
    lb_node_t fresh;
    lb_identity_t t2 = identity_cfg;
    t2.node_class = LB_CLASS_CONTROL;
    lb_node_init(&fresh, &bus2, &plat2, &t2, endpoints, 1);

    LB_TEST("a node with no address does NOT announce Booted — nothing to address it with");
    {
        lb_node_start(&fresh);
        LB_ASSERT(m2.sent == 0);
        LB_ASSERT(fresh.join_backoff_s == 2); /* but it scheduled one, in the 0…2 s window */
        LB_ASSERT(fresh.join_attempts == 0);
    }

    LB_TEST("the delay before announcing is RANDOM");
    {
        /* Were it fixed, forty nodes coming back after a power cut would announce in
         * the same millisecond. This test makes sure the randomness really enters the
         * result, rather than merely being called. */
        m2.jitter = 700;
        lb_node_t a;
        lb_node_init(&a, &bus2, &plat2, &t2, endpoints, 1);
        lb_node_start(&a);
        m2.jitter = 150;
        lb_node_t b;
        lb_node_init(&b, &bus2, &plat2, &t2, endpoints, 1);
        lb_node_start(&b);
        LB_ASSERT(a.next_join_ms != b.next_join_ms);
    }

    LB_TEST("silence before the deadline, an ANNOUNCEMENT after it");
    {
        m2.sent = 0;
        m2.jitter = 800;
        lb_node_init(&fresh, &bus2, &plat2, &t2, endpoints, 1);
        lb_node_start(&fresh);
        m2.clock = fresh.next_join_ms - 1;
        lb_node_tick(&fresh);
        LB_ASSERT(m2.sent == 0);

        m2.clock = fresh.next_join_ms;
        lb_node_tick(&fresh);
        LB_ASSERT(m2.sent == 1);

        const lb_frame_t *z = &m2.from_node[0];
        LB_ASSERT(lb_is_join(z->can_id));
        LB_ASSERT(((z->can_id >> LB_JOIN_DIRECTION_SHIFT) & 1u) == 1u); /* K=1 */
        LB_EQ_U32(lb_join_short_id(z->can_id), lb_node_short_id(&fresh) & 0xFFFFFFu);
        LB_ASSERT(z->len == 5);
        lb_value_t who;
        lb_decode(LB_T_U32, z->data, 4, &who);
        LB_EQ_U32((uint32_t)who.value, lb_node_short_id(&fresh));
        LB_ASSERT(z->data[4] == LB_CLASS_CONTROL);
    }

    LB_TEST("after an announcement, 500 ms for the assignment whatever the draw");
    {
        /* With a draw of 0 the delay is 0 — the wait is what keeps the bus quiet. */
        uint32_t kept = m2.jitter;
        m2.jitter = 0;
        m2.clock = fresh.next_join_ms;
        lb_node_tick(&fresh);
        LB_ASSERT(fresh.next_join_ms - m2.clock == 500u);
        m2.jitter = kept; /* the tests below share this platform */
    }

    LB_TEST("past the fifth attempt the windows grow — the hub may be off for hours");
    {
        uint16_t first = fresh.join_backoff_s;
        LB_ASSERT(first == 2);
        while (fresh.join_attempts < 10) {
            m2.clock = fresh.next_join_ms;
            lb_node_tick(&fresh);
        }
        LB_ASSERT(fresh.join_backoff_s == 60);
    }

    LB_TEST("somebody else’s assignment is passed over in silence");
    {
        m2.sent = 0;
        lb_frame_t foreign = {0};
        foreign.can_id = lb_join_id(0x00FFFFFF, false);
        lb_encode(LB_T_U32, LB_NUM(0x7FFFFFFF), foreign.data, 4);
        lb_encode(LB_T_U16, LB_NUM(0x123), foreign.data + 4, 2);
        foreign.len = 6;
        m2.to_node[m2.pending++] = foreign;
        lb_node_tick(&fresh);
        LB_ASSERT(fresh.address == LB_ADDRESS_UNASSIGNED);
        LB_ASSERT(m2.sent == 0);
    }

    LB_TEST("our assignment gives an address and ONLY THEN Booted");
    {
        m2.sent = 0;
        lb_frame_t p = {0};
        p.can_id = lb_join_id(lb_node_short_id(&fresh), false);
        lb_encode(LB_T_U32, LB_NUM((int32_t)lb_node_short_id(&fresh)), p.data, 4);
        lb_encode(LB_T_U16, LB_NUM(0x0A7), p.data + 4, 2);
        p.len = 6;
        m2.to_node[m2.pending++] = p;
        lb_node_tick(&fresh);

        LB_ASSERT(fresh.address == 0x0A7);
        LB_ASSERT(m2.sent == 1);
        lb_fields_t f = lb_decode_id(m2.from_node[0].can_id);
        LB_ASSERT(f.kind == LB_KIND_EVENT && f.address == 0x0A7);
        LB_ASSERT(f.element == LB_DEV_EVENT_BOOTED);
    }

    LB_TEST("a node that has an address does NOT announce any more");
    {
        m2.sent = 0;
        m2.clock += 600000; /* ten minutes later */
        lb_node_tick(&fresh);
        LB_ASSERT(m2.sent == 0);
    }

    printf("\nfactory reset (types/0x0001-device.md §3.3)\n");

    /* `fresh` has just been given 0x0A7 — the node the hub found at an address it did not
     * know, and the operator chose to send back to joining
     * (06-cykl-zycia-urzadzenia.md §2.3). */
    uint8_t magic[4];
    lb_encode(LB_T_U32, LB_NUM((int32_t)LB_FACTORY_RESET_MAGIC), magic, 4);

    LB_TEST("without the right argument FactoryReset is refused and nothing happens");
    {
        uint8_t wrong[4] = {0, 0, 0, 0};
        push_frame(&m2, LB_KIND_COMMAND, 0x0A7, 0, LB_DEV_CMD_FACTORY_RESET, 1, wrong, 4);
        lb_node_tick(&fresh);
        last_result(&m2, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_INVALID_VALUE);
        m2.clock += 1000;
        lb_node_tick(&fresh);
        LB_ASSERT(fresh.address == 0x0A7);
    }

    LB_TEST("the acknowledgement leaves from the OLD address — the hub has to hear it");
    {
        push_frame(&m2, LB_KIND_COMMAND, 0x0A7, 0, LB_DEV_CMD_FACTORY_RESET, 2, magic, 4);
        lb_node_tick(&fresh);
        last_result(&m2, &result, &tid, &data, &len);
        LB_ASSERT(result == LB_RESULT_OK && tid == 2);
        LB_ASSERT(lb_decode_id(m2.from_node[m2.sent - 1].can_id).address == 0x0A7);
        LB_ASSERT(fresh.address == 0x0A7); /* not yet — the reply is only queued */
    }

    LB_TEST("after the grace period the address is gone and the node announces anew");
    {
        m2.sent = 0;
        m2.clock += LB_RESET_GRACE_MS;
        lb_node_tick(&fresh);
        LB_ASSERT(fresh.address == LB_ADDRESS_UNASSIGNED);
        LB_ASSERT(fresh.join_attempts == 0);

        m2.clock = fresh.next_join_ms;
        lb_node_tick(&fresh);
        LB_ASSERT(m2.sent == 1 && lb_is_join(m2.from_node[0].can_id));
    }

    LB_TEST("with a restart available: the address is STORED as none, then the restart");
    {
        bus_t m3 = {0};
        lb_transport_t bus3 = {.send = m_send, .recv = m_recv, .ctx = &m3};
        lb_platform_t plat3 = {
            .millis = m_millis, .store = m_store, .restart = m_restart, .ctx = &m3,
        };
        lb_node_t board;
        lb_node_init(&board, &bus3, &plat3, &identity_cfg, endpoints, 1);
        board.address = ADDRESS;
        lb_node_start(&board);

        push_frame(&m3, LB_KIND_COMMAND, ADDRESS, 0, LB_DEV_CMD_FACTORY_RESET, 0, magic, 4);
        lb_node_tick(&board);
        LB_ASSERT(restarts == 0);
        m3.clock += LB_RESET_GRACE_MS;
        lb_node_tick(&board);
        LB_ASSERT(restarts == 1);
        LB_ASSERT(stored_address == LB_ADDRESS_UNASSIGNED);
    }

    LB_TEST("the state watch hears the start once, then changes only — not every tick");
    {
        bus_t m4 = {0};
        lb_transport_t bus4 = {.send = m_send, .recv = m_recv, .ctx = &m4};
        lb_platform_t plat4 = {.millis = m_millis, .ctx = &m4};
        lb_node_t led;
        lb_node_init(&led, &bus4, &plat4, &identity_cfg, endpoints, 1);
        led.address = ADDRESS;
        led.on_state = m_state;
        state_calls = 0;
        lb_node_start(&led);
        LB_ASSERT(state_calls == 1 && last_state == LB_STATE_READY);

        for (int i = 0; i < 100; i++) {
            m4.clock += 10;
            lb_node_tick(&led);
        }
        LB_ASSERT(state_calls == 1);

        uint8_t five_s[] = {5, 0};
        push_frame(&m4, LB_KIND_COMMAND, ADDRESS, 0, LB_DEV_CMD_IDENTIFY, 1, five_s, 2);
        lb_node_tick(&led);
        lb_node_tick(&led);
        LB_ASSERT(state_calls == 2 && last_state == LB_STATE_IDENTIFY);
    }

    LB_TEST("the button reports RESETTING at once, without waiting for a tick");
    {
        bus_t m5 = {0};
        lb_transport_t bus5 = {.send = m_send, .recv = m_recv, .ctx = &m5};
        lb_platform_t plat5 = {.millis = m_millis, .ctx = &m5};
        lb_node_t button;
        lb_node_init(&button, &bus5, &plat5, &identity_cfg, endpoints, 1);
        button.address = ADDRESS;
        button.on_state = m_state;
        lb_node_start(&button);
        state_calls = 0;
        lb_node_factory_reset(&button);
        LB_ASSERT(state_calls == 1 && last_state == LB_STATE_RESETTING);
        lb_node_factory_reset(&button); /* a second press: the same state, no report */
        LB_ASSERT(state_calls == 1);
        LB_ASSERT(m5.sent == 1); /* Booted only — the button puts nothing on the bus */
    }

    LB_TEST("no state watch: nothing is called and nothing breaks");
    {
        bus_t m6 = {0};
        lb_transport_t bus6 = {.send = m_send, .recv = m_recv, .ctx = &m6};
        lb_platform_t plat6 = {.millis = m_millis, .ctx = &m6};
        lb_node_t quiet;
        lb_node_init(&quiet, &bus6, &plat6, &identity_cfg, endpoints, 1);
        lb_node_start(&quiet);
        lb_node_factory_reset(&quiet);
        m6.clock += LB_RESET_GRACE_MS;
        lb_node_tick(&quiet);
        LB_ASSERT(quiet.state == LB_STATE_NO_ADDRESS);
    }

    LB_SUMMARY();
}
