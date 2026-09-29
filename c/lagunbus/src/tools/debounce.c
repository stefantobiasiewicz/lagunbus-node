#include "lagunbus/tools/debounce.h"

void lb_debounce_init(lb_debounce_t *d, bool level, uint32_t now_ms)
{
    d->stable = level;
    d->last_raw = level;
    d->changed_ms = now_ms;
}

bool lb_debounce_update(lb_debounce_t *d, bool level, uint32_t now_ms, uint32_t window_ms)
{
    if (level != d->last_raw) {
        d->last_raw = level;
        d->changed_ms = now_ms;
    }
    if (d->last_raw != d->stable && now_ms - d->changed_ms >= window_ms) {
        d->stable = d->last_raw;
    }
    return d->stable;
}
