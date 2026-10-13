#include "lagunbus/types/key.h"

#include "lagunbus/registry.h"

#include <string.h>

/* --- endpoint hooks ---------------------------------------------------------------- */

static bool within(lb_value_t value, int32_t lo, int32_t hi)
{
    return value.value >= lo && value.value <= hi;
}

uint8_t lb_key_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    /* The ranges are the type's table (types/0x0010-key-input.md §2.2), repeated here
     * because the table is prose; vectors/scenarios/key_checks.json pins them. */
    switch (element) {
    case LB_KEY_EVENT_MODE:
        if (value.value == LB_KEY_MODE_CLICK) {
            return LB_RESULT_OK;
        }
        if (value.value == LB_KEY_MODE_MULTICLICK) {
            return (self->features & LB_KEY_F_MULTICLICK) ? LB_RESULT_OK
                                                           : LB_RESULT_INVALID_VALUE;
        }
        if (value.value == LB_KEY_MODE_RAW) {
            return (self->features & LB_KEY_F_RAW) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
        }
        return LB_RESULT_INVALID_VALUE;
    case LB_KEY_MULTI_CLICK_WINDOW:
        return within(value, 150, 800) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_KEY_MAX_CLICKS:
        return within(value, 2, 5) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_KEY_HOLD_THRESHOLD:
        return within(value, 200, 3000) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_KEY_HOLD_TICK_INTERVAL:
        return within(value, 50, 1000) ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    default:
        return LB_RESULT_OK;
    }
}

uint16_t lb_key_feature_of(uint8_t element)
{
    switch (element) {
    case LB_KEY_EVENT_MODE:
        return LB_KEY_F_MULTICLICK | LB_KEY_F_RAW;
    case LB_KEY_MULTI_CLICK_WINDOW:
    case LB_KEY_MAX_CLICKS:
        return LB_KEY_F_MULTICLICK;
    case LB_KEY_HOLD_ENABLED:
    case LB_KEY_HOLD_THRESHOLD:
    case LB_KEY_HOLD_TICK_INTERVAL:
        return LB_KEY_F_HOLD;
    default:
        return 0;
    }
}

/* --- events ------------------------------------------------------------------------ */

static bool emit_u16(lb_node_t *node, uint8_t endpoint, uint8_t event, uint32_t ms)
{
    uint8_t p[2];
    lb_encode(LB_T_U16, LB_NUM(ms > 0xFFFFu ? 0xFFFF : (int32_t)ms), p, 2);
    return lb_node_emit(node, endpoint, event, p, 2);
}

bool lb_key_emit_down(lb_node_t *node, uint8_t endpoint)
{
    return lb_node_emit(node, endpoint, LB_KEY_EV_DOWN, NULL, 0);
}

bool lb_key_emit_up(lb_node_t *node, uint8_t endpoint, uint32_t held_ms)
{
    return emit_u16(node, endpoint, LB_KEY_EV_UP, held_ms);
}

bool lb_key_emit_click(lb_node_t *node, uint8_t endpoint, uint8_t count)
{
    uint8_t p[1] = {count};
    return lb_node_emit(node, endpoint, LB_KEY_EV_CLICK, p, 1);
}

bool lb_key_emit_hold_start(lb_node_t *node, uint8_t endpoint)
{
    return lb_node_emit(node, endpoint, LB_KEY_EV_HOLD_START, NULL, 0);
}

bool lb_key_emit_hold_tick(lb_node_t *node, uint8_t endpoint, uint32_t held_ms)
{
    return emit_u16(node, endpoint, LB_KEY_EV_HOLD_TICK, held_ms);
}

bool lb_key_emit_hold_end(lb_node_t *node, uint8_t endpoint, uint32_t held_ms)
{
    return emit_u16(node, endpoint, LB_KEY_EV_HOLD_END, held_ms);
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

/* --- the state machine ------------------------------------------------------------- */

/** The attribute when the endpoint exposes it, the device's own value when not. */
static uint32_t setting(const lb_key_t *key, uint8_t element, uint32_t own)
{
    lb_attr_t *a = lb_endpoint_attr(key->endpoint, element);
    return (a == NULL || a->value.absent) ? own : (uint32_t)a->value.value;
}

void lb_key_init(lb_key_t *key, lb_endpoint_t *endpoint, const lb_key_config_t *config,
                 bool pressed, uint32_t now_ms)
{
    memset(key, 0, sizeof *key);
    key->endpoint = endpoint;
    key->config = config;
    key->pressed = pressed;
    key->pressed_ms = now_ms;
    key->unseen_press = pressed && config->silent_start_press;
}

static void flush_clicks(lb_key_t *key, lb_node_t *node)
{
    if (!key->window_open || key->clicks == 0) {
        return;
    }
    lb_key_emit_click(node, key->endpoint->number, key->clicks);
    key->clicks = 0;
    key->window_open = false;
}

/** `PressCount` — loaded by the node at start, stored every `LB_PRESS_COUNT_EVERY`
 * presses (types/0x0010-key-input.md §2.1). Until TASK-01 it lived in RAM only and
 * started from zero at every restart, which made a lifetime counter a since-boot one. */
static void count_press(lb_key_t *key, lb_node_t *node)
{
    lb_attr_t *counter = lb_endpoint_attr(key->endpoint, LB_KEY_PRESS_COUNT);
    if (counter == NULL || counter->value.absent) {
        return;
    }
    counter->value.value++;
    if ((uint32_t)counter->value.value % LB_PRESS_COUNT_EVERY == 0u) {
        lb_node_persist(node, key->endpoint, LB_KEY_PRESS_COUNT);
    }
}

void lb_key_update(lb_key_t *key, lb_node_t *node, bool pressed, uint32_t now_ms)
{
    const lb_key_config_t *cfg = key->config;
    uint16_t features = key->endpoint->features;
    uint8_t number = key->endpoint->number;

    /* A mode exists only with its feature; without MULTICLICK and RAW there is no
     * EventMode and the key is in CLICK for good (types/0x0010-key-input.md §1). */
    uint8_t mode = (uint8_t)setting(key, LB_KEY_EVENT_MODE, LB_KEY_MODE_CLICK);
    uint32_t window = setting(key, LB_KEY_MULTI_CLICK_WINDOW, cfg->multi_click_window_ms);
    uint32_t max_clicks = setting(key, LB_KEY_MAX_CLICKS, cfg->max_clicks);
    bool hold_allowed = (features & LB_KEY_F_HOLD) && setting(key, LB_KEY_HOLD_ENABLED, 1);
    uint32_t hold_threshold = setting(key, LB_KEY_HOLD_THRESHOLD, cfg->hold_threshold_ms);
    uint32_t tick = setting(key, LB_KEY_HOLD_TICK_INTERVAL, cfg->hold_tick_ms);

    bool pressed_now = pressed && !key->pressed;
    bool released_now = !pressed && key->pressed;
    key->pressed = pressed;

    /* --- edges ---------------------------------------------------------------
     * Down and Up belong to RAW alone. In CLICK and MULTICLICK a press says nothing
     * until it ends as a click or turns into a hold — otherwise every press would put
     * three frames on the bus for one fact (types/0x0010-key-input.md §5.5). */
    if (pressed_now) {
        key->pressed_ms = now_ms;
        key->hold_active = false;
        count_press(key, node);
        if (mode == LB_KEY_MODE_RAW) {
            lb_key_emit_down(node, number);
        }
    } else if (released_now) {
        uint32_t elapsed = now_ms - key->pressed_ms;

        if (key->unseen_press) {
            key->unseen_press = false; /* the end of a press never seen: nothing */
        } else if (key->hold_active) {
            /* A release is exactly ONE event. After a hold that is HoldEnd — not a
             * Click, which would be a second, contradictory message about the same
             * press, and not an Up, which would fire a rule on Up after a hold. */
            lb_key_emit_hold_end(node, number, elapsed);
            key->hold_active = false;
        } else if (mode == LB_KEY_MODE_RAW) {
            lb_key_emit_up(node, number, elapsed);
        } else if (mode == LB_KEY_MODE_CLICK) {
            lb_key_emit_click(node, number, 1);
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
    }

    /* --- hold ----------------------------------------------------------------
     * In every mode, RAW included, but only with the HOLD feature. A hold cancels a
     * multi-click sequence as it starts, not at the release: the clicks before it
     * must not surface later as a stray Click. */
    if (key->pressed && hold_allowed && !key->unseen_press) {
        uint32_t active = now_ms - key->pressed_ms;
        if (!key->hold_active && active >= hold_threshold) {
            key->hold_active = true;
            key->last_tick_ms = now_ms;
            key->clicks = 0;
            key->window_open = false;
            lb_key_emit_hold_start(node, number);
        } else if (key->hold_active && tick > 0 && active < LB_KEY_HOLD_TICK_LIMIT_MS
                   && now_ms - key->last_tick_ms >= tick) {
            /* Past the limit the key is taken to be stuck — jammed, flooded, pressed
             * by a piece of furniture — and it may not keep the bus busy. HoldEnd
             * still comes on release. */
            key->last_tick_ms = now_ms;
            lb_key_emit_hold_tick(node, number, active);
        }
    }

    /* --- the multi-click window ------------------------------------------------ */
    if (key->window_open && !key->pressed && now_ms >= key->window_until_ms) {
        flush_clicks(key, node);
    }
}
