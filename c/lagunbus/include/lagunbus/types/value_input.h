/* An input with a value of its own — type 0x0011 ValueInput. **Tools, not a knob.**
 *
 * The type describes a NUMBER, not a finger (types/0x0011-value-input.md §1,
 * zadania/zrobione/CR-04.md): how a device arrives at the number — an encoder's detents,
 * a key pair, a slider's position — is the device's. So the library has no hardware
 * here, not even a quadrature decoder (zadania/zrobione/DEVICE-02.md N10): a board turns
 * its pins into "n steps up or down" and hands that in. What the library keeps is what
 * the hub sees and what a test can pin with a driven clock:
 *
 *   - `LB_VI_ATTR_…`             rows for the attribute array, only the ones exposed;
 *   - `lb_value_input_defaults`  the type's reporting defaults, before the start;
 *   - `lb_value_input_t`         the value: steps, limits, wrap, `Changed`, the report,
 *                                and storing it once the turning stops.
 *
 * No press/release with hold: since CR-04 hold is not the type's, and no device needs it
 * yet. A key pair that scrolls on hold calls `lb_value_input_step` from its own timer.
 */
#ifndef LAGUNBUS_VALUE_INPUT_H
#define LAGUNBUS_VALUE_INPUT_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** `ValueInput` attributes and events (types/0x0011-value-input.md §2, §4). */
#define LB_VI_VALUE         0x00u
#define LB_VI_CHANGE_COUNT  0x01u
#define LB_VI_MIN_VALUE     0x10u
#define LB_VI_MAX_VALUE     0x11u
#define LB_VI_STEP_SIZE     0x12u
#define LB_VI_WRAP          0x18u

#define LB_VI_EV_CHANGED    0x01u

/** The top of the scale: the value is in ‰ (§5.1). */
#define LB_VI_FULL          1000u

/** How long the value has to stand still before it is stored (§5.6): one scroll of four
 * seconds would otherwise cost twenty erase cycles. */
#define LB_VI_STORE_AFTER_MS  5000u

/** The type's reporting defaults (§5.7): the cycle as everywhere, and a gap of a second
 * between reports — turning costs as many events as detents and one report a second. */
#define LB_VI_REPORT_INTERVAL_S  300u
#define LB_VI_REPORT_MIN_GAP_MS  1000u

/* --- attribute rows ----------------------------------------------------------------
 * `Value` starts at 0 and is clipped into the range at `lb_value_input_init`, so a
 * device whose `MinValue` is not 0 starts — and comes back from a factory reset — at its
 * `MinValue` without repeating it here (§5.6). `ChangeCount` is RO and persistent like
 * `PressCount`: wear, not configuration, so a factory reset leaves it. */
#define LB_VI_ATTR_VALUE() \
    {LB_VI_VALUE, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(0)}
#define LB_VI_ATTR_CHANGE_COUNT() \
    {LB_VI_CHANGE_COUNT, LB_T_U32, LB_RO | LB_PERSIST, LB_NUM(0)}
#define LB_VI_ATTR_MIN_VALUE(permille) \
    {LB_VI_MIN_VALUE, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(permille)}
#define LB_VI_ATTR_MAX_VALUE(permille) \
    {LB_VI_MAX_VALUE, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(permille)}
#define LB_VI_ATTR_STEP_SIZE(permille) \
    {LB_VI_STEP_SIZE, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(permille)}
#define LB_VI_ATTR_WRAP(on) \
    {LB_VI_WRAP, LB_T_BOOL, LB_RW | LB_PERSIST, LB_NUM(on)}

typedef struct {
    lb_endpoint_t *endpoint;
    lb_node_t     *node;
    /** The step when the endpoint does not expose `StepSize` — the device's own, fixed,
     * 1–1000 ‰ (§2.2). */
    uint16_t own_step;

    /* Changed since stored, and when it last changed (§5.6). */
    bool     dirty;
    uint32_t changed_ms;
} lb_value_input_t;

/** The type's defaults into `endpoint->common` — `ReportInterval`, `ReportMinGap`. Call
 * it while defining the device, BEFORE `lb_node_start()`: the node restores stored ones
 * over these, and a factory reset brings these back. */
void lb_value_input_defaults(lb_endpoint_t *endpoint);

/** Prepare the endpoint: `Value` reported on every change (no hysteresis — a detent is
 * a decision, not noise, §4.1) and clipped into the range, the type's hooks on the
 * endpoint (`on_check`, `on_write`; `ctx` is the input). Call it after
 * `lb_node_start()`, as the other types: the hooks come after the restore, so a stored
 * `Value` is clipped here rather than refused against a range not restored yet. */
void lb_value_input_init(lb_value_input_t *vi, lb_node_t *node, lb_endpoint_t *endpoint,
                         uint16_t own_step);

/** `steps` steps by a person: up when positive, down when negative. Several handed in
 * at once — detents an interrupt counted since the last loop — are ONE change and one
 * `Changed` with their sum as `delta` (§5.3; DEVICE-02 N11). A step past a limit
 * reaches it; at the limit nothing changes and nothing is said, or with `Wrap` the step
 * goes to the other limit (§5.2; DEVICE-02 S2). Returns true when `Value` changed.
 * Nothing changes on an endpoint with `Enabled = 0` — it does not react. */
bool lb_value_input_step(lb_value_input_t *vi, int32_t steps);

/** Stores `Value` and `ChangeCount` once they have stood still for
 * `LB_VI_STORE_AFTER_MS`. Call it in the loop. */
void lb_value_input_tick(lb_value_input_t *vi, uint32_t now_ms);

/** The current `Value`, in ‰. */
uint16_t lb_value_input_value(const lb_value_input_t *vi);

/** The range: `MinValue`, `MaxValue` (0 and 1000 when the endpoint lacks the rows). */
void lb_value_input_range(const lb_value_input_t *vi, uint16_t *min, uint16_t *max);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_VALUE_INPUT_H */
