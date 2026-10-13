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
 * a restart: the node stores it when a write lands and loads it before `Booted`.
 *
 * `LB_REPORTED` and `LB_HYST` are the columns "Rap." and the type's word on hysteresis
 * (01-model-danych.md §6.2): the attribute is reported, and `ReportHysteresis` applies
 * to it. Without `LB_HYST` every change is a change — right for `bool`, `enum` and maps,
 * and for numbers the type says report always (`Output.Level`). The TYPE sets them in its
 * init, not the device: whether `OnOff` is reported is the type's contract, not a choice
 * a board could get wrong (zadania/zrobione/TASK-01.md N2). */
#define LB_RO        0x01u
#define LB_WO        0x02u
#define LB_RW        (LB_RO | LB_WO)
#define LB_PERSIST   0x04u
#define LB_REPORTED  0x08u
#define LB_HYST      0x10u

/** System endpoint numbers — the same in every device (01-model-danych.md §2). */
#define LB_EP_DEVICE       0u
#define LB_EP_DIAGNOSTICS  1u
#define LB_EP_GROUPS       2u
#define LB_EP_FIRST_USER   4u

/** First attribute number reserved for the common ones (01-model-danych.md §4). */
#define LB_COMMON_BASE 0xF0u

typedef struct {
    uint8_t    id;
    lb_type_t  type;
    uint8_t    access;
    lb_value_t value;
} lb_attr_t;

/** How many reported attributes one endpoint can have — `Output` with every feature has
 * eight (`OnOff`, `Level`, four colours, `ColorTemperature`, `Transitioning`), the most of
 * any type in the specification. A type that reports more has to raise it; a device can
 * set it lower at compile time. 12 bytes a slot. */
#ifndef LB_REPORTED_MAX
#define LB_REPORTED_MAX 8u
#endif

/** The reporting state of one reported attribute — kept by the node, never set by a
 * device.
 *
 * In the endpoint and not in `lb_attr_t`: every board writes its attribute rows as
 * positional initializers `{id, type, access, value}`, and a fifth field turns each of
 * them into a `-Wmissing-field-initializers` error under `-Wextra -Werror`. */
typedef struct {
    uint8_t    id;
    bool       used;
    bool       baseline; /**< `reported` holds something: reported, or settled at start */
    bool       due;      /**< a report is owed; goes out with the endpoint's next round */
    lb_value_t reported; /**< the last value reported — what hysteresis measures from */
} lb_report_t;

/** The common settings of an endpoint — `Enabled`, `QueryEnabled` and the three of
 * reporting (01-model-danych.md §4). The rest of the common set (`TypeId`, `Features`,
 * `Channel`…) describes the endpoint and lives in its own fields.
 *
 * One struct for system and functional endpoints alike: one code reads and writes them,
 * and the system ones differ only in what they refuse (§4.1). Until TASK-01 the system
 * ones had `lb_reporting_t` in the node and the functional ones nothing — `ReportMinGap`
 * of an output sat in its attribute array, a second description of the same thing.
 *
 * `disabled`, not `enabled`: every endpoint table in the devices is a designated
 * initializer, and a field it does not name is zero. Zero has to mean the default,
 * `Enabled = 1`, or every existing board would start with its endpoints switched off. */
typedef struct {
    bool     disabled;
    uint16_t query_enabled;     /**< stored, never acted on: no type here has inputs (N9) */
    uint16_t report_interval;   /**< s, 0 = no cycle */
    uint16_t report_min_gap;    /**< ms between ROUNDS of reports (01-model-danych.md §6) */
    uint16_t report_hysteresis;
} lb_common_t;

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

/** The bytes of a `string` attribute (`LB_T_STRING`), length prefix included, into
 * `out`. Returns a result code. The node sends them as `LONG_RESPONSE` segments after a
 * `SEGMENTED` reply — a string never fits a frame (02-typy-danych.md §2.2).
 *
 * Why a hook and not bytes in `lb_attr_t`: a row is twelve bytes for every attribute
 * of every board, and a text field needs dozens. Its storage is the type's, sized by the
 * device (`TextLength`). */
typedef uint8_t (*lb_long_read_fn)(struct lb_endpoint *self, uint8_t element,
                                   uint8_t *out, uint8_t cap, uint8_t *len);

/** A `string` attribute written with `LONG_WRITE`. `value` is the whole of it, prefix
 * included, already checked to be whole and to be the string the attribute is; what is
 * left is the type's own table (`TextLength`) and the reaction. Whole or not at all, as
 * every write (04-interakcje.md §4): refuse before changing anything. */
typedef uint8_t (*lb_long_write_fn)(struct lb_endpoint *self, uint8_t element,
                                    const uint8_t *value, uint8_t len);

/** The hysteresis that applies to a reported attribute right now, in its units on the
 * wire, given the value last REPORTED. NULL means `ReportHysteresis` as it is — every
 * type but one.
 *
 * The one is `Sense` measuring light: a change is worth a report at `ReportHysteresis`
 * or 10 % of the last reported value, whichever is more (types/0x0030-sense.md §5.3,
 * zadania/zrobione/DEVICE-01.md N2). The engine knows no quantities, and a type cannot
 * decide alone whether a value is a change — the baseline is the node's — so the type
 * lends the rule and the engine keeps the bookkeeping. A hook returning 0 means any
 * change, as `ReportHysteresis = 0` does. */
typedef uint32_t (*lb_hysteresis_fn)(const struct lb_endpoint *self, uint8_t element,
                                     lb_value_t reported);

typedef struct lb_endpoint {
    uint8_t   number;
    uint16_t  type_id;
    uint8_t   revision;
    uint16_t  features;
    uint8_t   channel;

    lb_attr_t *attrs;
    uint8_t    attr_count;

    /** `Enabled`, `QueryEnabled`, reporting. The device gives its defaults here —
     * `.common = {.report_min_gap = 200}` — and the node restores stored ones over them. */
    lb_common_t common;

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
    lb_long_read_fn  on_read_long;  /**< optional; needed for any `LB_T_STRING` row */
    lb_long_write_fn on_write_long; /**< optional; needed for a writable `LB_T_STRING` */
    lb_hysteresis_fn hysteresis;    /**< optional; NULL = `ReportHysteresis` */
    void         *ctx;

    /* Reporting state — the node's (`lb_report_t`). */
    lb_report_t reports[LB_REPORTED_MAX];
    uint32_t round_ms; /**< when the last round of reports went out */
    bool     rounded;  /**< a round has gone out since `Booted` — the gap counts from it */
    uint32_t cycle_ms; /**< when the current `ReportInterval` started */
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

/** Read a `string` attribute whole — what follows a `SEGMENTED` reply. The same checks
 * as `lb_endpoint_read`; the bytes come from `on_read_long`. */
uint8_t lb_endpoint_read_long(lb_endpoint_t *endpoint, uint8_t element,
                              uint8_t *out, uint8_t cap, uint8_t *len);

/** Write a value that came in segments (`LONG_WRITE`). The checks of
 * 04-interakcje.md §4 in their order — endpoint, element, feature, access — then the
 * form: only a `string` travels in segments, so a scalar here is `INVALID_VALUE`
 * (02-typy-danych.md §2.2), and so is a string whose prefix disagrees with its bytes. */
uint8_t lb_endpoint_write_long(lb_endpoint_t *endpoint, uint8_t element,
                               const uint8_t *value, uint8_t len);

/** Put back a value loaded from the persistent store — a write with the type's checks
 * but without the access check (`PressCount` is `RO` and persistent) and without
 * `on_write`: it runs before the type's init, and the type reads its settings there.
 * A value that no longer passes (a range narrowed by new firmware) is refused and the
 * default stays. */
uint8_t lb_endpoint_restore(lb_endpoint_t *endpoint, uint8_t element,
                            const uint8_t *data, uint8_t len);

/** Is `element` one of the common attributes a write can change — and so one the node
 * persists (all of them are `RW,P`, 01-model-danych.md §4)? */
bool lb_endpoint_common_setting(uint8_t element);

/** Set the value of an attribute the device measures or drives, and owe a report when
 * it is reported and changed enough (01-model-danych.md §6.2). Sends nothing: the report
 * goes out with the endpoint's next round, `lb_node_flush` or the node's tick. Returns
 * false when the endpoint has no such attribute. */
bool lb_endpoint_set(lb_endpoint_t *endpoint, uint8_t element, lb_value_t value);

/** The reporting slot of `element`, taken when `create` and free; NULL otherwise. */
lb_report_t *lb_endpoint_report(lb_endpoint_t *endpoint, uint8_t element, bool create);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_ENDPOINT_H */
