/* Momentary key — type 0x0010 KeyInput. **Tools, not a key.**
 *
 * The library gives a device the pieces and leaves the decisions to it
 * (zadania/CR-01.md, N6 and K1–K5). A device picks what it uses:
 *
 *   - `LB_KEY_ATTR_…`             rows for its attribute array, only the ones it exposes;
 *   - `lb_key_check`,
 *     `lb_key_feature_of`         the endpoint hooks: the type's ranges, `EventMode`
 *                                 against features, `UNSUPPORTED_FEATURE`;
 *   - `lb_key_t`                  the event state machine, fed an already-filtered
 *                                 "pressed" (`lagunbus/tools/debounce.h` is one way to get it);
 *   - `lb_key_emit_…`             the events encoded right, for a device with its own
 *                                 logic that does not use `lb_key_t` at all.
 *
 * Bounce and polarity are not here: they belong to how the device is built
 * (types/0x0010-key-input.md §5.7). Timings the device does not expose as attributes
 * come from its `lb_key_config_t` — fixed, and within the type's ranges (§2.4).
 *
 * Why the state machine is in the library at all: contact bounce aside, the multi-click
 * window and hold are time-dependent, and the only sensible way to check them is to
 * drive the clock from a test (vectors/scenarios/key*.json).
 */
#ifndef LAGUNBUS_KEY_H
#define LAGUNBUS_KEY_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** `KeyInput` attributes and events (types/0x0010-key-input.md). */
#define LB_KEY_PRESS_COUNT         0x00u
#define LB_KEY_EVENT_MODE          0x11u
#define LB_KEY_MULTI_CLICK_WINDOW  0x12u
#define LB_KEY_MAX_CLICKS          0x13u
#define LB_KEY_HOLD_ENABLED        0x14u
#define LB_KEY_HOLD_THRESHOLD      0x15u
#define LB_KEY_HOLD_TICK_INTERVAL  0x16u

#define LB_KEY_EV_DOWN        0x01u
#define LB_KEY_EV_UP          0x02u
#define LB_KEY_EV_CLICK       0x03u
#define LB_KEY_EV_HOLD_START  0x05u
#define LB_KEY_EV_HOLD_TICK   0x06u
#define LB_KEY_EV_HOLD_END    0x07u

#define LB_KEY_MODE_CLICK       0u
#define LB_KEY_MODE_MULTICLICK  1u
#define LB_KEY_MODE_RAW         2u

/** `KeyInput` features (types/0x0010-key-input.md §1). The core is `Click(1)` alone. */
#define LB_KEY_F_MULTICLICK  0x0001u
#define LB_KEY_F_HOLD        0x0002u
#define LB_KEY_F_RAW         0x0004u

/** A key held this long stops sending HoldTick and waits for the release. */
#define LB_KEY_HOLD_TICK_LIMIT_MS  60000u

/* --- attribute rows ----------------------------------------------------------------
 * One row per attribute the device EXPOSES. What it leaves out it sets itself through
 * `lb_key_config_t`; the hub learns that from the first `UNSUPPORTED_ELEMENT`. */
#define LB_KEY_ATTR_PRESS_COUNT() \
    {LB_KEY_PRESS_COUNT, LB_T_U32, LB_RO, LB_NUM(0)}
#define LB_KEY_ATTR_EVENT_MODE() \
    {LB_KEY_EVENT_MODE, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_KEY_MODE_CLICK)}
#define LB_KEY_ATTR_MULTI_CLICK_WINDOW(ms) \
    {LB_KEY_MULTI_CLICK_WINDOW, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(ms)}
#define LB_KEY_ATTR_MAX_CLICKS(n) \
    {LB_KEY_MAX_CLICKS, LB_T_U8, LB_RW | LB_PERSIST, LB_NUM(n)}
#define LB_KEY_ATTR_HOLD_ENABLED() \
    {LB_KEY_HOLD_ENABLED, LB_T_BOOL, LB_RW | LB_PERSIST, LB_NUM(1)}
#define LB_KEY_ATTR_HOLD_THRESHOLD(ms) \
    {LB_KEY_HOLD_THRESHOLD, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(ms)}
#define LB_KEY_ATTR_HOLD_TICK_INTERVAL(ms) \
    {LB_KEY_HOLD_TICK_INTERVAL, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(ms)}

/* --- endpoint hooks ---------------------------------------------------------------- */

/** `on_check` for a `KeyInput` endpoint: the ranges from the type's table, and
 * `EventMode` only in a mode whose feature the endpoint declares. */
uint8_t lb_key_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value);

/** `feature_of` for a `KeyInput` endpoint: which feature each attribute belongs to. */
uint16_t lb_key_feature_of(uint8_t element);

/* --- events ------------------------------------------------------------------------
 * Each takes the endpoint number and returns what `lb_node_emit` returns. Durations are
 * saturated at 0xFFFF ms rather than truncated: "a very long time" is more honest than
 * a random small number. */
bool lb_key_emit_down(lb_node_t *node, uint8_t endpoint);
bool lb_key_emit_up(lb_node_t *node, uint8_t endpoint, uint32_t held_ms);
bool lb_key_emit_click(lb_node_t *node, uint8_t endpoint, uint8_t count);
bool lb_key_emit_hold_start(lb_node_t *node, uint8_t endpoint);
bool lb_key_emit_hold_tick(lb_node_t *node, uint8_t endpoint, uint32_t held_ms);
bool lb_key_emit_hold_end(lb_node_t *node, uint8_t endpoint, uint32_t held_ms);

/** Event name for the log. Never NULL — an unknown one yields "?". */
const char *lb_key_event_name(uint8_t event);

/* --- the state machine ------------------------------------------------------------- */

/** The device's own decisions. An attribute the endpoint exposes wins over the value
 * here; this is what applies when it does not. */
typedef struct {
    uint16_t multi_click_window_ms;  /**< 150–800 */
    uint8_t  max_clicks;             /**< 2–5 */
    uint16_t hold_threshold_ms;      /**< 200–3000 */
    uint16_t hold_tick_ms;           /**< 50–1000 */
    /** A press already on at `lb_key_init` produces nothing, its release included —
     * not a rule of the type, a choice of this implementation (CR-01 N1b, K4). */
    bool     silent_start_press;
} lb_key_config_t;

/** The type's recommended values (types/0x0010-key-input.md §2.2). */
#define LB_KEY_CONFIG_DEFAULT {350u, 3u, 500u, 200u, true}

typedef struct {
    lb_endpoint_t         *endpoint;
    const lb_key_config_t *config;

    bool     pressed;       /**< the last filtered input */
    bool     unseen_press;  /**< on at start and `silent_start_press`: ignore to release */
    uint32_t pressed_ms;    /**< when the current press began */
    bool     hold_active;
    uint32_t last_tick_ms;

    uint8_t  clicks;          /**< counted within the current multi-click window */
    uint32_t window_until_ms;
    bool     window_open;
} lb_key_t;

/** Prepare the key. `pressed` is the filtered input now. `config` must outlive `key`. */
void lb_key_init(lb_key_t *key, lb_endpoint_t *endpoint, const lb_key_config_t *config,
                 bool pressed, uint32_t now_ms);

/** One step. `pressed` is the FILTERED input — bounce and polarity already handled by
 * the device. Call it every few milliseconds; it keeps track of time itself. */
void lb_key_update(lb_key_t *key, lb_node_t *node, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_KEY_H */
