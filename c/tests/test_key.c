/* The key: contact bounce, clicks, hold.
 *
 * Everything here depends on time, so the clock is driven from the test. The same
 * cases on a board would mean pressing a button with a stopwatch in hand — and would
 * not give a repeatable result.
 */
#include "lagunbus/key.h"
#include "lagunbus/registry.h"
#include "mock.h"
#include "lb_test.h"

#define ADDRESS 0x0A7u

static mock_t a;
static lb_node_t node;
static lb_key_t key;

static lb_attr_t attributes[] = {
    {LB_KEY_PRESS_COUNT,        LB_T_U32,   LB_RO,              LB_NUM(0)},
    {LB_KEY_DEBOUNCE_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(15)},
    {LB_KEY_EVENT_MODE,         LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_KEY_MODE_CLICK)},
    {LB_KEY_MULTI_CLICK_WINDOW, LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(350)},
    {LB_KEY_MAX_CLICKS,         LB_T_U8,    LB_RW | LB_PERSIST, LB_NUM(3)},
    {LB_KEY_HOLD_ENABLED,       LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(1)},
    {LB_KEY_HOLD_THRESHOLD,     LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(500)},
    {LB_KEY_HOLD_TICK_INTERVAL, LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(200)},
    {LB_KEY_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(0)},
};

static lb_endpoint_t endpoints[] = {{
    .number = 4, .type_id = LB_TYPE_KEYINPUT, .revision = 1, .channel = 1,
    .attrs = attributes, .attr_count = sizeof attributes / sizeof attributes[0],
}};

static void set_attr(uint8_t element, int32_t value)
{
    lb_endpoint_attr(&endpoints[0], element)->value = LB_NUM(value);
}

/** A fresh key, released, clock from zero. */
static void reset_key(void)
{
    memset(&a, 0, sizeof a);
    a.clock = 1000;
    endpoints[0].event_seq = 0; /* a fresh board starts numbering from zero */
    static lb_transport_t bus;
    static lb_platform_t plat;
    bus = (lb_transport_t){.send = mock_send, .recv = mock_recv, .ctx = &a};
    plat = (lb_platform_t){.millis = mock_millis, .store = mock_store, .load = mock_load, .random = mock_random, .ctx = &a};

    static lb_identity_t id;
    id = (lb_identity_t){.uid = {1, 2, 3, 4, 5, 6, 7, 8}, .device_type = 0x0010,
                         .firmware = 1, .hardware = 1, .protocol = 1};
    lb_node_init(&node, &bus, &plat, &id, endpoints, 1);
    node.address = ADDRESS;
    lb_node_start(&node);
    a.sent = 0; /* Booted does not interest us here */

    set_attr(LB_KEY_DEBOUNCE_TIME, 15);
    set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_CLICK);
    set_attr(LB_KEY_HOLD_ENABLED, 1);
    set_attr(LB_KEY_HOLD_THRESHOLD, 500);
    set_attr(LB_KEY_HOLD_TICK_INTERVAL, 200);
    set_attr(LB_KEY_INVERT, 0);
    set_attr(LB_KEY_PRESS_COUNT, 0);
    lb_key_init(&key, &endpoints[0], false, a.clock);
}

/** Advance the clock, holding the pin at this level for the whole time. */
static void hold_for(bool level, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        a.clock++;
        lb_key_update(&key, &node, level, a.clock);
    }
}

int main(void)
{
    printf("contact bounce\n");

    LB_TEST("a short bounce produces NO event at all");
    {
        reset_key();
        /* The contact bounces for a few ms — shorter than DebounceTime = 15 ms. */
        for (int i = 0; i < 6; i++) {
            hold_for(i % 2 == 0, 2);
        }
        hold_for(false, 50);
        LB_ASSERT(a.sent == 0);
    }

    LB_TEST("a press with bounce produces ONE Down, not two");
    {
        reset_key();
        hold_for(true, 3);
        hold_for(false, 2);   /* bounce */
        hold_for(true, 100);  /* and steady from here */
        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 1);
    }

    printf("\nclick\n");

    LB_TEST("Down on press, Up with a duration, Click on release");
    {
        reset_key();
        hold_for(true, 100);
        hold_for(false, 50);

        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 1);
        LB_ASSERT(event_count(&a, LB_KEY_EV_UP) == 1);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);

        /* Click happens ON RELEASE — one cannot call a click something that is still
         * going on. The order on the bus bears that out. */
        const lb_frame_t *up = event_num(&a, LB_KEY_EV_UP, 0);
        lb_value_t elapsed;
        lb_decode(LB_T_U16, up->data + 1, 2, &elapsed);
        LB_ASSERT(elapsed.value >= 80 && elapsed.value <= 105);

        const lb_frame_t *click = event_num(&a, LB_KEY_EV_CLICK, 0);
        LB_ASSERT(click->data[1] == 1); /* [0] is the sequence number, [1] the count */
    }

    LB_TEST("PressCount goes up");
    {
        reset_key();
        hold_for(true, 50); hold_for(false, 50);
        hold_for(true, 50); hold_for(false, 50);
        LB_ASSERT(lb_endpoint_attr(&endpoints[0], LB_KEY_PRESS_COUNT)->value.value == 2);
    }

    printf("\nhold\n");

    LB_TEST("HoldStart after the threshold, then HoldTick at the set interval");
    {
        reset_key();
        hold_for(true, 450);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 0); /* not yet */
        hold_for(true, 100);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 1);
        hold_for(true, 450);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_TICK) == 2);
    }

    LB_TEST("a hold ends with HoldEnd and IS NOT a click");
    {
        reset_key();
        hold_for(true, 800);
        hold_for(false, 50);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_END) == 1);
        /* Were it to produce a Click as well, the hub would get two contradictory
         * messages about one press — and the relay would switch twice. */
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 0);
    }

    LB_TEST("HoldEnabled = false switches off hold and leaves the click");
    {
        reset_key();
        set_attr(LB_KEY_HOLD_ENABLED, 0);
        hold_for(true, 900);
        hold_for(false, 50);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 0);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);
    }

    printf("\nmulti-click\n");

    LB_TEST("two clicks inside the window give ONE Click carrying 2");
    {
        reset_key();
        set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_MULTICLICK);
        hold_for(true, 40); hold_for(false, 60);
        hold_for(true, 40); hold_for(false, 60);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 0); /* the window is still open */

        hold_for(false, 400);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);
        LB_ASSERT(event_num(&a, LB_KEY_EV_CLICK, 0)->data[1] == 2);
    }

    LB_TEST("a full house of clicks does not wait for the window to expire");
    {
        reset_key();
        set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_MULTICLICK);
        set_attr(LB_KEY_MAX_CLICKS, 3);
        for (int i = 0; i < 3; i++) {
            hold_for(true, 40);
            hold_for(false, 40);
        }
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);
        LB_ASSERT(event_num(&a, LB_KEY_EV_CLICK, 0)->data[1] == 3);
    }

    printf("\nthe rest\n");

    LB_TEST("Invert flips the input — a key shorted to ground");
    {
        reset_key();
        set_attr(LB_KEY_INVERT, 1);
        lb_key_init(&key, &endpoints[0], true, a.clock); /* at rest = high level */
        a.sent = 0;
        hold_for(false, 100);  /* shorted to ground = pressed */
        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 1);
    }

    LB_TEST("starting with the key HELD DOWN does not fake a press");
    {
        reset_key();
        lb_key_init(&key, &endpoints[0], true, a.clock);
        a.sent = 0;
        hold_for(true, 200);
        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 0);
    }

    LB_TEST("the event sequence number goes up, so the hub can see a lost one");
    {
        reset_key();
        hold_for(true, 50); hold_for(false, 50);
        LB_ASSERT(a.sent >= 3);
        LB_ASSERT(a.from_node[0].data[0] == 0);
        LB_ASSERT(a.from_node[1].data[0] == 1);
        LB_ASSERT(a.from_node[2].data[0] == 2);
    }

    LB_SUMMARY();
}
