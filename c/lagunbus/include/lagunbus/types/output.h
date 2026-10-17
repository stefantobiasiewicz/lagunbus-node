/* A driven output — type 0x0020 Output: a relay, or a light with `LEVEL` and
 * `COLOR_RGB`.
 *
 * Same as with the key: the logic lives here, driving the pin lives in the
 * application. `AutoOffTime`, `PowerOnBehaviour` and transitions are behaviours that
 * depend on time and on restarts — they can only be checked when the clock and the
 * persistent store are in the test's hands.
 *
 * Two kinds of output, chosen at init: `lb_output_init` drives a coil (`bool`),
 * `lb_output_init_light` a light (`level` and colour). A light hands its drive function
 * INTENT — the level in per mille and the colour at full brightness — and the device
 * turns that into current: the brightness curve, the channel order, a white channel it
 * may not have, a cap on the current (types/0x0020-output.md §5.8;
 * zadania/zrobione/DEVICE-03.md N3, H1). The same reasoning as the coil's polarity: the
 * device knows its LEDs, the bus does not.
 */
#ifndef LAGUNBUS_OUTPUT_H
#define LAGUNBUS_OUTPUT_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_OUT_ON_OFF              0x00u
#define LB_OUT_LEVEL               0x01u
#define LB_OUT_RED                 0x02u
#define LB_OUT_GREEN               0x03u
#define LB_OUT_BLUE                0x04u
#define LB_OUT_WHITE               0x05u
#define LB_OUT_TRANSITIONING       0x07u
#define LB_OUT_LAST_LEVEL          0x08u
#define LB_OUT_MIN_LEVEL           0x10u
#define LB_OUT_MAX_LEVEL           0x11u
#define LB_OUT_DEFAULT_FADE_TIME   0x12u
#define LB_OUT_POWER_ON_BEHAVIOUR  0x13u
#define LB_OUT_INVERT              0x14u
#define LB_OUT_AUTO_OFF_TIME       0x16u
#define LB_OUT_PURPOSE             0x17u

#define LB_OUT_CMD_OFF     0x00u
#define LB_OUT_CMD_ON      0x01u
#define LB_OUT_CMD_TOGGLE  0x02u
#define LB_OUT_CMD_SET_LEVEL  0x03u
#define LB_OUT_CMD_STEP       0x04u
#define LB_OUT_CMD_STOP       0x05u
#define LB_OUT_CMD_SET_COLOR  0x06u
#define LB_OUT_CMD_SET_COLOR_TEMPERATURE 0x07u
#define LB_OUT_CMD_PULSE      0x08u

/** Features (types/0x0020-output.md §1). */
#define LB_OUT_F_LEVEL      0x0001u
#define LB_OUT_F_COLOR_RGB  0x0002u
#define LB_OUT_F_COLOR_CCT  0x0004u
#define LB_OUT_F_PULSE      0x0008u

/** `transition` meaning "use `DefaultFadeTime`" (§3). */
#define LB_OUT_TRANSITION_DEFAULT 0xFFFFu
/** How often a transition is recomputed — 40 steps over the default 400 ms, smooth to
 * the eye (zadania/zrobione/DEVICE-03.md N4). */
#define LB_OUT_STEP_MS 10u
/** How long the level, `LastLevel` and the colour wait unchanged before they are stored
 * (§2.3, §5.8): a knob turned for a minute is one write, not hundreds. */
#define LB_OUT_STORE_AFTER_MS 5000u

/** What to do when power comes back (types/0x0020-output.md §3). */
#define LB_POWER_ON_OFF      0u
#define LB_POWER_ON_ON       1u
#define LB_POWER_ON_RESTORE  2u

/** Set the physical output. `level` is the state of the COIL, with `Invert` already
 * applied when the endpoint has it.
 *
 * `Invert` is installation only — the receiver wired to the normally-closed contact.
 * The polarity of the board's own drive (a relay that switches on a LOW pin) is build:
 * the application inverts the pin here, and nothing on the bus can overwrite it
 * (types/0x0020-output.md §5.6; README.md § Decisions). */
typedef void (*lb_output_drive_fn)(void *ctx, bool level);

/** Light the output: `level` 0–1000 ‰ and the colour at full brightness, `rgbw`
 * {red, green, blue, white} 0–254 (0xFF is `u8`'s "absent") — NULL on an output without `COLOR_RGB`. Called at
 * init, at every change and every `LB_OUT_STEP_MS` of a transition; must not block. */
typedef void (*lb_output_light_fn)(void *ctx, uint16_t level, const uint8_t *rgbw);

/** A transition under way (§5.3, §5.8) — one for the level and the colour together. */
typedef struct {
    bool     active;
    uint32_t start_ms;
    uint32_t duration_ms;
    uint16_t from_level, to_level;
    uint8_t  from_rgbw[4], to_rgbw[4];
} lb_output_fade_t;

/** What a light stores, as last seen — compared at each tick to notice a change. Where
 * the light is GOING, not where a transition has got to. Eight bytes, no padding. */
typedef struct {
    uint16_t level;  /**< for `RESTORE`; 0 otherwise */
    uint16_t last;   /**< `LastLevel` */
    uint8_t  rgbw[4];
} lb_output_stored_t;

typedef struct {
    lb_endpoint_t     *endpoint;
    lb_node_t         *node;
    lb_output_drive_fn drive;
    lb_output_light_fn light;  /**< a light — `drive` is NULL then, and the other way round */
    void              *ctx;

    uint32_t lit_ms;   /**< when it was switched on — for AutoOffTime */

    /* A light only. `level`/`rgbw` are what shines NOW; the attributes say the same
     * except that, during a transition, they are written without being reported. */
    uint16_t         level;
    uint8_t          rgbw[4];
    lb_output_fade_t fade;
    uint32_t         step_ms;    /**< when the transition was last recomputed */
    lb_output_stored_t stored;   /**< as last seen */
    bool             dirty;      /**< something to store once it has been still long enough */
    uint32_t         changed_ms; /**< the last change of what is stored */
} lb_output_t;

/** Prepare the output and apply `PowerOnBehaviour`.
 *
 * Call it AFTER `lb_node_start()`: `RESTORE` reads the state from the persistent
 * store, and that is only available by then.
 */
void lb_output_init(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                    lb_output_drive_fn drive, void *ctx);

/** Prepare a light — `LEVEL`, and `COLOR_RGB` if the endpoint declares it — and apply
 * `PowerOnBehaviour`. After `lb_node_start()`, as `lb_output_init`: `LastLevel`, the
 * colour and the settings are the node's to restore first. */
void lb_output_init_light(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                          lb_output_light_fn light, void *ctx);

/** Handle a command with its arguments — the endpoint's `on_command`. Returns the
 * result code for the reply. A command of a feature the endpoint does not declare is
 * `UNSUPPORTED_FEATURE` (04-interakcje.md §9, LB-NODE-061). */
uint8_t lb_output_handle(lb_output_t *out, uint8_t element, const uint8_t *args,
                         uint8_t len);

/** `lb_output_handle` without arguments — `On`/`Off`/`Toggle` of a relay. */
uint8_t lb_output_command(lb_output_t *out, uint8_t element);

/** The type's ranges, for the endpoint's `on_check` (`MinLevel` 1–1000 ‰ …). */
uint8_t lb_output_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value);

/** The feature an attribute of the type needs — the endpoint's `feature_of`. */
uint16_t lb_output_feature_of(uint8_t element);

/** Set the state directly. `true` = the receiver on (before `Invert`). */
void lb_output_set(lb_output_t *out, bool state);

/** React to a write that has already landed — wire it into the endpoint's `on_write`.
 *
 * Only `Invert` needs anything: `OnOff` describes the receiver, so when the contact it
 * hangs on changes, the coil has to move AT ONCE to keep the receiver where it was.
 * Waiting for the next command left the receiver switched while `OnOff` said otherwise
 * (zadania/zrobione/BUG-07.md). The receiver does not change, so nothing is reported.
 * Every other setting is read when it is used, so the rest is just `LB_RESULT_OK`. */
uint8_t lb_output_write(lb_output_t *out, uint8_t element, lb_value_t value);

/** Counts down `AutoOffTime`, steps a transition, stores a light's state once it has
 * been still for `LB_OUT_STORE_AFTER_MS`. Call it in the loop. Held-back reports are the node's
 * (`lb_node_tick`). */
void lb_output_tick(lb_output_t *out, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_OUTPUT_H */
