#include "lagunbus/types/output.h"

#include "lagunbus/registry.h"

#include <string.h>

/* The key in persistent storage — renamed from the Polish "wyjscie", see node.c. */
#define NVS_KEY_OUTPUT "output"

static int32_t setting(lb_endpoint_t *ep, uint8_t element, int32_t fallback)
{
    lb_attr_t *a = lb_endpoint_attr(ep, element);
    return (a == NULL || a->value.absent) ? fallback : a->value.value;
}

static bool is_on(const lb_output_t *out)
{
    return setting(out->endpoint, LB_OUT_ON_OFF, 0) != 0;
}

/** A report with a rate limit (`ReportMinGap`, common attribute 0xF9).
 *
 * An output switched rapidly — by a held-down key, say — would flood the bus with
 * reports. `MinGap` does not lose the final state: a held-back report goes out once
 * the gap has passed, and it then carries the CURRENT state, not the one from a
 * moment ago.
 */
static void report_attr(lb_output_t *out, uint32_t now_ms)
{
    uint32_t min_gap = (uint32_t)setting(out->endpoint, LB_COMMON_REPORTMINGAP, 0);
    if (min_gap > 0 && out->last_report_ms != 0
        && now_ms - out->last_report_ms < min_gap) {
        out->report_pending = true;
        return;
    }
    uint8_t v[1];
    lb_encode(LB_T_BOOL, LB_NUM(is_on(out) ? 1 : 0), v, 1);
    lb_node_report(out->node, out->endpoint->number, LB_OUT_ON_OFF, v, 1);
    out->last_report_ms = now_ms == 0 ? 1 : now_ms;
    out->report_pending = false;
}

/** Store the state, but only when `PowerOnBehaviour` is `RESTORE`.
 *
 * Without that condition every switch would grind the persistent store — and a relay
 * in a hallway switches a dozen-odd times a day for twenty years.
 */
static void remember(lb_output_t *out)
{
    if (setting(out->endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF)
        != LB_POWER_ON_RESTORE) {
        return;
    }
    lb_platform_t *pl = out->node->platform;
    if (pl->store != NULL) {
        uint8_t b = is_on(out) ? 1u : 0u;
        pl->store(pl->ctx, NVS_KEY_OUTPUT, &b, sizeof b);
    }
}

void lb_output_set(lb_output_t *out, bool fresh)
{
    lb_attr_t *on_off = lb_endpoint_attr(out->endpoint, LB_OUT_ON_OFF);
    if (on_off == NULL || (on_off->value.value != 0) == fresh) {
        return; /* nothing changed: nothing to report and nothing to grind */
    }
    on_off->value = LB_NUM(fresh ? 1 : 0);

    bool invert = setting(out->endpoint, LB_OUT_INVERT, 0) != 0;
    if (out->drive != NULL) {
        out->drive(out->ctx, invert ? !fresh : fresh);
    }
    out->lit_ms = out->node->platform->millis(out->node->platform->ctx);
    remember(out);
    report_attr(out, out->lit_ms);
}

void lb_output_init(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                    lb_output_drive_fn drive, void *ctx)
{
    memset(out, 0, sizeof *out);
    out->node = node;
    out->endpoint = endpoint;
    out->drive = drive;
    out->ctx = ctx;

    int32_t behaviour = setting(endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
    bool target = behaviour == LB_POWER_ON_ON;
    if (behaviour == LB_POWER_ON_RESTORE && node->platform->load != NULL) {
        uint8_t b = 0;
        if (node->platform->load(node->platform->ctx, NVS_KEY_OUTPUT, &b, sizeof b)) {
            target = b != 0;
        }
    }

    /* We ALWAYS drive the pin, even when the state does not differ from the
     * attribute's initial value: after a restart the hardware is in an undefined
     * state and has to be brought to whatever the node claims about itself. */
    lb_attr_t *on_off = lb_endpoint_attr(endpoint, LB_OUT_ON_OFF);
    if (on_off != NULL) {
        on_off->value = LB_NUM(target ? 1 : 0);
    }
    bool invert = setting(endpoint, LB_OUT_INVERT, 0) != 0;
    if (drive != NULL) {
        drive(ctx, invert ? !target : target);
    }
    out->lit_ms = node->platform->millis(node->platform->ctx);
}

uint8_t lb_output_command(lb_output_t *out, uint8_t element)
{
    switch (element) {
    case LB_OUT_CMD_OFF:
        lb_output_set(out, false);
        return LB_RESULT_OK;
    case LB_OUT_CMD_ON:
        lb_output_set(out, true);
        return LB_RESULT_OK;
    case LB_OUT_CMD_TOGGLE:
        lb_output_set(out, !is_on(out));
        return LB_RESULT_OK;
    default:
        /* The remaining `Output` commands belong to features this output does not
         * have: SetLevel without LEVEL, SetColor without COLOR_RGB. We do not pretend
         * we can. */
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
}

void lb_output_tick(lb_output_t *out, uint32_t now_ms)
{
    uint32_t auto_off = (uint32_t)setting(out->endpoint, LB_OUT_AUTO_OFF_TIME, 0);
    if (auto_off > 0 && is_on(out) && now_ms - out->lit_ms >= auto_off * 1000u) {
        lb_output_set(out, false);
    }
    if (out->report_pending) {
        report_attr(out, now_ms);
    }
}
