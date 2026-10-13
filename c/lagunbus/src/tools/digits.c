/* A number on a segment display — the rules are in tools/digits.h. */
#include "lagunbus/tools/digits.h"

uint8_t lb_digits_width(const lb_digits_t *layout)
{
    unsigned width = (layout->leading_one ? 1u : 0u) + layout->whole +
                     (layout->decimals ? 1u + layout->decimals : 0u);
    /* A layout that does not fit the buffer has no places at all: `lb_digits_format` then
     * writes an empty string rather than past the end. */
    return width <= LB_DIGITS_MAX ? (uint8_t)width : 0u;
}

/** Dashes in every whole and decimal place; the half digit stays blank, the point lit. */
static void dashes(const lb_digits_t *layout, char *out)
{
    uint8_t at = 0;
    if (layout->leading_one) {
        out[at++] = ' ';
    }
    for (uint8_t i = 0; i < layout->whole; i++) {
        out[at++] = '-';
    }
    if (layout->decimals) {
        out[at++] = '.';
        for (uint8_t i = 0; i < layout->decimals; i++) {
            out[at++] = '-';
        }
    }
    out[at] = '\0';
}

bool lb_digits_format(const lb_digits_t *layout, lb_value_t value, uint8_t scale, char *out)
{
    uint8_t width = lb_digits_width(layout);
    if (width == 0) {
        out[0] = '\0';
        return false;
    }
    if (value.absent) {
        dashes(layout, out);
        return false;
    }

    /* The magnitude in the layout's decimals, rounded half away from zero. int64: an
     * int32 times 10^decimals would overflow, and INT32_MIN has no positive twin. */
    int64_t magnitude = value.value < 0 ? -(int64_t)value.value : (int64_t)value.value;
    for (uint8_t i = scale; i < layout->decimals; i++) {
        magnitude *= 10;
    }
    for (uint8_t i = layout->decimals; i < scale; i++) {
        magnitude = (magnitude + (i + 1 == scale ? 5 : 0)) / 10;
    }
    bool negative = value.value < 0 && magnitude != 0;

    /* The digits, least significant first: decimals, then the whole part — at least one
     * whole digit, the zero before the point. */
    uint8_t digits[LB_DIGITS_MAX * 2];
    uint8_t count = 0;
    int64_t rest = magnitude;
    do {
        digits[count++] = (uint8_t)(rest % 10);
        rest /= 10;
    } while ((rest != 0 || count <= layout->decimals) && count < sizeof digits);
    if (rest != 0) {
        dashes(layout, out); /* more digits than any layout can have */
        return false;
    }
    uint8_t whole_digits = (uint8_t)(count - layout->decimals);

    /* Does it fit? The minus needs a whole place of its own; the half digit takes only a
     * leading "1", never a minus. */
    bool in_whole = whole_digits + (negative ? 1u : 0u) <= layout->whole;
    bool one_in_front = !negative && layout->leading_one &&
                        whole_digits == layout->whole + 1u && digits[count - 1] == 1;
    if ((negative && !layout->minus) || !(in_whole || one_in_front)) {
        dashes(layout, out);
        return false;
    }

    /* Right to left: decimals, the point, the whole digits, the minus, blanks. */
    uint8_t at = width;
    out[at] = '\0';
    uint8_t next = 0;
    for (uint8_t i = 0; i < layout->decimals; i++) {
        out[--at] = (char)('0' + digits[next++]);
    }
    if (layout->decimals) {
        out[--at] = '.';
    }
    while (next < count) {
        out[--at] = (char)('0' + digits[next++]);
    }
    if (negative) {
        out[--at] = '-';
    }
    while (at > 0) {
        out[--at] = ' ';
    }
    return true;
}
