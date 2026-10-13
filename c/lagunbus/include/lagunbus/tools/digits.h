/* A number on a segment display — a tool, not a type (zadania/zrobione/DEVICE-04.md, N1, N12).
 *
 * A `Display` field with `VALUE` gets a number in its quantity's hundredths; a screen made
 * of seven-segment digits has a fixed number of places for it. What goes into those places
 * — rounding, whether it fits, where the minus stands, dashes when it does not — is the
 * same for every such screen and is worth testing on a computer, so it lives here. Which
 * segments light for a character is the screen's own business (devices/components/epd1in9).
 *
 * The answer is characters, one per place, left to right:
 *
 *     '0'…'9'  a digit
 *     ' '      nothing — a leading zero, or an unused half digit
 *     '-'      a minus, or a dash
 *     '.'      the decimal point, always lit when the layout has decimals
 *
 * The simulator draws the same characters, so `lagun-sim` shows what the glass would.
 *
 * The rules (DEVICE-04, N1; types/0x0022-display.md §5.3):
 *
 *   - rounded to the layout's decimals, halves away from zero (21.45 → 21.5, −0.05 → −0.1);
 *     a value that rounds to zero has no minus;
 *   - a minus stands right before the first digit and takes a whole place — a screen has
 *     no separate minus segment; a half digit can only be "1" and never a minus;
 *   - a value that does not fit, or "absent", is dashes in every whole and decimal place:
 *     the write was accepted, the field just cannot draw it (zadania/zrobione/FEAT-12.md, N12).
 */
#ifndef LAGUNBUS_DIGITS_H
#define LAGUNBUS_DIGITS_H

#include "lagunbus/values.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The longest layout, in places — `out` of `lb_digits_format` holds one more for '\0'. */
#define LB_DIGITS_MAX 12u

/** The places of one number on the glass. "188.8" is {.whole = 2, .leading_one = true,
 * .decimals = 1, .minus = true}: −9.9 … 199.9. */
typedef struct {
    uint8_t whole;       /**< full digits before the point */
    bool    leading_one; /**< a half digit in front of them that can show only "1" */
    uint8_t decimals;    /**< digits after the point; 0 — no point */
    bool    minus;       /**< a whole place can show a minus */
} lb_digits_t;

/** Places in `layout`, the point counted, '\0' not. */
uint8_t lb_digits_width(const lb_digits_t *layout);

/** `value` in units of 10^-`scale` (2 for every quantity a display shows today —
 * hundredths), into `out` (`lb_digits_width` + 1 bytes, '\0'-terminated). True when the
 * number is there, false when the field shows dashes. */
bool lb_digits_format(const lb_digits_t *layout, lb_value_t value, uint8_t scale, char *out);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_DIGITS_H */
