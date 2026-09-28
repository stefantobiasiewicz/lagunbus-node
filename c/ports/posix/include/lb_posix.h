/* The POSIX port — SocketCAN, a clock and files.
 *
 * This is neither a toy nor a stub: **it is a second node on the bus**, available
 * before anything reaches a board. Plugged into `vcan0` on a Raspberry Pi it looks to
 * the hub exactly like a device — because it is one, only without an enclosure.
 *
 * The module **builds everywhere**, macOS included; only opening the bus
 * needs Linux. The same trick as on the hub side
 * (lagunhub/protocol/bus/socketcan.py) and for the same reason: the package has to
 * build on the machine it is written on.
 */
#ifndef LB_POSIX_H
#define LB_POSIX_H

#include "lagunbus/platform.h"
#include "lagunbus/transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Whether this system has SocketCAN at all. On macOS `false`, and that is normal. */
bool lb_posix_has_socketcan(void);

/** Open the bus. `false`, with `error` filled in, when it cannot be done.
 *
 * `error` receives a sentence ready to be shown to a human — because this is the only
 * place where it can be said WHAT went wrong: a missing interface is a different thing
 * from missing permissions, and from above the two look identical.
 */
bool lb_posix_open(lb_transport_t *out, const char *channel, char *error, size_t error_len);

/** Close the bus. May be called on one that was never initialised. */
void lb_posix_close(lb_transport_t *transport);

/** Platform: monotonic clock, files in the given directory, randomness from
 * `/dev/urandom`.
 *
 * The directory may be NULL — then nothing is persistent and the node announces itself
 * anew after every restart. Fine for experiments, not for service.
 */
bool lb_posix_platform(lb_platform_t *out, const char *catalog);

/** Release whatever the platform held. */
void lb_posix_platform_free(lb_platform_t *platform);

#ifdef __cplusplus
}
#endif

#endif /* LB_POSIX_H */
