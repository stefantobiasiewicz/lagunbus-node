/* When an e-paper screen refreshes — the rules are in tools/epaper.h. */
#include "lagunbus/tools/epaper.h"

void lb_epaper_init(lb_epaper_t *pacer, uint32_t min_gap_ms, uint8_t full_every)
{
    *pacer = (lb_epaper_t){.min_gap_ms = min_gap_ms, .full_every = full_every};
}

lb_epaper_kind_t lb_epaper_due(const lb_epaper_t *pacer, uint32_t now_ms, bool differs)
{
    if (!pacer->started) {
        return LB_EPAPER_FULL;
    }
    if (!differs) {
        return LB_EPAPER_NONE;
    }
    /* Unsigned difference: right across the wrap of the millisecond clock (49 days). */
    if (pacer->content && now_ms - pacer->last_ms < pacer->min_gap_ms) {
        return LB_EPAPER_NONE;
    }
    return pacer->partials >= pacer->full_every ? LB_EPAPER_FULL : LB_EPAPER_PARTIAL;
}

void lb_epaper_started(lb_epaper_t *pacer, uint32_t now_ms, lb_epaper_kind_t kind,
                       bool content)
{
    if (kind == LB_EPAPER_NONE) {
        return;
    }
    pacer->started = true;
    pacer->content = content;
    pacer->last_ms = now_ms;
    if (kind == LB_EPAPER_FULL) {
        pacer->partials = 0;
    } else if (pacer->partials < UINT8_MAX) {
        pacer->partials++;
    }
}
