/* Endpoint — the smallest thing the hub addresses (01-model-danych.md §1).
 *
 * No allocation: the application supplies the attributes as an array it owns. On the
 * ESP32, having no `malloc` on the working path is worth more than the convenience —
 * a heap that runs out after a week of uptime runs out at three in the morning.
 */
#ifndef LAGUNBUS_ENDPOINT_H
#define LAGUNBUS_ENDPOINT_H

#include "lagunbus/values.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attribute access (01-model-danych.md §3.1). `LB_PERSIST` says the value survives
 * a restart. */
#define LB_RO       0x01u
#define LB_WO       0x02u
#define LB_RW       (LB_RO | LB_WO)
#define LB_PERSIST  0x04u

/** First attribute number reserved for the common ones (01-model-danych.md §4). */
#define LB_COMMON_BASE 0xF0u

typedef struct {
    uint8_t    id;
    lb_type_t  type;
    uint8_t    access;
    lb_value_t value;
} lb_attr_t;

struct lb_endpoint;

/** Command handler. Returns a result code from the registry; `LB_RESULT_OK` means
 * "done". */
typedef uint8_t (*lb_command_fn)(struct lb_endpoint *self, uint8_t element,
                                 const uint8_t *args, uint8_t len);

/** Reaction to a write, AFTER the value has already landed in the array.
 *
 * Separate from the write itself, because writing is common to all types while the
 * reaction belongs to the endpoint: `OnOff` has to flip the relay, `DebounceTime` has
 * to do nothing at all.
 */
typedef uint8_t (*lb_write_fn)(struct lb_endpoint *self, uint8_t element,
                               lb_value_t value);

typedef struct lb_endpoint {
    uint8_t   number;
    uint16_t  type_id;
    uint8_t   revision;
    uint16_t  features;
    uint8_t   channel;

    lb_attr_t *attrs;
    uint8_t    attr_count;

    /** Event counter, **separate for every endpoint** (01-model-danych.md §3.3).
     *
     * So that the hub can see a lost event: a number that jumped means something did
     * not arrive — and on a bus that drops frames it is better to know.
     */
    uint8_t   event_seq;

    lb_command_fn on_command;
    lb_write_fn   on_write;
    void         *ctx;
} lb_endpoint_t;

/** Attribute by number, or NULL. */
lb_attr_t *lb_endpoint_attr(lb_endpoint_t *endpoint, uint8_t element);

/** Read an attribute into a buffer. Returns a result code, and the value length
 * through `len`. */
uint8_t lb_endpoint_read(lb_endpoint_t *endpoint, uint8_t element,
                         uint8_t *out, uint8_t cap, uint8_t *len);

/** Write an attribute. Checks access, type and range — in that order. */
uint8_t lb_endpoint_write(lb_endpoint_t *endpoint, uint8_t element,
                          const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_ENDPOINT_H */
