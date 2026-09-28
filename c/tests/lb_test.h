/* The smallest possible set of assertions. No dependencies — same as the library.
 *
 * At this number of tests a framework would cost more than it gives: a dependency to
 * fetch, a second way of building, and somebody else's way of printing failures.
 * `ctest` looks at nothing but the exit code anyway.
 */
#ifndef LB_TEST_H
#define LB_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int lb_test_failures = 0;
static const char *lb_test_name = "";

#define LB_TEST(name) \
    lb_test_name = name; \
    printf("  %s\n", name);

#define LB_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            printf("    ✖ %s:%d  %s\n", __FILE__, __LINE__, #condition); \
            lb_test_failures++; \
        } \
    } while (0)

#define LB_EQ_U32(a, b) \
    do { \
        uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b); \
        if (_a != _b) { \
            printf("    ✖ %s:%d  %s\n        got      0x%08X\n        expected 0x%08X\n", \
                   __FILE__, __LINE__, #a, _a, _b); \
            lb_test_failures++; \
        } \
    } while (0)

#define LB_SUMMARY() \
    do { \
        if (lb_test_failures) { \
            printf("\n%d assertions failed\n", lb_test_failures); \
            return EXIT_FAILURE; \
        } \
        printf("\nall passed\n"); \
        return EXIT_SUCCESS; \
    } while (0)

#endif /* LB_TEST_H */
