/* The segment-number tool: rounding, the minus, the half digit, dashes — on the two
 * layouts of the e-paper `epd` (zadania/zrobione/DEVICE-04.md, N1). */
#include "lagunbus/tools/digits.h"
#include "lb_test.h"

#include <string.h>

/* "188.8" — the temperature: a half digit "1", two digits, one decimal, a minus. */
static const lb_digits_t TEMPERATURE = {.whole = 2, .leading_one = true, .decimals = 1,
                                        .minus = true};
/* "88.8" — the humidity: no half digit, no minus. */
static const lb_digits_t HUMIDITY = {.whole = 2, .leading_one = false, .decimals = 1,
                                     .minus = false};

static char out[LB_DIGITS_MAX + 1];

/** Formats `hundredths` and checks both the answer and the characters. */
static bool shows(const lb_digits_t *layout, int32_t hundredths, const char *expected,
                  bool fits)
{
    memset(out, 'X', sizeof out);
    bool got = lb_digits_format(layout, LB_NUM(hundredths), 2, out);
    if (got != fits || strcmp(out, expected) != 0) {
        printf("    %ld → \"%s\" (%s), expected \"%s\" (%s)\n", (long)hundredths, out,
               got ? "fits" : "dashes", expected, fits ? "fits" : "dashes");
        return false;
    }
    return true;
}

int main(void)
{
    LB_TEST("width: half digit, whole digits, the point, decimals");
    {
        LB_EQ_U32(lb_digits_width(&TEMPERATURE), 5);
        LB_EQ_U32(lb_digits_width(&HUMIDITY), 4);
        lb_digits_t whole_only = {.whole = 3};
        LB_EQ_U32(lb_digits_width(&whole_only), 3);
    }

    LB_TEST("an ordinary room: leading zeros blank, the one before the point stays");
    {
        LB_ASSERT(shows(&TEMPERATURE, 2137, " 21.4", true));
        LB_ASSERT(shows(&TEMPERATURE, 512, "  5.1", true));
        LB_ASSERT(shows(&TEMPERATURE, 4, "  0.0", true));
        LB_ASSERT(shows(&HUMIDITY, 4550, "45.5", true));
        LB_ASSERT(shows(&HUMIDITY, 0, " 0.0", true));
    }

    LB_TEST("rounding to tenths, halves away from zero");
    {
        LB_ASSERT(shows(&TEMPERATURE, 2145, " 21.5", true));
        LB_ASSERT(shows(&TEMPERATURE, 2144, " 21.4", true));
        LB_ASSERT(shows(&TEMPERATURE, -5, " -0.1", true));
        LB_ASSERT(shows(&TEMPERATURE, -105, " -1.1", true));
        LB_ASSERT(shows(&HUMIDITY, 9994, "99.9", true));
    }

    LB_TEST("a value that rounds to zero has no minus");
    {
        LB_ASSERT(shows(&TEMPERATURE, -4, "  0.0", true));
    }

    LB_TEST("the minus takes the place right before the first digit");
    {
        LB_ASSERT(shows(&TEMPERATURE, -990, " -9.9", true));
        LB_ASSERT(shows(&TEMPERATURE, -50, " -0.5", true));
    }

    LB_TEST("the half digit shows 1 only: 100.0 … 199.9 fit, 200.0 does not");
    {
        LB_ASSERT(shows(&TEMPERATURE, 10000, "100.0", true));
        LB_ASSERT(shows(&TEMPERATURE, 19990, "199.9", true));
        LB_ASSERT(shows(&TEMPERATURE, 19994, "199.9", true));
        LB_ASSERT(shows(&TEMPERATURE, 19995, " --.-", false)); /* rounds to 200.0 */
        LB_ASSERT(shows(&TEMPERATURE, 25000, " --.-", false));
    }

    LB_TEST("a minus never goes into the half digit: −10.0 does not fit");
    {
        LB_ASSERT(shows(&TEMPERATURE, -994, " -9.9", true));
        LB_ASSERT(shows(&TEMPERATURE, -995, " --.-", false)); /* rounds to −10.0 */
        LB_ASSERT(shows(&TEMPERATURE, -2000, " --.-", false));
    }

    LB_TEST("no half digit: 100 % does not fit, and no minus at all");
    {
        LB_ASSERT(shows(&HUMIDITY, 9995, "--.-", false));
        LB_ASSERT(shows(&HUMIDITY, 10000, "--.-", false));
        LB_ASSERT(shows(&HUMIDITY, -10, "--.-", false));
        LB_ASSERT(shows(&HUMIDITY, -4, " 0.0", true)); /* rounds to zero: no minus needed */
    }

    LB_TEST("absent: dashes");
    {
        memset(out, 'X', sizeof out);
        LB_ASSERT(!lb_digits_format(&TEMPERATURE, LB_ABSENT, 2, out));
        LB_ASSERT(strcmp(out, " --.-") == 0);
    }

    LB_TEST("the extremes of int32 do not overflow");
    {
        LB_ASSERT(shows(&TEMPERATURE, INT32_MAX, " --.-", false));
        LB_ASSERT(shows(&TEMPERATURE, INT32_MIN, " --.-", false));
    }

    LB_TEST("other scales: a value with fewer decimals than the layout, none at all");
    {
        memset(out, 'X', sizeof out);
        LB_ASSERT(lb_digits_format(&HUMIDITY, LB_NUM(45), 0, out));
        LB_ASSERT(strcmp(out, "45.0") == 0);
        lb_digits_t whole_only = {.whole = 3};
        memset(out, 'X', sizeof out);
        LB_ASSERT(lb_digits_format(&whole_only, LB_NUM(4250), 2, out));
        LB_ASSERT(strcmp(out, " 43") == 0);
    }

    LB_TEST("a layout too wide for the buffer is all dashes, never an overrun");
    {
        lb_digits_t huge = {.whole = 20, .decimals = 5};
        LB_EQ_U32(lb_digits_width(&huge), 0);
        out[0] = 'X';
        LB_ASSERT(!lb_digits_format(&huge, LB_NUM(1), 2, out));
        LB_ASSERT(out[0] == '\0');
    }

    LB_SUMMARY();
}
