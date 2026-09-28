/* Value codec — 02-typy-danych.md.
 *
 * Vectors checked against the hub: what `lagunhub.protocol.values` returns
 * for the same inputs must come out byte for byte here.
 */
#include "lagunbus/values.h"
#include "lb_test.h"

#include <string.h>

static bool bytes_are(const uint8_t *a, const char *expected, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch[3] = {expected[i * 2], expected[i * 2 + 1], 0};
        if (a[i] != (uint8_t)strtol(ch, NULL, 16)) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    uint8_t buf[8];
    lb_value_t value;

    printf("little-endian, no exceptions (02-typy-danych.md §1)\n");

    LB_TEST("u16 goes low byte first");
    {
        LB_ASSERT(lb_encode(LB_T_U16, LB_NUM(300), buf, sizeof buf) == 2);
        LB_ASSERT(bytes_are(buf, "2C01", 2));     /* 300 = 0x012C */
        LB_ASSERT(lb_decode(LB_T_U16, buf, 2, &value));
        LB_ASSERT(!value.absent && value.value == 300);
    }

    LB_TEST("u32 the same way");
    {
        LB_ASSERT(lb_encode(LB_T_U32, LB_NUM(0x01020304), buf, sizeof buf) == 4);
        LB_ASSERT(bytes_are(buf, "04030201", 4));
    }

    printf("\nsigned numbers\n");

    LB_TEST("a negative i16 comes back negative — without this -1 would be 65535");
    {
        /* Patterns from the hub: encode('i16', -1) == FFFF, encode('i16', ABSENT) == 0080.
         * Those two values ARE different, and that is the whole point of separating
         * "absent" from the extreme number — for `u16` absent is exactly FFFF. */
        LB_ASSERT(lb_encode(LB_T_I16, LB_NUM(-1), buf, sizeof buf) == 2);
        LB_ASSERT(bytes_are(buf, "FFFF", 2));
        LB_ASSERT(lb_decode(LB_T_I16, buf, 2, &value));
        LB_ASSERT(!value.absent && value.value == -1);
    }

    LB_TEST("the same pattern means different things in i16 and u16");
    {
        lb_value_t u;
        lb_encode(LB_T_U16, LB_ABSENT, buf, sizeof buf);
        LB_ASSERT(bytes_are(buf, "FFFF", 2));
        LB_ASSERT(lb_decode(LB_T_U16, buf, 2, &u) && u.absent);

        LB_ASSERT(lb_decode(LB_T_I16, buf, 2, &u));
        LB_ASSERT(!u.absent && u.value == -1); /* for i16 this is an ordinary -1 */
    }

    LB_TEST("absent in i16 is 0080, not FFFF");
    {
        LB_ASSERT(lb_encode(LB_T_I16, LB_ABSENT, buf, sizeof buf) == 2);
        LB_ASSERT(bytes_are(buf, "0080", 2));
        LB_ASSERT(lb_decode(LB_T_I16, buf, 2, &value) && value.absent);
    }

    LB_TEST("i8 at the extremes");
    {
        LB_ASSERT(lb_encode(LB_T_I8, LB_NUM(-127), buf, sizeof buf) == 1);
        LB_ASSERT(bytes_are(buf, "81", 1));
        LB_ASSERT(lb_decode(LB_T_I8, buf, 1, &value));
        LB_ASSERT(value.value == -127);
        LB_ASSERT(lb_lo(LB_T_I8) == -127);  /* -128 is taken by "absent" */
        LB_ASSERT(lb_hi(LB_T_I8) == 127);
    }

    LB_TEST("i16 right next to the absent pattern");
    {
        LB_ASSERT(lb_encode(LB_T_I16, LB_NUM(-32767), buf, sizeof buf) == 2);
        LB_ASSERT(bytes_are(buf, "0180", 2));
        LB_ASSERT(lb_decode(LB_T_I16, buf, 2, &value));
        LB_ASSERT(!value.absent && value.value == -32767);
    }

    printf("\n\"I do not know\" is a different thing from zero (02-typy-danych.md §1.3)\n");

    LB_TEST("absent encodes as the extreme pattern and comes back as absent");
    {
        LB_ASSERT(lb_encode(LB_T_U8, LB_ABSENT, buf, sizeof buf) == 1);
        LB_ASSERT(buf[0] == 0xFF);
        LB_ASSERT(lb_decode(LB_T_U8, buf, 1, &value));
        LB_ASSERT(value.absent);
    }

    LB_TEST("zero is NOT absent");
    {
        lb_encode(LB_T_U8, LB_NUM(0), buf, sizeof buf);
        LB_ASSERT(buf[0] == 0x00);
        lb_decode(LB_T_U8, buf, 1, &value);
        LB_ASSERT(!value.absent && value.value == 0);
    }

    LB_TEST("a bit field has no absent — every arrangement of bits is valid");
    {
        LB_ASSERT(!lb_has_absent(LB_T_MAP8));
        LB_ASSERT(lb_encode(LB_T_MAP8, LB_ABSENT, buf, sizeof buf) == 0);
        lb_encode(LB_T_MAP8, LB_NUM(0xFF), buf, sizeof buf);
        LB_ASSERT(lb_decode(LB_T_MAP8, buf, 1, &value));
        LB_ASSERT(!value.absent && value.value == 0xFF);
    }

    LB_TEST("a type's range leaves out the absent pattern");
    {
        LB_ASSERT(lb_hi(LB_T_U8) == 254);    /* 255 is taken */
        LB_ASSERT(lb_hi(LB_T_MAP8) == 255);  /* nothing is taken */
    }

    printf("\nrefusal instead of guessing\n");

    LB_TEST("too short a buffer writes nothing at all");
    {
        uint8_t tight[1] = {0xAA};
        LB_ASSERT(lb_encode(LB_T_U32, LB_NUM(1), tight, sizeof tight) == 0);
        LB_ASSERT(tight[0] == 0xAA);
    }

    LB_TEST("a wrong length on decode is a refusal");
    {
        LB_ASSERT(!lb_decode(LB_T_U16, buf, 1, &value));
        LB_ASSERT(!lb_decode(LB_T_U16, buf, 3, &value));
    }

    LB_TEST("a bool accepts only zero and one");
    {
        LB_ASSERT(lb_in_range(LB_T_BOOL, LB_NUM(1)));
        LB_ASSERT(!lb_in_range(LB_T_BOOL, LB_NUM(2)));
        LB_ASSERT(lb_in_range(LB_T_BOOL, LB_ABSENT));
    }

    printf("\nvalues longer than a frame (02-typy-danych.md §2.2)\n");

    LB_TEST("a blob gets a two-byte length prefix");
    {
        const uint8_t uid[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        uint8_t big[16];
        size_t n = lb_encode_blob(uid, sizeof uid, big, sizeof big);
        LB_ASSERT(n == 10);
        LB_ASSERT(big[0] == 8 && big[1] == 0);

        const uint8_t *data = NULL;
        size_t length = 0;
        LB_ASSERT(lb_decode_blob(big, n, &data, &length));
        LB_ASSERT(length == 8 && memcmp(data, uid, 8) == 0);
    }

    LB_TEST("a lying prefix is rejected, not patched up");
    {
        uint8_t broken[4] = {0x10, 0x00, 0xAA, 0xBB}; /* promises 16 B, has 2 */
        const uint8_t *data = NULL;
        size_t length = 0;
        LB_ASSERT(!lb_decode_blob(broken, sizeof broken, &data, &length));
    }

    LB_SUMMARY();
}
