/* One measured quantity — type 0x0030 Sense (types/0x0030-sense.md).
 *
 * As with the key and the display: the type knows what a measurement means on the bus,
 * the application knows how to take one. The application reads its sensor (I²C, an ADC,
 * a simulator's slider) and hands the result in — `lb_sense_update` with the raw value in
 * the quantity's units, `lb_sense_fail` when the reading is impossible or not to be
 * trusted. When a report goes out is the node's reporting engine's business
 * (01-model-danych.md §6): a change of at least `ReportHysteresis`, the cycle of
 * `ReportInterval`, never closer than `ReportMinGap`.
 *
 * The whole type: `Value` and `Quantity` (zadania/zrobione/FEAT-13.md), the features `MIN_MAX`
 * and `THRESHOLD` and `Offset` (zadania/zrobione/DEVICE-01.md). A device lists the rows it
 * has; one declaring no features and no `Offset` is still a complete `Sense` (§1).
 */
#ifndef LAGUNBUS_SENSE_H
#define LAGUNBUS_SENSE_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_SNS_VALUE                 0x00u
#define LB_SNS_QUANTITY              0x01u
#define LB_SNS_MIN_MEASURED          0x02u
#define LB_SNS_MAX_MEASURED          0x03u
#define LB_SNS_OFFSET                0x10u
#define LB_SNS_THRESHOLD_LOW         0x11u
#define LB_SNS_THRESHOLD_HIGH        0x12u
#define LB_SNS_THRESHOLD_HYSTERESIS  0x13u

#define LB_SNS_CMD_RESET_MIN_MAX     0x00u
#define LB_SNS_EVENT_THRESHOLD       0x01u

#define LB_SNS_F_MIN_MAX    (1u << 0)
#define LB_SNS_F_THRESHOLD  (1u << 1)

/* `zone` of `Threshold` (§4). */
#define LB_SNS_ZONE_NORMAL      0u
#define LB_SNS_ZONE_BELOW_LOW   1u
#define LB_SNS_ZONE_ABOVE_HIGH  2u

/** How long `MinMeasured`/`MaxMeasured` may wait to be stored (§5.7, DEVICE-01 N10). */
#define LB_SNS_MIN_MAX_STORE_MS 600000u

/* --- attribute rows ----------------------------------------------------------------
 * `Value` gets its wire type from `Quantity` in `lb_sense_init`, so a board cannot give a
 * temperature an `u16` — the same rule as `Display.Value`. `Quantity` is the device's,
 * read-only, never stored: a thermometer does not start measuring pressure (§2.3). */
#define LB_SNS_ATTR_VALUE() \
    {LB_SNS_VALUE, LB_T_COUNT, LB_RO, LB_ABSENT}
#define LB_SNS_ATTR_QUANTITY(quantity) \
    {LB_SNS_QUANTITY, LB_T_ENUM8, LB_RO, LB_NUM(quantity)}

/* `MIN_MAX`. Persistent and read-only: kept by the type, stored by it at most every
 * `LB_SNS_MIN_MAX_STORE_MS` (§5.7) — a factory reset leaves them, they are measurements,
 * not settings. Their `⟨Q⟩` type comes from `lb_sense_defaults`, before the node restores
 * them. */
#define LB_SNS_ATTR_MIN_MEASURED() \
    {LB_SNS_MIN_MEASURED, LB_T_COUNT, LB_RO | LB_PERSIST, LB_ABSENT}
#define LB_SNS_ATTR_MAX_MEASURED() \
    {LB_SNS_MAX_MEASURED, LB_T_COUNT, LB_RO | LB_PERSIST, LB_ABSENT}

/* Optional: `i16` in the quantity's units whatever the quantity (§2.2, §5.1). */
#define LB_SNS_ATTR_OFFSET() \
    {LB_SNS_OFFSET, LB_T_I16, LB_RW | LB_PERSIST, LB_NUM(0)}

/* `THRESHOLD`. Absent by default — a threshold without a value is inactive (§5.4). The
 * hysteresis is optional within the feature; a device without the row uses the
 * quantity's report hysteresis (`lb_sense_defaults`), which is also the default of the
 * row — the noise of the sensor is the same whether a report or a zone hears it. */
#define LB_SNS_ATTR_THRESHOLD_LOW() \
    {LB_SNS_THRESHOLD_LOW, LB_T_COUNT, LB_RW | LB_PERSIST, LB_ABSENT}
#define LB_SNS_ATTR_THRESHOLD_HIGH() \
    {LB_SNS_THRESHOLD_HIGH, LB_T_COUNT, LB_RW | LB_PERSIST, LB_ABSENT}
#define LB_SNS_ATTR_THRESHOLD_HYSTERESIS() \
    {LB_SNS_THRESHOLD_HYSTERESIS, LB_T_U16, LB_RW | LB_PERSIST, LB_ABSENT}

typedef struct {
    lb_endpoint_t *endpoint;
    lb_node_t     *node;

    /* `THRESHOLD`: the zone, known from the first good reading on (§5.5). */
    bool    zoned;
    uint8_t zone;

    /* `MIN_MAX`: changed since stored, and when they were stored (§5.7). */
    bool     extremes_dirty;
    uint32_t extremes_stored_ms;
} lb_sense_t;

/** What the device is before it starts: the `⟨Q⟩` rows typed by `quantity`, the
 * defaults of §5.3 — `ReportHysteresis` and `ReportInterval` into `endpoint->common` —
 * and the default `ThresholdHysteresis`. Call it while defining the device, BEFORE
 * `lb_node_start()`: the node restores stored values over these, and needs the types to
 * know how many bytes a stored threshold is (two for a temperature, four for light). A
 * factory reset brings the defaults back. A board with a better idea of its sensor's
 * noise sets `.common` itself afterwards. */
void lb_sense_defaults(lb_endpoint_t *endpoint, uint8_t quantity);

/** Prepare the endpoint: `Value` absent and reported with hysteresis, the type's hooks
 * on the endpoint (`on_command`, `on_check`, `feature_of`, `hysteresis`; `ctx` is the
 * sense). Nothing is reported until the first measurement (§5.5). Call it after
 * `lb_node_start()`, as the other types. */
void lb_sense_init(lb_sense_t *sense, lb_node_t *node, lb_endpoint_t *endpoint);

/** The features an element needs — `feature_of` for a device that wants it before
 * `lb_sense_init`. */
uint16_t lb_sense_feature_of(uint8_t element);

/** A measurement, in the quantity's units on the wire (0.01 °C for a temperature), before
 * `Offset`. A report goes out at once when the change is worth one, a `Threshold` before it
 * when the zone changed. */
void lb_sense_update(lb_sense_t *sense, int32_t raw);

/** The reading failed: `Value` becomes absent and is reported (§5.2). Never a zero —
 * a thermostat would heat a room to a temperature nobody measured. */
void lb_sense_fail(lb_sense_t *sense);

/** The current `Value` — absent before the first measurement and after a failure. */
lb_value_t lb_sense_value(const lb_sense_t *sense);

/** `Quantity`, or `0xFF` when the device did not declare one. */
uint8_t lb_sense_quantity(const lb_sense_t *sense);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_SENSE_H */
