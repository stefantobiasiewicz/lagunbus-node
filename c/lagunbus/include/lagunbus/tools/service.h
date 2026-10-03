/* The service button — a tool, not a type (zadania/CR-05.md).
 *
 * Every device has one, for a local factory reset; the library's part of the reset is
 * `lb_node_factory_reset()`. How the button guards against an accidental reset is the
 * device's decision, but the project has one rule for all of its devices, and it lives
 * here so it is written — and tested — once:
 *
 *   - **held for `hold_ms` (5 s), the reset happens on the RELEASE**, not at 5 s: past
 *     the threshold the LED goes dark ("let go now", `lb_led_release`). A reset at the
 *     5 s mark would also come from a button pressed by a lid or a cable tie; one that
 *     waits for the release does not — that button never gets released;
 *   - **not armed until seen released**: a button stuck or shorted at start-up would
 *     otherwise reset the node, which restarts, sees it held, resets again — forever.
 *     On the ESP32-C3 Super Mini the button is BOOT on GPIO9, and held at power-on it
 *     enters the download mode anyway; this covers a button held through `esp_restart()`.
 *
 * Feed it the DEBOUNCED level ("is the finger on it", not the pin): `lb_debounce_t`.
 */
#ifndef LAGUNBUS_SERVICE_H
#define LAGUNBUS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The project's hold time — a guideline for its devices, not a rule of the
 * specification (zadania/CR-05.md, N4). */
#define LB_SERVICE_HOLD_MS 5000u

typedef enum {
    LB_SERVICE_IDLE = 0, /**< nothing to do */
    LB_SERVICE_HELD,     /**< held past the threshold: show "let go now" */
    LB_SERVICE_RESET,    /**< released after the threshold: call lb_node_factory_reset() */
} lb_service_event_t;

typedef struct {
    uint32_t hold_ms;
    bool     armed;      /**< seen released since start */
    bool     pressed;
    uint32_t pressed_ms;
} lb_service_t;

void lb_service_init(lb_service_t *s, uint32_t hold_ms);

/** One step. `LB_SERVICE_HELD` comes on every step while held past the threshold, so
 * `lb_led_release(&led, event == LB_SERVICE_HELD)` is all a device needs. */
lb_service_event_t lb_service_update(lb_service_t *s, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_SERVICE_H */
