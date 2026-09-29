/* The key tools: contact bounce, clicks, hold, and what a device leaves out.
 *
 * Everything here depends on time, so the clock is driven from the test. The same
 * cases on a board would mean pressing a button with a stopwatch in hand — and would
 * not give a repeatable result. The input goes through `lb_debounce_t` first, the way
 * an application feeds it.
 */
#include "lagunbus/tools/debounce.h"
#include "lagunbus/types/key.h"
#include "lagunbus/registry.h"
#include "mock.h"
#include "lb_test.h"

#define ADDRESS 0x0A7u

static mock_t a;
static lb_node_t node;
static lb_key_t key;
static lb_debounce_t debounce;
static lb_key_config_t config = LB_KEY_CONFIG_DEFAULT;

#define DEBOUNCE_MS 15u
#define ALL_FEATURES (LB_KEY_F_MULTICLICK | LB_KEY_F_HOLD | LB_KEY_F_RAW)

static lb_attr_t attributes[] = {
    LB_KEY_ATTR_PRESS_COUNT(),
    LB_KEY_ATTR_EVENT_MODE(),
    LB_KEY_ATTR_MULTI_CLICK_WINDOW(350),
    LB_KEY_ATTR_MAX_CLICKS(3),
    LB_KEY_ATTR_HOLD_ENABLED(),
    LB_KEY_ATTR_HOLD_THRESHOLD(500),
    LB_KEY_ATTR_HOLD_TICK_INTERVAL(200),
};

static lb_endpoint_t endpoints[] = {{
    .number = 4, .type_id = LB_TYPE_KEYINPUT, .revision = 1, .channel = 1,
    .features = ALL_FEATURES,
    .attrs = attributes, .attr_count = sizeof attributes / sizeof attributes[0],
    .on_check = lb_key_check, .feature_of = lb_key_feature_of,
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

    endpoints[0].features = ALL_FEATURES;
    endpoints[0].attrs = attributes;
    endpoints[0].attr_count = sizeof attributes / sizeof attributes[0];
    config = (lb_key_config_t)LB_KEY_CONFIG_DEFAULT;
    set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_CLICK);
    set_attr(LB_KEY_HOLD_ENABLED, 1);
    set_attr(LB_KEY_HOLD_THRESHOLD, 500);
    set_attr(LB_KEY_HOLD_TICK_INTERVAL, 200);
    set_attr(LB_KEY_PRESS_COUNT, 0);
    lb_debounce_init(&debounce, false, a.clock);
    lb_key_init(&key, &endpoints[0], &config, false, a.clock);
}

/** Start the key over with the finger already on it — as a board powered up that way. */
static void start_pressed(void)
{
    lb_debounce_init(&debounce, true, a.clock);
    lb_key_init(&key, &endpoints[0], &config, true, a.clock);
    a.sent = 0;
}

/** Advance the clock with the finger on (`true`) or off the key the whole time. */
static void hold_for(bool pressed, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        a.clock++;
        bool stable = lb_debounce_update(&debounce, pressed, a.clock, DEBOUNCE_MS);
        lb_key_update(&key, &node, stable, a.clock);
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
        set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_RAW);
        hold_for(true, 3);
        hold_for(false, 2);   /* bounce */
        hold_for(true, 100);  /* and steady from here */
        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 1);
    }

    printf("\nclick\n");

    LB_TEST("CLICK: one Click on release and nothing else — no Down, no Up");
    {
        reset_key();
        hold_for(true, 100);
        LB_ASSERT(a.sent == 0); /* one cannot call a click something still going on */
        hold_for(false, 50);

        LB_ASSERT(a.sent == 1);
        const lb_frame_t *click = event_num(&a, LB_KEY_EV_CLICK, 0);
        LB_ASSERT(click != NULL);
        LB_ASSERT(click->data[1] == 1); /* [0] is the sequence number, [1] the count */
    }

    LB_TEST("RAW: Down on press, Up with a duration on release, no Click");
    {
        reset_key();
        set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_RAW);
        hold_for(true, 100);
        LB_ASSERT(event_count(&a, LB_KEY_EV_DOWN) == 1);
        hold_for(false, 50);

        LB_ASSERT(event_count(&a, LB_KEY_EV_UP) == 1);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 0);
        const lb_frame_t *up = event_num(&a, LB_KEY_EV_UP, 0);
        LB_ASSERT(up != NULL);
        lb_value_t elapsed;
        lb_decode(LB_T_U16, up->data + 1, 2, &elapsed);
        LB_ASSERT(elapsed.value >= 80 && elapsed.value <= 105);
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

    LB_TEST("RAW: hold works too, and the release is HoldEnd ALONE — no Up");
    {
        reset_key();
        set_attr(LB_KEY_EVENT_MODE, LB_KEY_MODE_RAW);
        hold_for(true, 800);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 1);
        hold_for(false, 50);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_END) == 1);
        /* A rule on Up must not fire after a hold. */
        LB_ASSERT(event_count(&a, LB_KEY_EV_UP) == 0);
    }

    LB_TEST("after 60 s held HoldTick stops, and HoldEnd still comes");
    {
        reset_key();
        /* One tick a second, so that a minute of them fits the mock's 64 frames. */
        set_attr(LB_KEY_HOLD_TICK_INTERVAL, 1000);
        hold_for(true, LB_KEY_HOLD_TICK_LIMIT_MS);
        LB_ASSERT(a.sent < MOCK_CAPACITY);
        uint8_t ticks = event_count(&a, LB_KEY_EV_HOLD_TICK);
        hold_for(true, 5000);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_TICK) == ticks);
        hold_for(false, 50);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_END) == 1);
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

    LB_TEST("starting with the key HELD DOWN does not fake a press");
    {
        reset_key();
        start_pressed();
        hold_for(true, 800);  /* past the hold threshold, too */
        hold_for(false, 50);
        LB_ASSERT(a.sent == 0); /* the release of a press never seen is not a click */
    }

    LB_TEST("...unless the device chooses otherwise (silent_start_press = false)");
    {
        reset_key();
        config.silent_start_press = false;
        start_pressed();
        hold_for(true, 200);
        hold_for(false, 50);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);
    }

    printf("\nwhat a device leaves out\n");

    LB_TEST("a doorbell: no features, no attributes — a long press is still one Click");
    {
        reset_key();
        endpoints[0].features = 0;
        endpoints[0].attr_count = 0;
        hold_for(true, 900);
        hold_for(false, 50);
        LB_ASSERT(a.sent == 1);
        LB_ASSERT(event_count(&a, LB_KEY_EV_CLICK) == 1);

        uint8_t out[8];
        uint8_t len = 0;
        LB_ASSERT(lb_endpoint_read(&endpoints[0], LB_KEY_EVENT_MODE, out, sizeof out, &len)
                  == LB_RESULT_UNSUPPORTED_FEATURE);
        LB_ASSERT(lb_endpoint_read(&endpoints[0], 0x10, out, sizeof out, &len)
                  == LB_RESULT_UNSUPPORTED_ELEMENT); /* not an element of the type any more */
    }

    LB_TEST("a timing the device does not expose comes from its config");
    {
        reset_key();
        static lb_attr_t fixed[] = {LB_KEY_ATTR_HOLD_ENABLED()};
        endpoints[0].attrs = fixed;
        endpoints[0].attr_count = 1;
        endpoints[0].features = LB_KEY_F_HOLD;
        config.hold_threshold_ms = 800;
        hold_for(true, 700);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 0); /* 500 is not the rule here */
        hold_for(true, 150);
        LB_ASSERT(event_count(&a, LB_KEY_EV_HOLD_START) == 1);
        endpoints[0].attrs = attributes;
    }

    LB_TEST("a write outside the type's range is refused whole");
    {
        reset_key();
        uint8_t ten[2] = {10, 0};
        LB_ASSERT(lb_endpoint_write(&endpoints[0], LB_KEY_HOLD_THRESHOLD, ten, 2)
                  == LB_RESULT_INVALID_VALUE);
        LB_ASSERT(lb_endpoint_attr(&endpoints[0], LB_KEY_HOLD_THRESHOLD)->value.value == 500);
    }

    LB_TEST("EventMode takes only a mode whose feature is declared");
    {
        reset_key();
        endpoints[0].features = LB_KEY_F_MULTICLICK;
        uint8_t raw[1] = {LB_KEY_MODE_RAW};
        uint8_t multi[1] = {LB_KEY_MODE_MULTICLICK};
        LB_ASSERT(lb_endpoint_write(&endpoints[0], LB_KEY_EVENT_MODE, raw, 1)
                  == LB_RESULT_INVALID_VALUE);
        LB_ASSERT(lb_endpoint_write(&endpoints[0], LB_KEY_EVENT_MODE, multi, 1)
                  == LB_RESULT_OK);
    }

    LB_TEST("an emitted duration saturates instead of wrapping");
    {
        reset_key();
        lb_key_emit_up(&node, 4, 70000);
        const lb_frame_t *up = event_num(&a, LB_KEY_EV_UP, 0);
        LB_ASSERT(up != NULL && up->data[1] == 0xFF && up->data[2] == 0xFF);
    }

    LB_TEST("the event sequence number goes up, so the hub can see a lost one");
    {
        reset_key();
        for (int i = 0; i < 3; i++) {
            hold_for(true, 50);
            hold_for(false, 50);
        }
        LB_ASSERT(a.sent == 3);
        LB_ASSERT(a.from_node[0].data[0] == 0);
        LB_ASSERT(a.from_node[1].data[0] == 1);
        LB_ASSERT(a.from_node[2].data[0] == 2);
    }

    LB_SUMMARY();
}
