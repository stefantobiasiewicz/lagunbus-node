#include "lagunbus/types/value_input.h"

#include "lagunbus/registry.h"

#include <string.h>

/* --- reading the endpoint ----------------------------------------------------------- */

static int32_t setting(const lb_endpoint_t *ep, uint8_t element, int32_t fallback)
{
    lb_attr_t *a = lb_endpoint_attr((lb_endpoint_t *)ep, element);
    return (a == NULL || a->value.absent) ? fallback : a->value.value;
}

static int32_t min_of(const lb_endpoint_t *ep) { return setting(ep, LB_VI_MIN_VALUE, 0); }

static int32_t max_of(const lb_endpoint_t *ep)
{
    return setting(ep, LB_VI_MAX_VALUE, (int32_t)LB_VI_FULL);
}

static uint32_t now_ms(const lb_value_input_t *vi)
{
    const lb_platform_t *pl = vi->node->platform;
    return pl->millis(pl->ctx);
}

/* --- endpoint hooks ---------------------------------------------------------------- */

/** The type's table (§2.2) and the rules between attributes — `MinValue ≤ MaxValue`,
 * `Value` within them (DEVICE-02 S1). Repeated here because the table is prose;
 * vectors/scenarios/value_input.json pins them. */
static uint8_t check(const lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    int32_t v = value.value;
    switch (element) {
    case LB_VI_VALUE:
        return v >= min_of(self) && v <= max_of(self) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_VI_MIN_VALUE:
        return v <= (int32_t)LB_VI_FULL && v <= max_of(self) ? LB_RESULT_OK
                                                              : LB_RESULT_INVALID_VALUE;
    case LB_VI_MAX_VALUE:
        return v <= (int32_t)LB_VI_FULL && v >= min_of(self) ? LB_RESULT_OK
                                                              : LB_RESULT_INVALID_VALUE;
    case LB_VI_STEP_SIZE:
        return v >= 1 && v <= (int32_t)LB_VI_FULL ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    default:
        return LB_RESULT_OK;
    }
}

/** A narrowed range clips `Value` rather than refusing the write (DEVICE-02 S1): a
 * refusal would make the hub write in an order — value first, range second — just to
 * narrow a knob. Not a person's change, so no `Changed` (§5.5); a change of state, so
 * reported at once — and stored at once, as the write that caused it is, because the
 * hub that got `OK` may cut the power the next moment. */
static uint8_t on_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)value;
    if (element != LB_VI_MIN_VALUE && element != LB_VI_MAX_VALUE) {
        return LB_RESULT_OK; /* a written `Value` is the node's to store; nothing to say */
    }
    lb_value_input_t *vi = self->ctx;
    int32_t now = setting(self, LB_VI_VALUE, 0);
    int32_t lo = min_of(self);
    int32_t hi = max_of(self);
    int32_t clipped = now < lo ? lo : now > hi ? hi : now;
    if (clipped != now) {
        lb_endpoint_set(self, LB_VI_VALUE, LB_NUM(clipped));
        lb_node_persist(vi->node, self, LB_VI_VALUE);
        lb_node_flush(vi->node, self);
    }
    return LB_RESULT_OK;
}

/* --- the type ----------------------------------------------------------------------- */

void lb_value_input_defaults(lb_endpoint_t *endpoint)
{
    endpoint->common.report_interval = LB_VI_REPORT_INTERVAL_S;
    endpoint->common.report_min_gap = LB_VI_REPORT_MIN_GAP_MS;
}

void lb_value_input_init(lb_value_input_t *vi, lb_node_t *node, lb_endpoint_t *endpoint,
                         uint16_t own_step)
{
    memset(vi, 0, sizeof *vi);
    vi->node = node;
    vi->endpoint = endpoint;
    vi->own_step = own_step;
    endpoint->ctx = vi;
    endpoint->on_check = check;
    endpoint->on_write = on_write;

    lb_attr_t *value = lb_endpoint_attr(endpoint, LB_VI_VALUE);
    if (value == NULL) {
        return;
    }
    /* Reported on every change, no hysteresis: limited only by `ReportMinGap` (§5.7). */
    value->access |= LB_REPORTED;
    /* Into the range, set directly: where the value starts is not a change, and nothing
     * is reported before `Booted`. A stored value outside a stored range cannot come
     * from this node, but a range a new firmware narrowed can — and after a factory
     * reset the row's 0 becomes `MinValue` here (§5.6). A range that is itself wrong
     * (`Min > Max`, a store from elsewhere) leaves `Value` at `MinValue`. */
    int32_t lo = min_of(endpoint);
    int32_t hi = max_of(endpoint);
    if (hi < lo) {
        hi = lo;
    }
    int32_t v = value->value.absent ? lo : value->value.value;
    value->value = LB_NUM(v < lo ? lo : v > hi ? hi : v);
}

/** One step from `v` in direction `up`, by `step`, within `lo`…`hi`. A step past a
 * limit reaches it; with `wrap`, only a step FROM a limit goes to the other one — else a
 * step that does not divide the range would never reach full brightness (S2). */
static int32_t one_step(int32_t v, bool up, int32_t step, int32_t lo, int32_t hi, bool wrap)
{
    if (up) {
        if (v >= hi) {
            return wrap ? lo : hi;
        }
        return v + step > hi ? hi : v + step;
    }
    if (v <= lo) {
        return wrap ? hi : lo;
    }
    return v - step < lo ? lo : v - step;
}

bool lb_value_input_step(lb_value_input_t *vi, int32_t steps)
{
    lb_endpoint_t *ep = vi->endpoint;
    lb_attr_t *value = lb_endpoint_attr(ep, LB_VI_VALUE);
    if (value == NULL || steps == 0 || ep->common.disabled) {
        return false;
    }
    int32_t lo = min_of(ep);
    int32_t hi = max_of(ep);
    int32_t step = setting(ep, LB_VI_STEP_SIZE, vi->own_step);
    bool wrap = setting(ep, LB_VI_WRAP, 0) != 0;
    bool up = steps > 0;
    /* More than the scale has steps cannot mean more at the limits, and with `Wrap` a
     * runaway count must not spin here for long: capped at one full scale. */
    uint32_t count = (uint32_t)(up ? steps : -steps);
    if (count > LB_VI_FULL) {
        count = LB_VI_FULL;
    }

    int32_t before = value->value.value;
    int32_t after = before;
    for (uint32_t i = 0; i < count; i++) {
        after = one_step(after, up, step, lo, hi, wrap);
    }
    if (after == before) {
        return false; /* at the limit: nothing to announce (§5.2) */
    }

    /* `delta` arithmetic, wrap included (S2): 1000 → 0 is −1000, so a target bound
     * through `Step` follows the value instead of saturating at the top. */
    int32_t delta = after - before;
    uint8_t p[4] = {
        (uint8_t)(after & 0xFF), (uint8_t)((after >> 8) & 0xFF),
        (uint8_t)((uint32_t)delta & 0xFFu), (uint8_t)(((uint32_t)delta >> 8) & 0xFFu),
    };
    lb_endpoint_set(ep, LB_VI_VALUE, LB_NUM(after));
    lb_attr_t *count_attr = lb_endpoint_attr(ep, LB_VI_CHANGE_COUNT);
    if (count_attr != NULL) {
        count_attr->value = LB_NUM((int32_t)((uint32_t)count_attr->value.value + 1u));
    }
    /* The event first: it is the fast path the hub acts on (§4.1). The report follows
     * under `ReportMinGap` — at once, or held back and sent with the value then. */
    lb_node_emit(vi->node, ep->number, LB_VI_EV_CHANGED, p, sizeof p);
    lb_node_flush(vi->node, ep);
    vi->dirty = true;
    vi->changed_ms = now_ms(vi);
    return true;
}

void lb_value_input_tick(lb_value_input_t *vi, uint32_t now)
{
    if (vi->dirty && now - vi->changed_ms >= LB_VI_STORE_AFTER_MS) {
        lb_node_persist(vi->node, vi->endpoint, LB_VI_VALUE);
        lb_node_persist(vi->node, vi->endpoint, LB_VI_CHANGE_COUNT);
        vi->dirty = false;
    }
}

uint16_t lb_value_input_value(const lb_value_input_t *vi)
{
    return (uint16_t)setting(vi->endpoint, LB_VI_VALUE, 0);
}

void lb_value_input_range(const lb_value_input_t *vi, uint16_t *min, uint16_t *max)
{
    *min = (uint16_t)min_of(vi->endpoint);
    *max = (uint16_t)max_of(vi->endpoint);
}
