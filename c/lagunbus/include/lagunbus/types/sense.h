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
 * Only the core of the type for now: `Value` and `Quantity`. The features `MIN_MAX` and
 * `THRESHOLD`, and `Offset`, come with the board that needs them — DEVICE-01; this much is
 * what a simulated sensor feeding a display field needs (zadania/zrobione/FEAT-13.md). A device
 * declaring no features is a complete `Sense` (§1).
 */
#ifndef LAGUNBUS_SENSE_H
#define LAGUNBUS_SENSE_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_SNS_VALUE     0x00u
#define LB_SNS_QUANTITY  0x01u

/* --- attribute rows ----------------------------------------------------------------
 * `Value` gets its wire type from `Quantity` in `lb_sense_init`, so a board cannot give a
 * temperature an `u16` — the same rule as `Display.Value`. `Quantity` is the device's,
 * read-only, never stored: a thermometer does not start measuring pressure (§2.3). */
#define LB_SNS_ATTR_VALUE() \
    {LB_SNS_VALUE, LB_T_COUNT, LB_RO, LB_ABSENT}
#define LB_SNS_ATTR_QUANTITY(quantity) \
    {LB_SNS_QUANTITY, LB_T_ENUM8, LB_RO, LB_NUM(quantity)}

typedef struct {
    lb_endpoint_t *endpoint;
    lb_node_t     *node;
} lb_sense_t;

/** The reporting defaults of §5.3 for `quantity` — `ReportHysteresis` and
 * `ReportInterval` into `endpoint->common`. Call it while defining the device, BEFORE
 * `lb_node_start()`: the node restores stored settings over the defaults, and a factory
 * reset brings these back. A board with a better idea of its sensor's noise sets
 * `.common` itself instead. */
void lb_sense_defaults(lb_endpoint_t *endpoint, uint8_t quantity);

/** Prepare the endpoint: `Value` typed by `Quantity`, absent, reported with hysteresis.
 * Nothing is reported until the first measurement (§5.5). Call it after
 * `lb_node_start()`, as the other types. */
void lb_sense_init(lb_sense_t *sense, lb_node_t *node, lb_endpoint_t *endpoint);

/** A measurement, in the quantity's units on the wire (0.01 °C for a temperature). A
 * report goes out at once when the change is worth one. */
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
