#include "lagunbus/tools/service.h"

void lb_service_init(lb_service_t *s, uint32_t hold_ms)
{
    *s = (lb_service_t){.hold_ms = hold_ms};
}

lb_service_event_t lb_service_update(lb_service_t *s, bool pressed, uint32_t now_ms)
{
    if (!s->armed) {
        /* Held since start — or stuck. Only a release proves a finger was on it. */
        s->armed = !pressed;
        return LB_SERVICE_IDLE;
    }
    if (pressed && !s->pressed) {
        s->pressed = true;
        s->pressed_ms = now_ms;
        return LB_SERVICE_IDLE;
    }
    /* Modulo 2^32, as every deadline in the library: millis() wraps after 49 days. */
    bool long_enough = now_ms - s->pressed_ms >= s->hold_ms;
    if (pressed) {
        return long_enough ? LB_SERVICE_HELD : LB_SERVICE_IDLE;
    }
    if (s->pressed) {
        s->pressed = false;
        return long_enough ? LB_SERVICE_RESET : LB_SERVICE_IDLE;
    }
    return LB_SERVICE_IDLE;
}
