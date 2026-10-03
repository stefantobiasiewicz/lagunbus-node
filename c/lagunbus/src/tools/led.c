#include "lagunbus/tools/led.h"

#include <stddef.h>

void lb_led_init(lb_led_t *led, lb_led_fn set, void *ctx)
{
    *led = (lb_led_t){.set = set, .ctx = ctx, .state = LB_STATE_NO_ADDRESS,
                      .fresh = true, .shown = -1};
}

void lb_led_init_rgb(lb_led_t *led, lb_led_rgb_fn set_rgb, void *ctx)
{
    *led = (lb_led_t){.set_rgb = set_rgb, .ctx = ctx, .state = LB_STATE_NO_ADDRESS,
                      .fresh = true, .shown = -1};
}

void lb_led_on_state(void *ctx, lb_node_state_t state)
{
    lb_led_t *led = ctx;
    /* No clock here — the node calls this from inside its tick and passes no time — so
     * the pattern is restarted by the next lb_led_tick. */
    led->state = state;
    led->fresh = true;
}

void lb_led_release(lb_led_t *led, bool release) { led->release = release; }

bool lb_led_lit(lb_node_state_t state, uint32_t elapsed_ms)
{
    switch (state) {
    case LB_STATE_READY:
        return elapsed_ms % 3000u < 2900u;
    case LB_STATE_NO_ADDRESS:
        return elapsed_ms % 1000u < 500u;
    case LB_STATE_BUS_FAULT: {
        uint32_t t = elapsed_ms % 1000u;
        return t < 100u || (t >= 200u && t < 300u);
    }
    case LB_STATE_IDENTIFY:
        return elapsed_ms % 200u < 100u;
    case LB_STATE_RESETTING:
        return false;
    }
    return false;
}

void lb_led_colour(lb_node_state_t state, uint8_t rgb[3])
{
    rgb[0] = rgb[1] = rgb[2] = 0;
    switch (state) {
    case LB_STATE_READY:      rgb[1] = 255; break;
    case LB_STATE_NO_ADDRESS: rgb[2] = 255; break;
    case LB_STATE_BUS_FAULT:  rgb[0] = 255; break;
    case LB_STATE_IDENTIFY:   rgb[0] = rgb[1] = rgb[2] = 255; break;
    case LB_STATE_RESETTING:  break;
    }
}

void lb_led_tick(lb_led_t *led, uint32_t now_ms)
{
    if (led->fresh) {
        led->fresh = false;
        led->since_ms = now_ms;
    }
    bool on = !led->release && lb_led_lit(led->state, now_ms - led->since_ms);

    /* What is shown as one number, so "changed" is one comparison for both kinds:
     * 0/1 for a single colour, the packed colour (0 when dark) for RGB. */
    uint8_t rgb[3] = {0, 0, 0};
    if (on && led->set_rgb != NULL) {
        lb_led_colour(led->state, rgb);
    }
    int32_t shown = led->set_rgb != NULL
                  ? (int32_t)(((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2])
                  : (on ? 1 : 0);
    if (shown == led->shown) {
        return;
    }
    led->shown = shown;
    if (led->set_rgb != NULL) {
        led->set_rgb(led->ctx, rgb[0], rgb[1], rgb[2]);
    } else if (led->set != NULL) {
        led->set(led->ctx, on);
    }
}
