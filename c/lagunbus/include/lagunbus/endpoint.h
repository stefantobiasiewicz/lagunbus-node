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

/** A check BEFORE a write lands. Returns a result code; anything but `LB_RESULT_OK`
 * refuses the write and the old value stays.
 *
 * The generic write knows only the value type — that a `u16` fits in 16 bits. The
 * type's own table (`HoldThreshold` 200–3000 ms) and rules between attributes and
 * features (`EventMode = RAW` needs `RAW`) belong to the type, so the type supplies
 * them here. It has to run before the value lands, not in `on_write`: a write is whole
 * or not at all (04-interakcje.md §4, LB-NODE-026), and a refusal after the fact would
 * leave the refused value in place.
 */
typedef uint8_t (*lb_check_fn)(const struct lb_endpoint *self, uint8_t element,
                               lb_value_t value);

/** The features an element of the TYPE needs, or 0 when the type does not know it.
 *
 * Needed for one answer: an element the type defines but whose feature this endpoint
 * does not declare is `UNSUPPORTED_FEATURE`, not `UNSUPPORTED_ELEMENT`
 * (04-interakcje.md §9, LB-NODE-061). The attribute array cannot tell — a device
 * simply does not list what it lacks — so the type answers. Any one bit of the mask
 * is enough (`[MULTICLICK, RAW]`, 01-model-danych.md §8).
 */
typedef uint16_t (*lb_feature_fn)(uint8_t element);

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
    lb_check_fn   on_check;    /**< optional; NULL = the value type is the only check */
    lb_feature_fn feature_of;  /**< optional; NULL = no element is feature-bound */
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
