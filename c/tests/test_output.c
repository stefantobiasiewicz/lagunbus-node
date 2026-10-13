/* The output: commands, behaviour after a restart, auto-off, reports. */
#include "lagunbus/types/output.h"
#include "lagunbus/registry.h"
#include "mock.h"
#include "lb_test.h"

#define ADDRESS 0x0A7u

static mock_t a;
static lb_node_t node;
static lb_output_t out;

static bool pin = false;
static uint8_t bounces = 0;

static void drive_output(void *ctx, bool level)
{
    (void)ctx;
    pin = level;
    bounces++;
}

static lb_attr_t attributes[] = {
    {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
    {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
    {LB_OUT_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)},
};

static lb_endpoint_t endpoints[] = {{
    .number = 4, .type_id = LB_TYPE_OUTPUT, .revision = 1, .channel = 1,
    .attrs = attributes, .attr_count = sizeof attributes / sizeof attributes[0],
}};

static void set_attr(uint8_t element, int32_t value)
{
    lb_endpoint_attr(&endpoints[0], element)->value = LB_NUM(value);
}

/** A common setting — the endpoint's `.common`, not a row (zadania/zrobione/TASK-01.md N1). */
static void set_min_gap(uint16_t ms)
{
    endpoints[0].common.report_min_gap = ms;
}

static lb_transport_t bus;
static lb_platform_t plat;
static lb_identity_t id;

/** Restart the node. `clean` wipes the persistent store, like a first boot. */
static void turn_on(bool clean)
{
    /* A restart clears RAM but not the persistent store — which is why we carry it
     * across the mock being zeroed. `clean` is a device straight out of the box. */
    mock_t old = a;
    memset(&a, 0, sizeof a);
    if (!clean) {
        memcpy(a.memory, old.memory, sizeof a.memory);
    }
    a.clock = 1000;

    bus = (lb_transport_t){.send = mock_send, .recv = mock_recv, .ctx = &a};
    plat = (lb_platform_t){.millis = mock_millis, .store = mock_store, .load = mock_load, .random = mock_random, .ctx = &a};
    id = (lb_identity_t){.uid = {1, 2, 3, 4, 5, 6, 7, 8}, .device_type = 0x0020,
                         .firmware = 1, .hardware = 1, .protocol = 1};
    lb_node_init(&node, &bus, &plat, &id, endpoints, 1);
    node.address = ADDRESS;
    lb_node_start(&node);
    a.sent = 0;

    bounces = 0;
    lb_output_init(&out, &node, &endpoints[0], drive_output, NULL);
}

static uint8_t report_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < a.sent; i++) {
        if (lb_decode_id(a.from_node[i].can_id).kind == LB_KIND_REPORT) {
            n++;
        }
    }
    return n;
}

int main(void)
{
    printf("commands\n");

    LB_TEST("On, Off and Toggle do what they say");
    {
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
        set_attr(LB_OUT_INVERT, 0);
        set_attr(LB_OUT_AUTO_OFF_TIME, 0);
        set_min_gap(0);
        turn_on(true);

        LB_ASSERT(lb_output_command(&out, LB_OUT_CMD_ON) == LB_RESULT_OK);
        LB_ASSERT(pin == true);
        LB_ASSERT(lb_output_command(&out, LB_OUT_CMD_TOGGLE) == LB_RESULT_OK);
        LB_ASSERT(pin == false);
        LB_ASSERT(lb_output_command(&out, LB_OUT_CMD_TOGGLE) == LB_RESULT_OK);
        LB_ASSERT(pin == true);
        LB_ASSERT(lb_output_command(&out, LB_OUT_CMD_OFF) == LB_RESULT_OK);
        LB_ASSERT(pin == false);
    }

    LB_TEST("a command from a feature that is absent says so outright");
    {
        /* SetLevel without the LEVEL feature. An answer of "no such command" is better
         * than silent disregard — the hub gets to learn its configuration does not
         * fit. */
        LB_ASSERT(lb_output_command(&out, 0x03) == LB_RESULT_UNSUPPORTED_ELEMENT);
    }

    LB_TEST("a repeated On does not touch the pin a second time");
    {
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        uint8_t after_first = bounces;
        lb_output_command(&out, LB_OUT_CMD_ON);
        LB_ASSERT(bounces == after_first);
    }

    printf("\nInvert — the receiver on the normally-closed contact\n");

    LB_TEST("on means the coil goes off, and the attribute still tells the truth");
    {
        set_attr(LB_OUT_INVERT, 1);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        LB_ASSERT(pin == false); /* coil off: the closed contact lights it */
        LB_ASSERT(lb_endpoint_attr(&endpoints[0], LB_OUT_ON_OFF)->value.value == 1);
        set_attr(LB_OUT_INVERT, 0);
    }

    LB_TEST("a write of Invert moves the coil at once, the receiver stays (BUG-07)");
    {
        set_attr(LB_OUT_INVERT, 0);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        LB_ASSERT(pin == true);
        set_attr(LB_OUT_INVERT, 1);
        LB_ASSERT(lb_output_write(&out, LB_OUT_INVERT, LB_NUM(1)) == LB_RESULT_OK);
        LB_ASSERT(pin == false); /* on, through the closed contact */
        LB_ASSERT(lb_endpoint_attr(&endpoints[0], LB_OUT_ON_OFF)->value.value == 1);
        set_attr(LB_OUT_INVERT, 0);
        LB_ASSERT(lb_output_write(&out, LB_OUT_INVERT, LB_NUM(0)) == LB_RESULT_OK);
        LB_ASSERT(pin == true);
        LB_ASSERT(lb_output_write(&out, LB_OUT_AUTO_OFF_TIME, LB_NUM(5)) == LB_RESULT_OK);
        LB_ASSERT(pin == true); /* any other setting moves nothing */
    }

    printf("\nPowerOnBehaviour\n");

    LB_TEST("OFF: off after a restart, however lit it was before");
    {
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        turn_on(false); /* restart with the store preserved */
        LB_ASSERT(pin == false);
    }

    LB_TEST("ON: on after a restart, however dark it was before");
    {
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_ON);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_OFF);
        turn_on(false);
        LB_ASSERT(pin == true);
    }

    LB_TEST("RESTORE really does survive a restart");
    {
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_RESTORE);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        LB_ASSERT(mock_has(&a, "output4"));
        turn_on(false);
        LB_ASSERT(pin == true);

        lb_output_command(&out, LB_OUT_CMD_OFF);
        turn_on(false);
        LB_ASSERT(pin == false);
    }

    LB_TEST("without RESTORE the persistent store is NOT ground down on every flick");
    {
        /* A relay in a hallway switches a dozen-odd times a day for twenty years. A
         * write on every switch would wear the storage out. */
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
        turn_on(true);
        for (int i = 0; i < 20; i++) {
            lb_output_command(&out, LB_OUT_CMD_TOGGLE);
        }
        LB_ASSERT(!mock_has(&a, "output4"));
    }

    printf("\nAutoOffTime\n");

    LB_TEST("the output goes off by itself after the configured time");
    {
        set_attr(LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
        set_attr(LB_OUT_AUTO_OFF_TIME, 5); /* seconds */
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);

        a.clock += 4000;
        lb_output_tick(&out, a.clock);
        LB_ASSERT(pin == true);

        a.clock += 1500;
        lb_output_tick(&out, a.clock);
        LB_ASSERT(pin == false);
    }

    LB_TEST("switching on again restarts the countdown");
    {
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        a.clock += 4000;
        lb_output_tick(&out, a.clock);
        lb_output_command(&out, LB_OUT_CMD_OFF);
        lb_output_command(&out, LB_OUT_CMD_ON);
        a.clock += 3000;
        lb_output_tick(&out, a.clock);
        LB_ASSERT(pin == true);
        set_attr(LB_OUT_AUTO_OFF_TIME, 0);
    }

    printf("\nreports\n");

    LB_TEST("every state change is a REPORT carrying the bare value");
    {
        set_min_gap(0);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);
        LB_ASSERT(report_count() == 1);

        const lb_frame_t *r = &a.from_node[a.sent - 1];
        lb_fields_t f = lb_decode_id(r->can_id);
        LB_ASSERT(f.endpoint == 4 && f.element == LB_OUT_ON_OFF);
        LB_ASSERT(r->len == 1 && r->data[0] == 1); /* no header byte */
    }

    LB_TEST("MinGap holds back a flood but does NOT LOSE the last state");
    {
        set_min_gap(1000);
        turn_on(true);
        lb_output_command(&out, LB_OUT_CMD_ON);   /* this one goes through */
        uint8_t after_first = report_count();

        a.clock += 100;
        lb_output_command(&out, LB_OUT_CMD_OFF);  /* this one is too soon */
        LB_ASSERT(report_count() == after_first);

        a.clock += 1500;
        lb_node_tick(&node); /* held-back reports are the node's since TASK-01 */
        LB_ASSERT(report_count() == after_first + 1);
        /* and carries the CURRENT state, not the one from a moment ago */
        LB_ASSERT(a.from_node[a.sent - 1].data[0] == 0);
        set_min_gap(0);
    }

    LB_SUMMARY();
}
