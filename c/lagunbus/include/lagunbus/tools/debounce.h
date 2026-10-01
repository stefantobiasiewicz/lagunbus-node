/* Contact bounce filter — a tool, not a type.
 *
 * Bounce belongs to how an input is built, and the specification leaves it to the
 * device entirely: no type has an attribute for it any more (types/0x0010-key-input.md
 * §5.7, zadania/zrobione/CR-01.md N1a). The device decides whether it needs this at all — a
 * hardware RC filter or a touch controller may do the job — and with what window.
 *
 * Polarity is the device's too: pass in "is the finger on it", not the pin level.
 * Nothing here is `KeyInput`-specific; `Contact` and `ToggleInput` will want the same.
 */
#ifndef LAGUNBUS_DEBOUNCE_H
#define LAGUNBUS_DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     stable;      /**< the accepted level */
    bool     last_raw;    /**< the latest reading */
    uint32_t changed_ms;  /**< when the reading last changed */
} lb_debounce_t;

/** Start from `level` as already stable — a start is not a change. */
void lb_debounce_init(lb_debounce_t *d, bool level, uint32_t now_ms);

/** One step; returns the stable level.
 *
 * A change is accepted once the reading has held for `window_ms` since its LAST
 * change: bounce inside the window restarts it, so a bouncing contact yields one
 * edge, not several. `window_ms = 0` passes the reading straight through.
 */
bool lb_debounce_update(lb_debounce_t *d, bool level, uint32_t now_ms, uint32_t window_ms);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_DEBOUNCE_H */
