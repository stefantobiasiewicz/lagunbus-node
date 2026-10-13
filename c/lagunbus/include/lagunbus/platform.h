/* Time, non-volatile storage and randomness — everything the library needs from
 * the system it runs on.
 *
 * Three things, because that is exactly how many cannot be written portably.
 * Everything else in `c/lagunbus/` is plain C and compiles anywhere.
 */
#ifndef LAGUNBUS_PLATFORM_H
#define LAGUNBUS_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lb_platform {
    /** Milliseconds since start. Must increase monotonically and **must not step
     * backwards** when the clock is set — transaction deadlines are derived from
     * it (04-interakcje.md §2.1). */
    uint32_t (*millis)(void *ctx);

    /** Store persistently. `false` means "it did not work", and the node has to
     * survive that: it keeps running, and after a restart it comes back to
     * whatever was stored earlier. */
    bool (*store)(void *ctx, const char *key, const void *data, size_t len);

    /** Load persisted data. `false` means "not there" — which is the normal state
     * on first boot, not a fault. */
    bool (*load)(void *ctx, const char *key, void *out, size_t len);

    /** Forget a key — a later `load` says "not there". Optional; erasing a key that was
     * never stored is not a fault.
     *
     * Needed by the factory reset since the node stores its settings
     * (zadania/zrobione/TASK-01.md N6): a restart returns RAM to its defaults, but the
     * settings in the store would load right back. `NULL` makes the reset leave them stored — a port
     * with `store` should have it. */
    void (*erase)(void *ctx, const char *key);

    /** Random number.
     *
     * This is here for a specific reason, not for completeness: **the delay before
     * announcing must be random**. Without it, forty nodes coming back after a power
     * cut announce themselves in the same millisecond, collide on the bus and retry —
     * just as much in unison (03-adresacja.md §5).
     */
    uint32_t (*random)(void *ctx);

    /** Restart the node. Optional, and on the board it does not return.
     *
     * Needed by `FactoryReset`: most configuration lives only in RAM and returns to its
     * defaults exactly by restarting (types/0x0001-device.md §3.3). `NULL` — a test, the
     * host binary — makes the library do what it can without one: forget the address
     * and go back to joining, leaving the endpoints' settings as they are. */
    void (*restart)(void *ctx);

    /** Why the node started — one of `LB_RESET_*` (node.h), read once by
     * `lb_node_start`. Optional: `NULL` means `LB_RESET_POWER_ON`.
     *
     * Only the chip knows: on the ESP32 `esp_reset_reason()` tells a watchdog from a
     * brownout from `esp_restart()`. Until 2026-10-03 the node had no way to ask and
     * reported `POWER_ON` after every restart — a node resetting itself once a day looked
     * like one whose power is switched (zadania/zrobione/CR-05.md). */
    uint8_t (*reset_reason)(void *ctx);

    void *ctx;
} lb_platform_t;

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_PLATFORM_H */
