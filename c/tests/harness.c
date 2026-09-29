/* The conformance harness — the C node behind a flat API for `ctypes`.
 *
 * The conformance tests (python/tests/conformance/) run one scenario through the C
 * node and through the Python node and compare what each put on the bus. Driving the
 * C node from Python needs *something* in between, and there were two ways:
 *
 * - mirror `lb_node_t` and friends as `ctypes.Structure`s and call the library
 *   directly. Rejected: `lb_node_t` is a large struct that changes with every feature,
 *   and a mirror that drifts from it by one field corrupts memory silently — the worst
 *   possible failure for a test whose whole job is to catch divergence;
 * - **this file**: a handful of functions with plain arguments, compiled together with
 *   the library into `liblagunbus_harness`. The structs never cross the boundary.
 *
 * The harness plays the part of the application AND of the port: an in-memory bus, a
 * clock set from the test, a key-value store, and the two device profiles from
 * `devices/apps/` — `key` and `output` — with exactly their attributes. The Python
 * side builds the same profiles in python/tests/conformance/profiles.py; a mismatch
 * between the two shows up in the first scenario that reads an attribute.
 *
 * This is test code: it allocates, which the library itself never does.
 */
#include "lagunbus/tools/debounce.h"
#include "lagunbus/types/key.h"
#include "lagunbus/node.h"
#include "lagunbus/types/output.h"
#include "lagunbus/registry.h"

#include <stdlib.h>
#include <string.h>

#define HARNESS_QUEUE 256
#define HARNESS_KEYS  8

typedef struct {
    char    key[16];
    uint8_t data[8];
    uint8_t len;
    bool    taken;
} harness_slot_t;

typedef struct lbh {
    lb_transport_t bus;
    lb_platform_t  platform;
    lb_node_t      node;

    lb_endpoint_t  endpoint;
    lb_attr_t      attrs[10];
    bool           is_key;
    lb_key_t       key;
    lb_key_config_t key_config;
    lb_debounce_t  debounce;
    uint32_t       debounce_ms;
    bool           active_low;  /**< the key shorts to ground: a LOW pin is a press */
    lb_output_t    output;
    int            driven;   /**< the last level the output drove; -1 before any */

    uint32_t       clock;
    uint32_t       jitter;

    lb_frame_t     inbox[HARNESS_QUEUE];
    uint16_t       inbox_len;
    lb_frame_t     outbox[HARNESS_QUEUE];
    uint16_t       outbox_len;

    harness_slot_t store[HARNESS_KEYS];
} lbh_t;

/* --- the port ---------------------------------------------------------------------- */

static bool h_send(void *ctx, const lb_frame_t *frame)
{
    lbh_t *h = ctx;
    if (h->outbox_len >= HARNESS_QUEUE) {
        return false;
    }
    h->outbox[h->outbox_len++] = *frame;
    return true;
}

static bool h_recv(void *ctx, lb_frame_t *out)
{
    lbh_t *h = ctx;
    if (h->inbox_len == 0) {
        return false;
    }
    *out = h->inbox[0];
    h->inbox_len--;
    memmove(h->inbox, h->inbox + 1, sizeof(lb_frame_t) * h->inbox_len);
    return true;
}

static uint32_t h_millis(void *ctx) { return ((lbh_t *)ctx)->clock; }
static uint32_t h_random(void *ctx) { return ((lbh_t *)ctx)->jitter; }

static harness_slot_t *h_slot(lbh_t *h, const char *key, bool create)
{
    harness_slot_t *free_slot = NULL;
    for (size_t i = 0; i < HARNESS_KEYS; i++) {
        if (h->store[i].taken && strcmp(h->store[i].key, key) == 0) {
            return &h->store[i];
        }
        if (!h->store[i].taken && free_slot == NULL) {
            free_slot = &h->store[i];
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    strncpy(free_slot->key, key, sizeof free_slot->key - 1);
    free_slot->taken = true;
    return free_slot;
}

static bool h_store(void *ctx, const char *key, const void *data, size_t len)
{
    if (len > 8) {
        return false;
    }
    harness_slot_t *slot = h_slot(ctx, key, true);
    if (slot == NULL) {
        return false;
    }
    memcpy(slot->data, data, len);
    slot->len = (uint8_t)len;
    return true;
}

static bool h_load(void *ctx, const char *key, void *out, size_t len)
{
    harness_slot_t *slot = h_slot(ctx, key, false);
    if (slot == NULL || slot->len != len) {
        return false;
    }
    memcpy(out, slot->data, len);
    return true;
}

/* --- the two profiles, copied from devices/apps/ ------------------------------------ */

static void h_drive(void *ctx, bool level)
{
    ((lbh_t *)ctx)->driven = level ? 1 : 0;
}

static uint8_t h_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                         uint8_t len)
{
    (void)args;
    (void)len;
    lbh_t *h = self->ctx;
    return lb_output_command(&h->output, element);
}

/* The key profiles share the board: a key shorted to ground (a LOW pin is a press),
 * filtered for 15 ms. Both are the DEVICE's decisions, made here as an application
 * makes them, and not attributes (types/0x0010-key-input.md §5.7). */
static void key_board(lbh_t *h)
{
    h->key_config = (lb_key_config_t)LB_KEY_CONFIG_DEFAULT;
    h->debounce_ms = 15;
    h->active_low = true;
    h->node.identity.device_type = 0x0010;
    h->is_key = true;
}

/** Everything: all three features, every timing tunable from the hub. */
static void profile_key(lbh_t *h)
{
    const lb_attr_t attrs[] = {
        LB_KEY_ATTR_PRESS_COUNT(),
        LB_KEY_ATTR_EVENT_MODE(),
        LB_KEY_ATTR_MULTI_CLICK_WINDOW(350),
        LB_KEY_ATTR_MAX_CLICKS(3),
        LB_KEY_ATTR_HOLD_ENABLED(),
        LB_KEY_ATTR_HOLD_THRESHOLD(500),
        LB_KEY_ATTR_HOLD_TICK_INTERVAL(200),
    };
    memcpy(h->attrs, attrs, sizeof attrs);
    h->endpoint = (lb_endpoint_t){
        .number = 4, .type_id = LB_TYPE_KEYINPUT, .revision = 1,
        .features = LB_KEY_F_MULTICLICK | LB_KEY_F_HOLD | LB_KEY_F_RAW,
        .channel = 1, .attrs = h->attrs,
        .attr_count = (uint8_t)(sizeof attrs / sizeof attrs[0]),
        .on_check = lb_key_check, .feature_of = lb_key_feature_of,
    };
    key_board(h);
}

/** A doorbell: no features, no attributes — the core alone. */
static void profile_bell(lbh_t *h)
{
    h->endpoint = (lb_endpoint_t){
        .number = 4, .type_id = LB_TYPE_KEYINPUT, .revision = 1, .features = 0,
        .channel = 1, .attrs = h->attrs, .attr_count = 0,
        .on_check = lb_key_check, .feature_of = lb_key_feature_of,
    };
    key_board(h);
}

/** HOLD and MULTICLICK, but the timings are the device's own: the hub may switch the
 * mode and hold on or off, and tune nothing. Values unlike the type's recommendations,
 * so that the scenario shows whose they are. */
static void profile_key_fixed(lbh_t *h)
{
    const lb_attr_t attrs[] = {
        LB_KEY_ATTR_EVENT_MODE(),
        LB_KEY_ATTR_HOLD_ENABLED(),
    };
    memcpy(h->attrs, attrs, sizeof attrs);
    h->endpoint = (lb_endpoint_t){
        .number = 4, .type_id = LB_TYPE_KEYINPUT, .revision = 1,
        .features = LB_KEY_F_HOLD | LB_KEY_F_MULTICLICK,
        .channel = 1, .attrs = h->attrs,
        .attr_count = (uint8_t)(sizeof attrs / sizeof attrs[0]),
        .on_check = lb_key_check, .feature_of = lb_key_feature_of,
    };
    key_board(h);
    h->key_config.hold_threshold_ms = 800;
    h->key_config.hold_tick_ms = 400;
    h->key_config.multi_click_window_ms = 250;
    h->key_config.max_clicks = 2;
}

static void profile_output(lbh_t *h)
{
    const lb_attr_t attrs[] = {
        {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
        {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
        {LB_OUT_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(1)},
        {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
        {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)},
        {LB_COMMON_REPORTMINGAP,    LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(200)},
    };
    memcpy(h->attrs, attrs, sizeof attrs);
    h->endpoint = (lb_endpoint_t){
        .number = 4, .type_id = LB_TYPE_OUTPUT, .revision = 1, .features = 0,
        .channel = 1, .attrs = h->attrs,
        .attr_count = (uint8_t)(sizeof attrs / sizeof attrs[0]),
        .on_command = h_command, .ctx = h,
    };
    h->node.identity.device_type = 0x0020;
    h->is_key = false;
}

/* --- the API ----------------------------------------------------------------------- */

lbh_t *lbh_create(const char *profile, const uint8_t *uid, uint32_t jitter)
{
    lbh_t *h = calloc(1, sizeof *h);
    if (h == NULL) {
        return NULL;
    }
    h->bus = (lb_transport_t){.send = h_send, .recv = h_recv, .ctx = h};
    h->platform = (lb_platform_t){
        .millis = h_millis, .store = h_store, .load = h_load, .random = h_random, .ctx = h,
    };
    h->jitter = jitter;
    h->driven = -1;

    lb_identity_t identity = {
        .node_class = LB_CLASS_CONTROL, .firmware = 0x00000100, .hardware = 1,
        .protocol = 1,
    };
    memcpy(identity.uid, uid, LB_UID_LEN);
    lb_node_init(&h->node, &h->bus, &h->platform, &identity, &h->endpoint, 1);

    if (strcmp(profile, "key") == 0) {
        profile_key(h);
    } else if (strcmp(profile, "bell") == 0) {
        profile_bell(h);
    } else if (strcmp(profile, "key_fixed") == 0) {
        profile_key_fixed(h);
    } else if (strcmp(profile, "output") == 0) {
        profile_output(h);
    } else {
        free(h);
        return NULL;
    }
    return h;
}

void lbh_destroy(lbh_t *h) { free(h); }

void lbh_set_clock(lbh_t *h, uint32_t ms) { h->clock = ms; }

bool lbh_store_put(lbh_t *h, const char *key, const uint8_t *data, uint8_t len)
{
    return h_store(h, key, data, len);
}

/** Stored length, or -1 when the key is not there. */
int lbh_store_get(lbh_t *h, const char *key, uint8_t *out, uint8_t cap)
{
    harness_slot_t *slot = h_slot(h, key, false);
    if (slot == NULL || slot->len > cap) {
        return -1;
    }
    memcpy(out, slot->data, slot->len);
    return slot->len;
}

/** Start in the order the applications use: node first, then the endpoint logic. */
void lbh_start(lbh_t *h, bool pin)
{
    if (h->is_key) {
        lb_node_start(&h->node);
        bool pressed = h->active_low ? !pin : pin;
        lb_debounce_init(&h->debounce, pressed, h->clock);
        lb_key_init(&h->key, &h->endpoint, &h->key_config, pressed, h->clock);
    } else {
        lb_node_start(&h->node);
        lb_output_init(&h->output, &h->node, &h->endpoint, h_drive, h);
    }
}

void lbh_rx(lbh_t *h, uint32_t can_id, const uint8_t *data, uint8_t len)
{
    if (h->inbox_len >= HARNESS_QUEUE || len > LB_MAX_PAYLOAD) {
        return;
    }
    lb_frame_t *f = &h->inbox[h->inbox_len++];
    memset(f, 0, sizeof *f);
    f->can_id = can_id;
    f->len = len;
    memcpy(f->data, data, len);
}

/** One turn of an application's loop: the node, then the endpoint logic. */
void lbh_loop(lbh_t *h, bool pin)
{
    lb_node_tick(&h->node);
    if (h->is_key) {
        bool pressed = h->active_low ? !pin : pin;
        bool stable = lb_debounce_update(&h->debounce, pressed, h->clock, h->debounce_ms);
        lb_key_update(&h->key, &h->node, stable, h->clock);
    } else {
        lb_output_tick(&h->output, h->clock);
    }
}

int lbh_tx_count(const lbh_t *h) { return h->outbox_len; }

void lbh_tx_get(const lbh_t *h, int i, uint32_t *can_id, uint8_t *data, uint8_t *len)
{
    const lb_frame_t *f = &h->outbox[i];
    *can_id = f->can_id;
    *len = f->len;
    memcpy(data, f->data, f->len);
}

void lbh_tx_clear(lbh_t *h) { h->outbox_len = 0; }

int lbh_driven(const lbh_t *h) { return h->driven; }

uint16_t lbh_address(const lbh_t *h) { return h->node.address; }

uint32_t lbh_short_id(const lbh_t *h) { return lb_node_short_id(&h->node); }
