/* The status LED — a tool, not a type (zadania/CR-05.md).
 *
 * Every device has one, and every device of the project is to blink the same way in the
 * same situation — so the patterns live here, once, and are tested on a computer. The
 * device gives only the function that drives its hardware: a pin for a single-colour LED,
 * three channels for an RGB one. Nothing here knows GPIO exists.
 *
 * Wiring, in the device:
 *
 *     lb_led_init(&led, set_pin, NULL);          // or lb_led_init_rgb(&led, set_rgb, …)
 *     node.on_state  = lb_led_on_state;
 *     node.state_ctx = &led;
 *     …
 *     lb_led_tick(&led, now_ms);                 // every turn of the loop
 *
 * `lb_led_on_state` is called from inside the node and must not block, so it only
 * remembers the state; the blinking is counted out by `lb_led_tick`, in the device's
 * loop. The pin is written on change only — an RGB LED on I²C would otherwise be written
 * every couple of milliseconds for nothing.
 *
 * The patterns (one period each, from the moment the state began):
 *
 *     READY       lit, dark for 100 ms every 3 s   green
 *     NO_ADDRESS  500 ms on, 500 ms off            blue
 *     BUS_FAULT   two 100 ms flashes every 1 s     red
 *     IDENTIFY    100 ms on, 100 ms off            white
 *     RESETTING   dark
 *
 * READY is lit rather than dark so that a dark LED means "dead"; the blip says the loop
 * still runs — an LED stuck on by hung firmware would not blink. RESETTING is dark, and
 * so is a service button held past its threshold (`lb_led_release`): "hold until the LED
 * goes out, then let go" — the reset itself then keeps it dark until the restart.
 */
#ifndef LAGUNBUS_LED_H
#define LAGUNBUS_LED_H

#include "lagunbus/node.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*lb_led_fn)(void *ctx, bool on);
typedef void (*lb_led_rgb_fn)(void *ctx, uint8_t r, uint8_t g, uint8_t b);

typedef struct {
    lb_led_fn       set;      /**< single colour — or NULL */
    lb_led_rgb_fn   set_rgb;  /**< RGB — or NULL */
    void           *ctx;
    lb_node_state_t state;
    bool            fresh;    /**< a new state: its pattern starts at the next tick */
    uint32_t        since_ms;
    bool            release;  /**< the service button is past its threshold */
    int32_t         shown;    /**< what was last written, -1 before anything */
} lb_led_t;

void lb_led_init(lb_led_t *led, lb_led_fn set, void *ctx);
void lb_led_init_rgb(lb_led_t *led, lb_led_rgb_fn set_rgb, void *ctx);

/** The node's state watch (`lb_state_fn`), with the `lb_led_t` as `ctx`. */
void lb_led_on_state(void *ctx, lb_node_state_t state);

/** The service button is held past its threshold — dark over any state — or not. */
void lb_led_release(lb_led_t *led, bool release);

/** One turn of the device's loop: work out the level and write it if it changed. */
void lb_led_tick(lb_led_t *led, uint32_t now_ms);

/** The pattern itself: whether `state` is lit `elapsed_ms` after it began. */
bool lb_led_lit(lb_node_state_t state, uint32_t elapsed_ms);

/** The colour of `state` on an RGB LED, into `rgb[3]`. */
void lb_led_colour(lb_node_state_t state, uint8_t rgb[3]);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_LED_H */
