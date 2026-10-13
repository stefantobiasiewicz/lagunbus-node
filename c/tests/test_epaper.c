/* The e-paper pacer: the clear at start, the gap, the exception for an empty glass, the
 * full refresh every few partial ones (zadania/zrobione/DEVICE-04.md, N3). */
#include "lagunbus/tools/epaper.h"
#include "lb_test.h"

#define GAP 60000u

int main(void)
{
    LB_TEST("the first refresh is FULL and at once, even with nothing to show");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        LB_EQ_U32(lb_epaper_due(&p, 0, false), LB_EPAPER_FULL);
        LB_EQ_U32(lb_epaper_due(&p, 5, true), LB_EPAPER_FULL);
    }

    LB_TEST("the same picture as the glass: no refresh, however long");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        lb_epaper_started(&p, 0, LB_EPAPER_FULL, true);
        LB_EQ_U32(lb_epaper_due(&p, 10 * GAP, false), LB_EPAPER_NONE);
    }

    LB_TEST("a number on the glass: the next change waits for the gap");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        lb_epaper_started(&p, 1000, LB_EPAPER_FULL, true);
        LB_EQ_U32(lb_epaper_due(&p, 1000 + GAP - 1, true), LB_EPAPER_NONE);
        LB_EQ_U32(lb_epaper_due(&p, 1000 + GAP, true), LB_EPAPER_PARTIAL);
    }

    LB_TEST("dashes on the glass: a value goes out at once — start, then the hub writes");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        lb_epaper_started(&p, 0, LB_EPAPER_FULL, false); /* the clear: dashes */
        LB_EQ_U32(lb_epaper_due(&p, 2000, true), LB_EPAPER_PARTIAL);
        lb_epaper_started(&p, 2000, LB_EPAPER_PARTIAL, true);
        LB_EQ_U32(lb_epaper_due(&p, 4000, true), LB_EPAPER_NONE); /* now the gap counts */
    }

    LB_TEST("a number expiring to dashes waits for the gap like any change");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        lb_epaper_started(&p, 0, LB_EPAPER_FULL, true);
        LB_EQ_U32(lb_epaper_due(&p, 30000, true), LB_EPAPER_NONE);
        LB_EQ_U32(lb_epaper_due(&p, GAP, true), LB_EPAPER_PARTIAL);
    }

    LB_TEST("after full_every partial refreshes the next one is FULL, and counting restarts");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 3);
        uint32_t t = 0;
        lb_epaper_started(&p, t, LB_EPAPER_FULL, true);
        for (int i = 0; i < 3; i++) {
            t += GAP;
            LB_EQ_U32(lb_epaper_due(&p, t, true), LB_EPAPER_PARTIAL);
            lb_epaper_started(&p, t, LB_EPAPER_PARTIAL, true);
        }
        t += GAP;
        LB_EQ_U32(lb_epaper_due(&p, t, true), LB_EPAPER_FULL);
        lb_epaper_started(&p, t, LB_EPAPER_FULL, true);
        t += GAP;
        LB_EQ_U32(lb_epaper_due(&p, t, true), LB_EPAPER_PARTIAL);
    }

    LB_TEST("the gap survives the wrap of the millisecond clock");
    {
        lb_epaper_t p;
        lb_epaper_init(&p, GAP, 10);
        uint32_t before = 0xFFFFFFFFu - 1000u;
        lb_epaper_started(&p, before, LB_EPAPER_FULL, true);
        LB_EQ_U32(lb_epaper_due(&p, before + 2000u, true), LB_EPAPER_NONE);
        LB_EQ_U32(lb_epaper_due(&p, before + GAP, true), LB_EPAPER_PARTIAL);
    }

    LB_SUMMARY();
}
