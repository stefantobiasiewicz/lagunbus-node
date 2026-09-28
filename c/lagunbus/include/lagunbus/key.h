/* Momentary key — type 0x0010 KeyInput.
 *
 * **All of the logic is here, not in the application on the ESP32.** The reason is
 * practical: contact bounce, the multi-click window and hold are all time-dependent,
 * and the only sensible way to check them is to drive the clock from a test. What is
 * left on the board is reading the pin and calling `lb_key_update()`.
 *
 * The settings are not fields of this struct — it reads them from the endpoint's
 * attributes, because the hub owns them and has to be able to change them on the fly.
 *
 * See zadania/CR-01.md: whether this division is the right one is an open question.
 */
#ifndef LAGUNBUS_KEY_H
#define LAGUNBUS_KEY_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** `KeyInput` attributes and events (types/0x0010-key-input.md). */
#define LB_KEY_PRESS_COUNT         0x00u
#define LB_KEY_DEBOUNCE_TIME       0x10u
#define LB_KEY_EVENT_MODE          0x11u
#define LB_KEY_MULTI_CLICK_WINDOW  0x12u
#define LB_KEY_MAX_CLICKS          0x13u
#define LB_KEY_HOLD_ENABLED        0x14u
#define LB_KEY_HOLD_THRESHOLD      0x15u
#define LB_KEY_HOLD_TICK_INTERVAL  0x16u
#define LB_KEY_INVERT              0x17u

#define LB_KEY_EV_DOWN        0x01u
#define LB_KEY_EV_UP          0x02u
#define LB_KEY_EV_CLICK       0x03u
#define LB_KEY_EV_HOLD_START  0x05u
#define LB_KEY_EV_HOLD_TICK   0x06u
#define LB_KEY_EV_HOLD_END    0x07u

#define LB_KEY_MODE_CLICK       0u
#define LB_KEY_MODE_MULTICLICK  1u
#define LB_KEY_MODE_RAW         2u

typedef struct {
    lb_endpoint_t *endpoint;

    /* Contact bounce: `stable` is the accepted state, `last_raw` the latest reading. */
    bool     stable;
    bool     last_raw;
    uint32_t changed_ms;

    uint32_t pressed_ms;   /**< when the current press began */
    bool     hold_active;
    uint32_t last_tick_ms;

    uint8_t  clicks;          /**< counted within the current multi-click window */
    uint32_t window_until_ms;
    bool     window_open;
} lb_key_t;

/** Event name for the log. Never NULL — an unknown one yields "?". */
const char *lb_key_event_name(uint8_t event);

/** Prepare the key. The initial state comes from `level`, so that a start does not
 * look like a press — a node powered up with the key held down has nothing to
 * report. */
void lb_key_init(lb_key_t *key, lb_endpoint_t *endpoint, bool level, uint32_t now_ms);

/** One step. `level` is the RAW pin reading; `Invert` is applied by this function.
 *
 * Call it often — every few milliseconds. It keeps track of time itself, so it does
 * not matter whether the caller is punctual.
 */
void lb_key_update(lb_key_t *key, lb_node_t *node, bool level, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_KEY_H */
