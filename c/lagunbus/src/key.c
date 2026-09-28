#include "lagunbus/key.h"

#include <string.h>

/** A setting from an attribute, or a fallback when the attribute is not there.
 *
 * An endpoint need not carry every attribute — `Invert` and `PressCount` are
 * optional. A missing attribute must not upset the key, only mean "use the default".
 */
static int32_t setting(lb_endpoint_t *ep, uint8_t element, int32_t fallback)
{
    lb_attr_t *a = lb_endpoint_attr(ep, element);
    return (a == NULL || a->value.absent) ? fallback : a->value.value;
}

const char *lb_key_event_name(uint8_t event)
{
    switch (event) {
    case LB_KEY_EV_DOWN:       return "Down";
    case LB_KEY_EV_UP:         return "Up";
    case LB_KEY_EV_CLICK:      return "Click";
    case LB_KEY_EV_HOLD_START: return "HoldStart";
    case LB_KEY_EV_HOLD_TICK:  return "HoldTick";
    case LB_KEY_EV_HOLD_END:   return "HoldEnd";
    default:                   return "?";
    }
}

void lb_key_init(lb_key_t *key, lb_endpoint_t *endpoint, bool level, uint32_t now_ms)
{
    memset(key, 0, sizeof *key);
    key->endpoint = endpoint;

    bool invert = setting(endpoint, LB_KEY_INVERT, 0) != 0;
    key->stable = invert ? !level : level;
    key->last_raw = key->stable;
    key->changed_ms = now_ms;
}

static void emit(lb_key_t *key, lb_node_t *node, uint8_t ev,
                      const uint8_t *params, uint8_t len)
{
    lb_node_emit(node, key->endpoint->number, ev, params, len);
}

static void emit_u16(lb_key_t *key, lb_node_t *node, uint8_t ev, uint32_t value)
{
    /* Durations travel as u16, so anything longer than ~65 s would be truncated
     * silently. Saturating is more honest: "a very long time" instead of a random
     * small number. */
    uint8_t p[2];
    lb_encode(LB_T_U16, LB_NUM(value > 0xFFFFu ? 0xFFFF : (int32_t)value), p, 2);
    emit(key, node, ev, p, 2);
}

/** Clicks are only settled once the multi-click window has passed. */
static void flush_clicks(lb_key_t *key, lb_node_t *node)
{
    if (!key->window_open || key->clicks == 0) {
        return;
    }
    uint8_t p[1] = {key->clicks};
    emit(key, node, LB_KEY_EV_CLICK, p, 1);
    key->clicks = 0;
    key->window_open = false;
}

static void count_press(lb_key_t *key)
{
    lb_attr_t *counter = lb_endpoint_attr(key->endpoint, LB_KEY_PRESS_COUNT);
    if (counter != NULL && !counter->value.absent) {
        counter->value.value++;
    }
}

void lb_key_update(lb_key_t *key, lb_node_t *node, bool level, uint32_t now_ms)
{
    bool invert = setting(key->endpoint, LB_KEY_INVERT, 0) != 0;
    bool input_ep = invert ? !level : level;

    uint32_t debounce = (uint32_t)setting(key->endpoint, LB_KEY_DEBOUNCE_TIME, 15);
    uint8_t mode = (uint8_t)setting(key->endpoint, LB_KEY_EVENT_MODE, LB_KEY_MODE_CLICK);
    uint32_t window = (uint32_t)setting(key->endpoint, LB_KEY_MULTI_CLICK_WINDOW, 350);
    uint8_t max_clicks = (uint8_t)setting(key->endpoint, LB_KEY_MAX_CLICKS, 3);
    bool hold_allowed = setting(key->endpoint, LB_KEY_HOLD_ENABLED, 1) != 0;
    uint32_t hold_threshold = (uint32_t)setting(key->endpoint, LB_KEY_HOLD_THRESHOLD, 500);
    uint32_t tick = (uint32_t)setting(key->endpoint, LB_KEY_HOLD_TICK_INTERVAL, 200);

    /* --- contact bounce -------------------------------------------------------
     * A change has to hold for `DebounceTime` before it is accepted. Bounce on
     * contact lasts single milliseconds and disappears entirely inside that window. */
    if (input_ep != key->last_raw) {
        key->last_raw = input_ep;
        key->changed_ms = now_ms;
    }
    bool changed = false;
    if (key->last_raw != key->stable && now_ms - key->changed_ms >= debounce) {
        key->stable = key->last_raw;
        changed = true;
    }

    /* --- edges --------------------------------------------------------------- */
    if (changed && key->stable) {
        key->pressed_ms = now_ms;
        key->hold_active = false;
        count_press(key);
        emit(key, node, LB_KEY_EV_DOWN, NULL, 0);
    } else if (changed && !key->stable) {
        uint32_t elapsed = now_ms - key->pressed_ms;
        emit_u16(key, node, LB_KEY_EV_UP, elapsed);

        if (key->hold_active) {
            /* A hold is NOT a click. If it were, every long press would produce both
             * HoldEnd and Click — two contradictory messages about one event. */
            emit_u16(key, node, LB_KEY_EV_HOLD_END, elapsed);
            key->hold_active = false;
            key->clicks = 0;
            key->window_open = false;
        } else if (mode == LB_KEY_MODE_CLICK) {
            uint8_t p[1] = {1};
            emit(key, node, LB_KEY_EV_CLICK, p, 1);
        } else if (mode == LB_KEY_MODE_MULTICLICK) {
            if (key->clicks < 0xFFu) {
                key->clicks++;
            }
            key->window_open = true;
            key->window_until_ms = now_ms + window;
            if (key->clicks >= max_clicks) {
                flush_clicks(key, node); /* full house — nothing left to wait for */
            }
        }
        /* LB_KEY_MODE_RAW: Down and Up are enough, the rest is the hub’s business. */
    }

    /* --- hold -------------------------------------------------------- */
    if (key->stable && hold_allowed && mode != LB_KEY_MODE_RAW) {
        uint32_t active = now_ms - key->pressed_ms;
        if (!key->hold_active && active >= hold_threshold) {
            key->hold_active = true;
            key->last_tick_ms = now_ms;
            emit(key, node, LB_KEY_EV_HOLD_START, NULL, 0);
        } else if (key->hold_active && tick > 0 && now_ms - key->last_tick_ms >= tick) {
            key->last_tick_ms = now_ms;
            emit_u16(key, node, LB_KEY_EV_HOLD_TICK, active);
        }
    }

    /* --- the multi-click window ------------------------------------------------------ */
    if (key->window_open && !key->stable && now_ms >= key->window_until_ms) {
        flush_clicks(key, node);
    }
}
