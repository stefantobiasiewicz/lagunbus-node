/* A driven output — type 0x0020 Output, the variant without dimming.
 *
 * Same as with the key: the logic lives here, driving the pin lives in the
 * application. `AutoOffTime` and `PowerOnBehaviour` are behaviours that depend on time
 * and on restarts — both can only be checked when the clock and the persistent store
 * are in the test's hands.
 */
#ifndef LAGUNBUS_OUTPUT_H
#define LAGUNBUS_OUTPUT_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_OUT_ON_OFF              0x00u
#define LB_OUT_POWER_ON_BEHAVIOUR  0x13u
#define LB_OUT_INVERT              0x14u
#define LB_OUT_AUTO_OFF_TIME       0x16u
#define LB_OUT_PURPOSE             0x17u

#define LB_OUT_CMD_OFF     0x00u
#define LB_OUT_CMD_ON      0x01u
#define LB_OUT_CMD_TOGGLE  0x02u

/** What to do when power comes back (types/0x0020-output.md §3). */
#define LB_POWER_ON_OFF      0u
#define LB_POWER_ON_ON       1u
#define LB_POWER_ON_RESTORE  2u

/** Set the physical output. `Invert` has already been applied — this is the pin
 * level. */
typedef void (*lb_output_drive_fn)(void *ctx, bool level);

typedef struct {
    lb_endpoint_t     *endpoint;
    lb_node_t         *node;
    lb_output_drive_fn drive;
    void              *ctx;

    uint32_t lit_ms;   /**< when it was switched on — for AutoOffTime */
    uint32_t last_report_ms;
    bool     report_pending;
} lb_output_t;

/** Prepare the output and apply `PowerOnBehaviour`.
 *
 * Call it AFTER `lb_node_start()`: `RESTORE` reads the state from the persistent
 * store, and that is only available by then.
 */
void lb_output_init(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                    lb_output_drive_fn drive, void *ctx);

/** Handle an `On`/`Off`/`Toggle` command. Returns the result code for the reply. */
uint8_t lb_output_command(lb_output_t *out, uint8_t element);

/** Set the state directly. `true` = on (before `Invert` is taken into account). */
void lb_output_set(lb_output_t *out, bool state);

/** Counts down `AutoOffTime` and releases held-back reports. Call it in the loop. */
void lb_output_tick(lb_output_t *out, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_OUTPUT_H */
