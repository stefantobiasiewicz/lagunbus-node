#include "lagunbus/types/sense.h"

#include "lagunbus/registry.h"

/** `ReportHysteresis` by quantity, in its units on the wire (types/0x0030-sense.md §5.3).
 * Copied by hand from `default_hysteresis` in lagunbus-spec/registry/quantities.yaml, as
 * the wire types in `values.h` are — the generator does not carry them. For `ILLUMINANCE`
 * it is the floor of a relative rule, 50 lx (`light_hysteresis` below; DEVICE-01 N2). The
 * quantities whose default is "1 % of the range" get 0 — any change — until a sensor of
 * theirs exists. */
static uint16_t default_hysteresis(uint8_t quantity)
{
    switch (quantity) {
    case LB_QUANTITY_TEMPERATURE: return 20;   /* 0.20 °C at 0.01 °C */
    case LB_QUANTITY_HUMIDITY:    return 100;  /* 1.00 % at 0.01 % */
    case LB_QUANTITY_ILLUMINANCE: return 5000; /* 50 lx at 0.01 lx */
    case LB_QUANTITY_CO2:         return 30;   /* 30 ppm */
    case LB_QUANTITY_PRESSURE:    return 50;   /* 0.5 hPa at 0.01 hPa */
    case LB_QUANTITY_POWER:       return 50;   /* 5 W at 0.1 W */
    default:                      return 0;
    }
}

/** `ReportInterval` by quantity, in seconds (§5.3; `default_interval_s` in the registry). */
static uint16_t default_interval(uint8_t quantity)
{
    switch (quantity) {
    case LB_QUANTITY_PRESSURE: return 300;
    case LB_QUANTITY_POWER:    return 30;
    default:                   return 60;
    }
}

static uint8_t quantity_of(const lb_endpoint_t *endpoint)
{
    for (uint8_t i = 0; i < endpoint->attr_count; i++) {
        const lb_attr_t *a = &endpoint->attrs[i];
        if (a->id == LB_SNS_QUANTITY) {
            return a->value.absent ? 0xFFu : (uint8_t)a->value.value;
        }
    }
    return 0xFFu;
}

static bool is_quantity_typed(uint8_t element)
{
    switch (element) {
    case LB_SNS_VALUE:
    case LB_SNS_MIN_MEASURED:
    case LB_SNS_MAX_MEASURED:
    case LB_SNS_THRESHOLD_LOW:
    case LB_SNS_THRESHOLD_HIGH:
        return true;
    default:
        return false;
    }
}

void lb_sense_defaults(lb_endpoint_t *endpoint, uint8_t quantity)
{
    endpoint->common.report_hysteresis = default_hysteresis(quantity);
    endpoint->common.report_interval = default_interval(quantity);
    lb_type_t type = lb_quantity_type(quantity);
    for (uint8_t i = 0; i < endpoint->attr_count; i++) {
        lb_attr_t *a = &endpoint->attrs[i];
        if (is_quantity_typed(a->id)) {
            a->type = type;
        } else if (a->id == LB_SNS_THRESHOLD_HYSTERESIS && a->value.absent) {
            a->value = LB_NUM(default_hysteresis(quantity));
        }
    }
}

/** A value as the number it means: `u32` travels in an `int32_t` and would turn negative
 * above 2^31 — the same care as the engine's `numeric` (endpoint.c). */
static int64_t number(lb_type_t type, lb_value_t value)
{
    return type == LB_T_U32 ? (int64_t)(uint32_t)value.value : (int64_t)value.value;
}

/** §5.3 for light: `ReportHysteresis` is the floor, 10 % of the last reported value above
 * it, and 0 is still "any change" — 01-model-danych.md §6.2 has no exception for one
 * quantity (lagunbus-spec/DECYZJE.md, "Sense: histereza światła względna"). */
static uint32_t light_hysteresis(const lb_endpoint_t *self, uint8_t element,
                                 lb_value_t reported)
{
    (void)element;
    uint32_t floor = self->common.report_hysteresis;
    if (floor == 0 || reported.absent) {
        return floor;
    }
    uint32_t tenth = (uint32_t)reported.value / 10u;
    return tenth > floor ? tenth : floor;
}

uint16_t lb_sense_feature_of(uint8_t element)
{
    switch (element) {
    case LB_SNS_MIN_MEASURED:
    case LB_SNS_MAX_MEASURED:
        return LB_SNS_F_MIN_MAX;
    case LB_SNS_THRESHOLD_LOW:
    case LB_SNS_THRESHOLD_HIGH:
    case LB_SNS_THRESHOLD_HYSTERESIS:
        return LB_SNS_F_THRESHOLD;
    default:
        return 0;
    }
}

/** A threshold or a hysteresis is a setting, and "absent" is meaningful only for the
 * thresholds (inactive, §5.4) — a hysteresis of "I don't know" is no hysteresis. */
static uint8_t check(const lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    if (element == LB_SNS_THRESHOLD_HYSTERESIS && value.absent) {
        return LB_RESULT_INVALID_VALUE;
    }
    if (element == LB_SNS_OFFSET && value.absent) {
        return LB_RESULT_INVALID_VALUE;
    }
    return LB_RESULT_OK;
}

static uint32_t now_ms(const lb_sense_t *sense)
{
    const lb_platform_t *pl = sense->node->platform;
    return pl->millis(pl->ctx);
}

static void store_extremes(lb_sense_t *sense)
{
    lb_node_persist(sense->node, sense->endpoint, LB_SNS_MIN_MEASURED);
    lb_node_persist(sense->node, sense->endpoint, LB_SNS_MAX_MEASURED);
    sense->extremes_dirty = false;
    sense->extremes_stored_ms = now_ms(sense);
}

/** At most every ten minutes, and only when something changed (§5.7, DEVICE-01 N10):
 * after a reset every reading of a warming morning is a new maximum, and flash wears. */
static void maybe_store_extremes(lb_sense_t *sense)
{
    if (sense->extremes_dirty
        && now_ms(sense) - sense->extremes_stored_ms >= LB_SNS_MIN_MAX_STORE_MS) {
        store_extremes(sense);
    }
}

static uint8_t command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                       uint8_t len)
{
    (void)args;
    (void)len;
    lb_sense_t *sense = self->ctx;
    if (element != LB_SNS_CMD_RESET_MIN_MAX) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!(self->features & LB_SNS_F_MIN_MAX)) {
        return LB_RESULT_UNSUPPORTED_FEATURE;
    }
    /* The current `Value`, not absent: zero readings have no extremes, one has (§3). Stored
     * at once — a reset that a power cut undoes looks broken (§5.7). */
    lb_value_t now = lb_sense_value(sense);
    lb_endpoint_set(self, LB_SNS_MIN_MEASURED, now);
    lb_endpoint_set(self, LB_SNS_MAX_MEASURED, now);
    store_extremes(sense);
    return LB_RESULT_OK;
}

void lb_sense_init(lb_sense_t *sense, lb_node_t *node, lb_endpoint_t *endpoint)
{
    sense->node = node;
    sense->endpoint = endpoint;
    sense->zoned = false;
    sense->zone = LB_SNS_ZONE_NORMAL;
    sense->extremes_dirty = false;
    sense->extremes_stored_ms = now_ms(sense);
    endpoint->ctx = sense;
    endpoint->on_command = command;
    endpoint->on_check = check;
    endpoint->feature_of = lb_sense_feature_of;
    if (lb_sense_quantity(sense) == LB_QUANTITY_ILLUMINANCE) {
        endpoint->hysteresis = light_hysteresis;
    }
    lb_attr_t *value = lb_endpoint_attr(endpoint, LB_SNS_VALUE);
    if (value == NULL) {
        return;
    }
    /* Reported with hysteresis — the type's contract (§5.3), so the type marks it, not
     * the device. A 12-bit converter flickers in its last bit without pause; without
     * hysteresis one sensor would send a dozen times a second, measuring nothing new. */
    value->access |= LB_REPORTED | LB_HYST;
    value->type = lb_quantity_type(lb_sense_quantity(sense));
    /* Set directly: "absent" until the first measurement is not a change, and nothing is
     * reported before it (§5.5). */
    value->value = LB_ABSENT;
}

/** The attribute's value, or absent when the device does not list it. */
static lb_value_t attr_value(lb_endpoint_t *endpoint, uint8_t element)
{
    const lb_attr_t *a = lb_endpoint_attr(endpoint, element);
    return a == NULL ? LB_ABSENT : a->value;
}

/** §5.2: failed readings are no extremes — only a good value comes here. */
static void track_extremes(lb_sense_t *sense, lb_type_t type, lb_value_t value)
{
    lb_endpoint_t *ep = sense->endpoint;
    if (!(ep->features & LB_SNS_F_MIN_MAX)) {
        return;
    }
    lb_value_t lo = attr_value(ep, LB_SNS_MIN_MEASURED);
    lb_value_t hi = attr_value(ep, LB_SNS_MAX_MEASURED);
    if (lo.absent || number(type, value) < number(type, lo)) {
        lb_endpoint_set(ep, LB_SNS_MIN_MEASURED, value);
        sense->extremes_dirty = true;
    }
    if (hi.absent || number(type, value) > number(type, hi)) {
        lb_endpoint_set(ep, LB_SNS_MAX_MEASURED, value);
        sense->extremes_dirty = true;
    }
}

/** The zone a good value puts the sensor in, from the zone it was in (§5.4). Into a zone
 * past the threshold itself — at the threshold is not above it; out of one only past the
 * threshold minus the hysteresis, at it included. A threshold left absent is inactive,
 * and a zone whose threshold went absent is left at the next reading. */
static uint8_t zone_of(lb_sense_t *sense, lb_type_t type, lb_value_t value)
{
    lb_endpoint_t *ep = sense->endpoint;
    lb_value_t low = attr_value(ep, LB_SNS_THRESHOLD_LOW);
    lb_value_t high = attr_value(ep, LB_SNS_THRESHOLD_HIGH);
    lb_value_t hyst_attr = attr_value(ep, LB_SNS_THRESHOLD_HYSTERESIS);
    int64_t hyst = hyst_attr.absent ? (int64_t)default_hysteresis(lb_sense_quantity(sense))
                                    : (int64_t)hyst_attr.value;
    int64_t v = number(type, value);

    if (sense->zoned && sense->zone == LB_SNS_ZONE_ABOVE_HIGH && !high.absent
        && v > number(type, high) - hyst) {
        return LB_SNS_ZONE_ABOVE_HIGH;
    }
    if (sense->zoned && sense->zone == LB_SNS_ZONE_BELOW_LOW && !low.absent
        && v < number(type, low) + hyst) {
        return LB_SNS_ZONE_BELOW_LOW;
    }
    if (!high.absent && v > number(type, high)) {
        return LB_SNS_ZONE_ABOVE_HIGH;
    }
    if (!low.absent && v < number(type, low)) {
        return LB_SNS_ZONE_BELOW_LOW;
    }
    return LB_SNS_ZONE_NORMAL;
}

/** `Threshold` at a crossing, and only there. The first good reading sets the zone
 * without one: nothing was crossed, and an event at every start of the node would be
 * the false alarm §5.5 forbids. A failed reading crosses nothing — the zone waits for
 * the next good one. */
static void track_zone(lb_sense_t *sense, lb_type_t type, lb_value_t value)
{
    if (!(sense->endpoint->features & LB_SNS_F_THRESHOLD)) {
        return;
    }
    uint8_t zone = zone_of(sense, type, value);
    bool crossed = sense->zoned && zone != sense->zone;
    sense->zone = zone;
    sense->zoned = true;
    if (!crossed) {
        return;
    }
    uint8_t params[1 + 4];
    params[0] = zone;
    uint8_t len = lb_encode(type, value, params + 1, sizeof params - 1u);
    lb_node_emit(sense->node, sense->endpoint->number, LB_SNS_EVENT_THRESHOLD, params,
                 (uint8_t)(1u + len));
}

/** `Value` after `Offset`, held to what the type can carry: a calibration that pushes a
 * reading past the range says the reading is at the edge, not that it is unknown. */
static lb_value_t calibrated(lb_sense_t *sense, lb_type_t type, int32_t raw)
{
    lb_value_t offset = attr_value(sense->endpoint, LB_SNS_OFFSET);
    int64_t v = (int64_t)raw + (offset.absent ? 0 : offset.value);
    int64_t lo = lb_lo(type);
    int64_t hi = lb_hi(type);
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return LB_NUM((int32_t)v);
}

void lb_sense_update(lb_sense_t *sense, int32_t raw)
{
    lb_type_t type = lb_quantity_type(lb_sense_quantity(sense));
    lb_value_t value = calibrated(sense, type, raw);
    track_extremes(sense, type, value);
    /* The event before the report of the same reading: it is the one somebody acts on
     * (§5.4 — "a fact needing a reaction, not another point on a chart"). */
    track_zone(sense, type, value);
    lb_endpoint_set(sense->endpoint, LB_SNS_VALUE, value);
    lb_node_flush(sense->node, sense->endpoint);
    maybe_store_extremes(sense);
}

void lb_sense_fail(lb_sense_t *sense)
{
    lb_endpoint_set(sense->endpoint, LB_SNS_VALUE, LB_ABSENT);
    lb_node_flush(sense->node, sense->endpoint);
    maybe_store_extremes(sense);
}

lb_value_t lb_sense_value(const lb_sense_t *sense)
{
    const lb_attr_t *value = lb_endpoint_attr(sense->endpoint, LB_SNS_VALUE);
    return value == NULL ? LB_ABSENT : value->value;
}

uint8_t lb_sense_quantity(const lb_sense_t *sense)
{
    return quantity_of(sense->endpoint);
}
