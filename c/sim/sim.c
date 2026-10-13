/* The simulated device: the port (bus, clock, store in memory) and the flat API.
 * What and why — sim.h. The devices themselves — devices.c. */
#include "sim.h"

#include "lagunbus/registry.h"

#include <stdlib.h>
#include <string.h>

/* --- the port ---------------------------------------------------------------------- */

static bool s_send(void *ctx, const lb_frame_t *frame)
{
    lbs_t *sim = ctx;
    if (sim->outbox_len >= LBS_QUEUE) {
        return false;
    }
    sim->outbox[sim->outbox_len++] = *frame;
    return true;
}

static bool s_recv(void *ctx, lb_frame_t *out)
{
    lbs_t *sim = ctx;
    if (sim->inbox_len == 0) {
        return false;
    }
    *out = sim->inbox[0];
    sim->inbox_len--;
    memmove(sim->inbox, sim->inbox + 1, sizeof(lb_frame_t) * sim->inbox_len);
    return true;
}

static uint32_t s_millis(void *ctx) { return ((lbs_t *)ctx)->clock; }

static void s_restart(void *ctx) { ((lbs_t *)ctx)->restart_requested = true; }
static uint8_t s_reset_reason(void *ctx) { return ((lbs_t *)ctx)->reset_reason; }

static bool s_health(void *ctx, uint16_t *tec, uint16_t *rec, uint8_t *state)
{
    lbs_t *sim = ctx;
    if (!sim->has_health) {
        return false;
    }
    *tec = sim->tec;
    *rec = sim->rec;
    *state = sim->bus_state;
    return true;
}

static void s_state(void *ctx, lb_node_state_t state)
{
    lbs_t *sim = ctx;
    sim->state = (int)state;
    sim->state_reports++;
    lb_led_on_state(&sim->led, state); /* and on to the LED, as a board wires it */
}

static void s_led(void *ctx, bool on) { ((lbs_t *)ctx)->led_lit = on; }
static uint32_t s_random(void *ctx) { return ((lbs_t *)ctx)->jitter; }

static lbs_slot_t *s_slot(lbs_t *sim, const char *key, bool create)
{
    lbs_slot_t *free_slot = NULL;
    for (size_t i = 0; i < LBS_SLOTS; i++) {
        if (sim->store[i].taken && strcmp(sim->store[i].key, key) == 0) {
            return &sim->store[i];
        }
        if (!sim->store[i].taken && free_slot == NULL) {
            free_slot = &sim->store[i];
        }
    }
    if (!create || free_slot == NULL || strlen(key) >= sizeof free_slot->key) {
        return NULL;
    }
    strncpy(free_slot->key, key, sizeof free_slot->key - 1);
    free_slot->taken = true;
    return free_slot;
}

static bool s_store(void *ctx, const char *key, const void *data, size_t len)
{
    lbs_t *sim = ctx;
    if (len > LBS_VALUE) {
        return false;
    }
    lbs_slot_t *slot = s_slot(sim, key, true);
    if (slot == NULL) {
        return false;
    }
    memcpy(slot->data, data, len);
    slot->len = (uint8_t)len;
    sim->store_version++;
    return true;
}

static void s_erase(void *ctx, const char *key)
{
    lbs_t *sim = ctx;
    lbs_slot_t *slot = s_slot(sim, key, false);
    if (slot != NULL) {
        memset(slot, 0, sizeof *slot);
        sim->store_version++;
    }
}

static bool s_load(void *ctx, const char *key, void *out, size_t len)
{
    lbs_slot_t *slot = s_slot(ctx, key, false);
    if (slot == NULL || slot->len != len) {
        return false;
    }
    memcpy(out, slot->data, len);
    return true;
}

/* --- endpoints ----------------------------------------------------------------------- */

static void s_drive(void *ctx, bool level)
{
    lbs_channel_t *ch = ctx;
    ch->sim->driven[ch->index] = level ? 1 : 0;
}

static uint8_t s_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    lbs_channel_t *ch = self->ctx;
    return lb_output_write(&ch->sim->outputs[ch->index], element, value);
}

static uint8_t s_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                         uint8_t len)
{
    (void)args;
    (void)len;
    lbs_channel_t *ch = self->ctx;
    return lb_output_command(&ch->sim->outputs[ch->index], element);
}

static lb_display_t *s_display(lb_endpoint_t *self)
{
    lbs_channel_t *ch = self->ctx;
    return &ch->sim->displays[ch->index];
}

static uint8_t s_display_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    return lb_display_write(s_display(self), element, value);
}

static uint8_t s_display_write_text(lb_endpoint_t *self, uint8_t element,
                                    const uint8_t *value, uint8_t len)
{
    return lb_display_write_text(s_display(self), element, value, len);
}

static uint8_t s_display_read_text(lb_endpoint_t *self, uint8_t element, uint8_t *out,
                                   uint8_t cap, uint8_t *len)
{
    return lb_display_read_text(s_display(self), element, out, cap, len);
}

static void s_drawn(void *ctx, const lb_display_t *field)
{
    (void)field;
    ((lbs_t *)ctx)->drawn++;
}

/** The next endpoint, its attribute rows copied in. NULL when the device is full. */
static lb_endpoint_t *s_next(lbs_t *sim, uint16_t type, const lb_attr_t *attrs,
                             uint8_t count, lbs_role_t role, uint8_t index)
{
    if (sim->endpoint_count >= LBS_ENDPOINTS || count > LBS_ATTRS) {
        return NULL;
    }
    uint8_t i = sim->endpoint_count++;
    if (count > 0) {
        memcpy(sim->attrs[i], attrs, sizeof(lb_attr_t) * count);
    }
    sim->role[i] = role;
    sim->index[i] = index;
    sim->endpoints[i] = (lb_endpoint_t){
        .number = (uint8_t)(4u + i), .type_id = type, .revision = 1,
        .channel = (uint8_t)(index + 1u), .attrs = sim->attrs[i], .attr_count = count,
    };
    return &sim->endpoints[i];
}

bool lbs_add_key(lbs_t *sim, uint16_t features, const lb_attr_t *attrs, uint8_t count)
{
    lb_endpoint_t *ep = s_next(sim, LB_TYPE_KEYINPUT, attrs, count, LBS_KEY,
                               sim->input_count);
    if (ep == NULL) {
        return false;
    }
    sim->input_count++;
    ep->features = features;
    ep->on_check = lb_key_check;
    ep->feature_of = lb_key_feature_of;
    return true;
}

lb_endpoint_t *lbs_add_output(lbs_t *sim, const lb_attr_t *attrs, uint8_t count)
{
    uint8_t index = sim->output_count;
    lb_endpoint_t *ep = s_next(sim, LB_TYPE_OUTPUT, attrs, count, LBS_OUTPUT, index);
    if (ep == NULL) {
        return NULL;
    }
    sim->output_count++;
    ep->on_command = s_command;
    ep->on_write = s_write;
    ep->ctx = &sim->channels[index];
    return ep;
}

lb_endpoint_t *lbs_add_display(lbs_t *sim, uint16_t features, const lb_attr_t *attrs,
                               uint8_t count, uint16_t own_timeout_s)
{
    uint8_t index = sim->display_count;
    lb_endpoint_t *ep = s_next(sim, LB_TYPE_DISPLAY, attrs, count, LBS_DISPLAY, index);
    if (ep == NULL) {
        return NULL;
    }
    sim->display_count++;
    ep->channel = 0;
    ep->features = features;
    ep->on_check = lb_display_check;
    ep->feature_of = lb_display_feature_of;
    ep->on_write = s_display_write;
    ep->on_write_long = s_display_write_text;
    ep->on_read_long = s_display_read_text;
    sim->display_channels[index] = (lbs_channel_t){.sim = sim, .index = index};
    ep->ctx = &sim->display_channels[index];
    sim->own_timeout[index] = own_timeout_s;
    return ep;
}

lb_endpoint_t *lbs_add_sense(lbs_t *sim, uint8_t quantity, uint16_t features,
                             const lb_attr_t *attrs, uint8_t count)
{
    uint8_t index = sim->sense_count;
    lb_endpoint_t *ep = s_next(sim, LB_TYPE_SENSE, attrs, count, LBS_SENSE, index);
    if (ep == NULL) {
        return NULL;
    }
    sim->sense_count++;
    ep->channel = 0;
    ep->features = features;
    ep->feature_of = lb_sense_feature_of;
    lb_sense_defaults(ep, quantity);
    /* The endpoint now, not at `lb_sense_init`: Python may list the sensors before the
     * start (`lagun-sim --headless` does), and a NULL there is a crash, not an empty list. */
    sim->senses[index].endpoint = ep;
    sim->senses[index].node = &sim->node;
    return ep;
}

static bool s_pressed(const lbs_t *sim, uint32_t pins, uint8_t input)
{
    bool level = (pins >> input) & 1u;
    return sim->active_low ? !level : level;
}

/* --- the API ----------------------------------------------------------------------- */

size_t lbs_device_count(void) { return lbs_device_total; }

const char *lbs_device_name(size_t i)
{
    return i < lbs_device_total ? lbs_devices[i].name : NULL;
}

const char *lbs_device_about(size_t i)
{
    return i < lbs_device_total ? lbs_devices[i].about : NULL;
}

int lbs_device_max_channels(size_t i)
{
    return i < lbs_device_total ? lbs_devices[i].max_channels : -1;
}

lbs_t *lbs_create(const char *name, const uint8_t *uid, uint32_t jitter, uint8_t channels)
{
    const lbs_device_t *device = NULL;
    for (size_t i = 0; i < lbs_device_total; i++) {
        if (strcmp(lbs_devices[i].name, name) == 0) {
            device = &lbs_devices[i];
        }
    }
    if (device == NULL) {
        return NULL;
    }
    if (device->max_channels == 0) {
        if (channels != 0) {
            return NULL; /* a fixed device comes as it is */
        }
    } else if (channels == 0) {
        channels = 1;
    } else if (channels > device->max_channels) {
        return NULL;
    }
    lbs_t *sim = calloc(1, sizeof *sim);
    if (sim == NULL) {
        return NULL;
    }
    sim->bus = (lb_transport_t){.send = s_send, .recv = s_recv, .ctx = sim};
    sim->platform = (lb_platform_t){
        .millis = s_millis, .store = s_store, .load = s_load, .erase = s_erase,
        .random = s_random,
        .reset_reason = s_reset_reason, .ctx = sim,
    };
    sim->state = -1;
    sim->jitter = jitter;
    for (uint8_t i = 0; i < LBS_ENDPOINTS; i++) {
        sim->driven[i] = -1;
        sim->channels[i] = (lbs_channel_t){.sim = sim, .index = i};
    }
    sim->key_config = (lb_key_config_t)LB_KEY_CONFIG_DEFAULT;
    sim->node_class = LB_CLASS_CONTROL;

    device->define(sim, channels);

    lb_identity_t identity = {
        .node_class = sim->node_class, .device_type = sim->device_type,
        .firmware = 0x00000100, .hardware = 1, .protocol = 1,
    };
    memcpy(identity.uid, uid, LB_UID_LEN);
    lb_node_init(&sim->node, &sim->bus, &sim->platform, &identity, sim->endpoints,
                 sim->endpoint_count);
    sim->node.on_state = s_state;
    sim->node.state_ctx = sim;
    lb_led_init(&sim->led, s_led, sim);
    return sim;
}

void lbs_destroy(lbs_t *sim) { free(sim); }

void lbs_set_clock(lbs_t *sim, uint32_t ms) { sim->clock = ms; }

void lbs_allow_restart(lbs_t *sim) { sim->platform.restart = s_restart; }

bool lbs_restart_requested(const lbs_t *sim) { return sim->restart_requested; }

bool lbs_store_put(lbs_t *sim, const char *key, const uint8_t *data, uint8_t len)
{
    return s_store(sim, key, data, len);
}

/** Stored length, or -1 when the key is not there. */
int lbs_store_get(lbs_t *sim, const char *key, uint8_t *out, uint8_t cap)
{
    lbs_slot_t *slot = s_slot(sim, key, false);
    if (slot == NULL || slot->len > cap) {
        return -1;
    }
    memcpy(out, slot->data, slot->len);
    return slot->len;
}

/** Slot `slot` of the store: its key into `key` (16 bytes), its value into `out`
 * (`LBS_VALUE` bytes). The length, or -1 for an empty slot. Lets Python save the whole
 * store without knowing which keys the library uses. */
int lbs_store_at(const lbs_t *sim, int slot, char *key, uint8_t *out)
{
    if (slot < 0 || slot >= LBS_SLOTS || !sim->store[slot].taken) {
        return -1;
    }
    const lbs_slot_t *s = &sim->store[slot];
    memcpy(key, s->key, sizeof s->key);
    memcpy(out, s->data, s->len);
    return s->len;
}

int lbs_store_slots(void) { return LBS_SLOTS; }

uint32_t lbs_store_version(const lbs_t *sim) { return sim->store_version; }

void lbs_start(lbs_t *sim, uint32_t pins)
{
    lb_node_start(&sim->node);
    for (uint8_t i = 0; i < sim->endpoint_count; i++) {
        uint8_t n = sim->index[i];
        if (sim->role[i] == LBS_KEY) {
            bool pressed = s_pressed(sim, pins, n);
            lb_debounce_init(&sim->debounce[n], pressed, sim->clock);
            lb_key_init(&sim->keys[n], &sim->endpoints[i], &sim->key_config, pressed,
                        sim->clock);
        } else if (sim->role[i] == LBS_OUTPUT) {
            lb_output_init(&sim->outputs[n], &sim->node, &sim->endpoints[i], s_drive,
                           &sim->channels[n]);
        } else if (sim->role[i] == LBS_DISPLAY) {
            lb_display_init(&sim->displays[n], &sim->node, &sim->endpoints[i],
                            sim->own_timeout[n], s_drawn, sim);
        } else if (sim->role[i] == LBS_SENSE) {
            lb_sense_init(&sim->senses[n], &sim->node, &sim->endpoints[i]);
        }
    }
}

void lbs_rx(lbs_t *sim, uint32_t can_id, const uint8_t *data, uint8_t len)
{
    if (sim->inbox_len >= LBS_QUEUE || len > LB_MAX_PAYLOAD) {
        return;
    }
    lb_frame_t *f = &sim->inbox[sim->inbox_len++];
    memset(f, 0, sizeof *f);
    f->can_id = can_id;
    f->len = len;
    memcpy(f->data, data, len);
}

void lbs_loop(lbs_t *sim, uint32_t pins)
{
    lb_node_tick(&sim->node);
    lb_led_tick(&sim->led, sim->clock);
    for (uint8_t i = 0; i < sim->endpoint_count; i++) {
        uint8_t n = sim->index[i];
        if (sim->role[i] == LBS_KEY) {
            bool stable = lb_debounce_update(&sim->debounce[n], s_pressed(sim, pins, n),
                                             sim->clock, sim->debounce_ms);
            lb_key_update(&sim->keys[n], &sim->node, stable, sim->clock);
        } else if (sim->role[i] == LBS_OUTPUT) {
            lb_output_tick(&sim->outputs[n], sim->clock);
        } else if (sim->role[i] == LBS_DISPLAY) {
            lb_display_tick(&sim->displays[n], sim->clock);
        }
    }
}

int lbs_tx_count(const lbs_t *sim) { return sim->outbox_len; }

void lbs_tx_get(const lbs_t *sim, int i, uint32_t *can_id, uint8_t *data, uint8_t *len)
{
    const lb_frame_t *f = &sim->outbox[i];
    *can_id = f->can_id;
    *len = f->len;
    memcpy(data, f->data, f->len);
}

void lbs_tx_clear(lbs_t *sim) { sim->outbox_len = 0; }

int lbs_input_count(const lbs_t *sim) { return sim->input_count; }

/** The pin levels with nothing pressed — what Python flips a bit of for a press. */
uint32_t lbs_inputs_idle(const lbs_t *sim)
{
    uint32_t all = sim->input_count >= 32 ? 0xFFFFFFFFu : (1u << sim->input_count) - 1u;
    return sim->active_low ? all : 0u;
}

int lbs_output_count(const lbs_t *sim) { return sim->output_count; }

/** The last level output `output` drove, or -1 before any (or past the last). */
int lbs_driven(const lbs_t *sim, int output)
{
    return output >= 0 && output < LBS_ENDPOINTS ? sim->driven[output] : -1;
}

/** `OnOff` of output `output` — the receiver's state, before `Invert`. -1 past the last
 * output. */
int lbs_output_on(const lbs_t *sim, int output)
{
    if (output < 0 || output >= sim->output_count) {
        return -1;
    }
    const lb_attr_t *on_off = NULL;
    for (uint8_t i = 0; i < sim->endpoint_count; i++) {
        if (sim->role[i] == LBS_OUTPUT && sim->index[i] == output) {
            for (uint8_t a = 0; a < sim->endpoints[i].attr_count; a++) {
                if (sim->endpoints[i].attrs[a].id == LB_OUT_ON_OFF) {
                    on_off = &sim->endpoints[i].attrs[a];
                }
            }
        }
    }
    return on_off != NULL && !on_off->value.absent && on_off->value.value != 0 ? 1 : 0;
}

int lbs_display_count(const lbs_t *sim) { return sim->display_count; }

static const lb_display_t *s_field(const lbs_t *sim, int field)
{
    return field >= 0 && field < sim->display_count ? &sim->displays[field] : NULL;
}

int lbs_display_endpoint(const lbs_t *sim, int field)
{
    const lb_display_t *d = s_field(sim, field);
    return d == NULL ? -1 : d->endpoint->number;
}

int lbs_display_content(const lbs_t *sim, int field)
{
    const lb_display_t *d = s_field(sim, field);
    return d == NULL ? -1 : lb_display_content(d);
}

/** `Quantity` of a VALUE field, -1 for a field without one. */
int lbs_display_quantity(const lbs_t *sim, int field)
{
    const lb_display_t *d = s_field(sim, field);
    if (d == NULL) {
        return -1;
    }
    for (uint8_t a = 0; a < d->endpoint->attr_count; a++) {
        const lb_attr_t *attr = &d->endpoint->attrs[a];
        if (attr->id == LB_DSP_QUANTITY && !attr->value.absent) {
            return attr->value.value;
        }
    }
    return -1;
}

/** True with the number in `value`; false when the field has no value to show. */
bool lbs_display_value(const lbs_t *sim, int field, int32_t *value)
{
    const lb_display_t *d = s_field(sim, field);
    if (d == NULL) {
        return false;
    }
    lb_value_t v = lb_display_value(d);
    *value = v.value;
    return !v.absent;
}

int lbs_display_text(const lbs_t *sim, int field, char *out, int cap)
{
    const lb_display_t *d = s_field(sim, field);
    if (d == NULL || cap < 0) {
        return -1;
    }
    uint8_t len = 0;
    const char *text = lb_display_text(d, &len);
    int n = len < cap ? len : cap;
    memcpy(out, text, (size_t)n);
    return n;
}

uint32_t lbs_display_drawn(const lbs_t *sim) { return sim->drawn; }

int lbs_sense_count(const lbs_t *sim) { return sim->sense_count; }

static lb_sense_t *s_sensor(lbs_t *sim, int sensor)
{
    return sensor >= 0 && sensor < sim->sense_count ? &sim->senses[sensor] : NULL;
}

int lbs_sense_endpoint(const lbs_t *sim, int sensor)
{
    const lb_sense_t *s = s_sensor((lbs_t *)sim, sensor);
    return s == NULL ? -1 : s->endpoint->number;
}

int lbs_sense_quantity(const lbs_t *sim, int sensor)
{
    const lb_sense_t *s = s_sensor((lbs_t *)sim, sensor);
    return s == NULL ? -1 : lb_sense_quantity(s);
}

bool lbs_sense_value(const lbs_t *sim, int sensor, int32_t *value)
{
    const lb_sense_t *s = s_sensor((lbs_t *)sim, sensor);
    if (s == NULL) {
        return false;
    }
    lb_value_t v = lb_sense_value(s);
    *value = v.value;
    return !v.absent;
}

void lbs_sense_update(lbs_t *sim, int sensor, int32_t raw)
{
    lb_sense_t *s = s_sensor(sim, sensor);
    if (s != NULL) {
        lb_sense_update(s, raw);
    }
}

void lbs_sense_fail(lbs_t *sim, int sensor)
{
    lb_sense_t *s = s_sensor(sim, sensor);
    if (s != NULL) {
        lb_sense_fail(s);
    }
}

void lbs_service_press(lbs_t *sim) { lb_node_factory_reset(&sim->node); }

int lbs_state(const lbs_t *sim) { return sim->state; }

bool lbs_led(const lbs_t *sim) { return sim->led_lit; }

uint32_t lbs_state_reports(const lbs_t *sim) { return sim->state_reports; }

const char *lbs_state_name(int state)
{
    return state < 0 ? NULL : lb_node_state_name((lb_node_state_t)state);
}

void lbs_set_bus(lbs_t *sim, uint8_t state)
{
    /* The health hook goes in on first use, so a device that never calls this keeps the
     * port it always had — Diagnostics answering UNAVAILABLE (system.json pins that). */
    sim->bus.health = s_health;
    sim->has_health = true;
    sim->bus_state = state;
}

void lbs_set_errors(lbs_t *sim, uint16_t tec, uint16_t rec)
{
    sim->bus.health = s_health;
    sim->has_health = true;
    sim->tec = tec;
    sim->rec = rec;
}

void lbs_set_reset_reason(lbs_t *sim, uint8_t reason) { sim->reset_reason = reason; }

uint16_t lbs_address(const lbs_t *sim) { return sim->node.address; }

uint32_t lbs_short_id(const lbs_t *sim) { return lb_node_short_id(&sim->node); }
