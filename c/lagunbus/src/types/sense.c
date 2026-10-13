#include "lagunbus/types/sense.h"

#include "lagunbus/registry.h"

/** `ReportHysteresis` by quantity, in its units on the wire (types/0x0030-sense.md §5.3).
 * Copied by hand from `default_hysteresis` in lagunbus-spec/registry/quantities.yaml, as
 * the wire types in `values.h` are — the generator does not carry them. `ILLUMINANCE` is
 * left out on purpose: its rule is relative ("50 lx or 10 %"), which one `u16` cannot
 * hold — DEVICE-01 N2 decides how. It and the quantities whose default is "1 % of the
 * range" get 0 — any change — until a sensor of theirs exists. */
static uint16_t default_hysteresis(uint8_t quantity)
{
    switch (quantity) {
    case LB_QUANTITY_TEMPERATURE: return 20;  /* 0.20 °C at 0.01 °C */
    case LB_QUANTITY_HUMIDITY:    return 100; /* 1.00 % at 0.01 % */
    case LB_QUANTITY_CO2:         return 30;  /* 30 ppm */
    case LB_QUANTITY_PRESSURE:    return 50;  /* 0.5 hPa at 0.01 hPa */
    case LB_QUANTITY_POWER:       return 50;  /* 5 W at 0.1 W */
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

void lb_sense_defaults(lb_endpoint_t *endpoint, uint8_t quantity)
{
    endpoint->common.report_hysteresis = default_hysteresis(quantity);
    endpoint->common.report_interval = default_interval(quantity);
}

void lb_sense_init(lb_sense_t *sense, lb_node_t *node, lb_endpoint_t *endpoint)
{
    sense->node = node;
    sense->endpoint = endpoint;
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

static void measured(lb_sense_t *sense, lb_value_t value)
{
    lb_endpoint_set(sense->endpoint, LB_SNS_VALUE, value);
    lb_node_flush(sense->node, sense->endpoint);
}

void lb_sense_update(lb_sense_t *sense, int32_t raw)
{
    measured(sense, LB_NUM(raw));
}

void lb_sense_fail(lb_sense_t *sense)
{
    measured(sense, LB_ABSENT);
}

lb_value_t lb_sense_value(const lb_sense_t *sense)
{
    const lb_attr_t *value = lb_endpoint_attr(sense->endpoint, LB_SNS_VALUE);
    return value == NULL ? LB_ABSENT : value->value;
}

uint8_t lb_sense_quantity(const lb_sense_t *sense)
{
    const lb_attr_t *quantity = lb_endpoint_attr(sense->endpoint, LB_SNS_QUANTITY);
    return quantity == NULL || quantity->value.absent ? 0xFFu : (uint8_t)quantity->value.value;
}
