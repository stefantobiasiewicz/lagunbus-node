/* Scalar value codec — lagunbus-spec/02-typy-danych.md.
 *
 * Two rules from there hold without exception: EVERYTHING IS LITTLE-ENDIAN and THERE
 * ARE NO FLOATING-POINT NUMBERS. Every physical quantity is an integer with a fixed
 * scale — which on the ESP32-C3 is an advantage rather than a limitation: the RISC-V
 * core has no floating-point unit, so every `float` would cost emulation exactly where
 * response time matters.
 */
#ifndef LAGUNBUS_VALUES_H
#define LAGUNBUS_VALUES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Scalar types from 02-typy-danych.md §1. The order is irrelevant — the numbers
 * exist only for the code. */
typedef enum {
    LB_T_BOOL = 0,
    LB_T_U8,
    LB_T_U16,
    LB_T_U32,
    LB_T_I8,
    LB_T_I16,
    LB_T_I32,
    LB_T_ENUM8,
    LB_T_ENUM16,
    LB_T_MAP8,
    LB_T_MAP16,
    LB_T_MAP32,
    /** A `string` attribute (02-typy-danych.md §2.2). Not a scalar: it has no size, no
     * range and no "absent", and every helper above treats it as unknown — `lb_encode`
     * gives 0, `lb_decode` false. That is the point: an ordinary write or report of it
     * fails by itself, because a string always travels in segments. Its bytes are the
     * endpoint's (`on_read_long`, `on_write_long` in endpoint.h), not `lb_attr_t.value`. */
    LB_T_STRING,
    LB_T_COUNT
} lb_type_t;

/** A scalar value. `absent` tells "I don't know" apart from zero — see
 * 02-typy-danych.md §1.3. */
typedef struct {
    int32_t value;
    bool absent;
} lb_value_t;

#define LB_ABSENT ((lb_value_t){0, true})
#define LB_NUM(x) ((lb_value_t){(int32_t)(x), false})

/** Size of a type in bytes. Zero for an unknown one. */
uint8_t lb_size_of(lb_type_t type);

/** Whether this type has an "absent" value at all.
 *
 * Bit fields (`map*`) **do not**: any set of bits is a valid state, so there is no
 * pattern that could be sacrificed for it (02-typy-danych.md §1.3).
 */
bool lb_has_absent(lb_type_t type);

/** Smallest and largest value of a type. The range excludes the "absent" pattern. */
int32_t lb_lo(lb_type_t type);
int32_t lb_hi(lb_type_t type);

/** Whether a value fits the type. "Absent" fits wherever it exists. */
bool lb_in_range(lb_type_t type, lb_value_t value);

/** Write a value into a buffer. Returns the byte count, or 0 when it cannot be done.
 *
 * `out` must hold at least `lb_size_of(type)` bytes — `cap` is what checks that.
 */
uint8_t lb_encode(lb_type_t type, lb_value_t value, uint8_t *out, size_t cap);

/** Read a value back. Returns false when the length does not match the type. */
bool lb_decode(lb_type_t type, const uint8_t *data, size_t len, lb_value_t *out);

/* --- physical quantities, 02-typy-danych.md §3 ------------------------------------
 *
 * `Quantity` (types/0x0030-sense.md §2.3) and the type a value of it travels as — the
 * `⟨Q⟩` of `Sense` and `Display`. Copied by hand from lagunbus-spec/registry/quantities.yaml,
 * as the hub's `protocol/quantities.py` is: the generator reads `id`/`name` registries and
 * this one is keyed differently. `vectors/codec.json` § quantities pins every row, so a
 * scale changed in the spec alone fails the conformance tests. The scale and unit are the
 * hub's business — a node never shows a number to a person. */
#define LB_QUANTITY_TEMPERATURE    0x00u
#define LB_QUANTITY_HUMIDITY       0x01u
#define LB_QUANTITY_ILLUMINANCE    0x02u
#define LB_QUANTITY_CO2            0x03u
#define LB_QUANTITY_VOC            0x04u
#define LB_QUANTITY_PM25           0x05u
#define LB_QUANTITY_PRESSURE       0x06u
#define LB_QUANTITY_FLOW           0x07u
#define LB_QUANTITY_SOIL_MOISTURE  0x08u
#define LB_QUANTITY_VOLTAGE        0x09u
#define LB_QUANTITY_CURRENT        0x0Au
#define LB_QUANTITY_POWER          0x0Bu
#define LB_QUANTITY_DISTANCE       0x0Cu

/** The wire type of a quantity's values, or `LB_T_COUNT` for one the library does not
 * know. */
lb_type_t lb_quantity_type(uint8_t quantity);

/* --- values longer than a frame, 02-typy-danych.md §2.2 -------------------------
 *
 * Strings and binary data NEVER fit into a single frame — they travel in segments
 * (05-mapowanie-can.md §6). What happens here is assembling the value itself with its
 * length prefix; the slicing is the layer above's business (`lb_node_send_segments`).
 */
#define LB_STRING_PREFIX 1
#define LB_BLOB_PREFIX   2

/** Whether `data` is a whole string — its `u8` prefix says exactly the bytes that
 * follow (05-mapowanie-can.md §6.6). The text itself is not checked as UTF-8: a field
 * draws what it can and puts `?` for the rest (types/0x0022-display.md §5.3). */
bool lb_string_whole(const uint8_t *data, size_t len);

/** A blob with a two-byte length prefix. Returns the total length, or 0. */
size_t lb_encode_blob(const uint8_t *data, size_t len, uint8_t *out, size_t cap);

/** A pointer to the data inside the buffer and its length. Returns false when the
 * two disagree. */
bool lb_decode_blob(const uint8_t *data, size_t len, const uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_VALUES_H */
