/* The service button tool: hold, release, and no reset from a button stuck at start. */
#include "lagunbus/tools/service.h"
#include "lb_test.h"

int main(void)
{
    LB_TEST("held 5 s and released: RESET — on the release, not at 5 s");
    {
        lb_service_t s;
        lb_service_init(&s, LB_SERVICE_HOLD_MS);
        LB_ASSERT(lb_service_update(&s, false, 0) == LB_SERVICE_IDLE);
        LB_ASSERT(lb_service_update(&s, true, 100) == LB_SERVICE_IDLE);
        LB_ASSERT(lb_service_update(&s, true, 5099) == LB_SERVICE_IDLE);
        LB_ASSERT(lb_service_update(&s, true, 5100) == LB_SERVICE_HELD);
        LB_ASSERT(lb_service_update(&s, true, 9000) == LB_SERVICE_HELD);
        LB_ASSERT(lb_service_update(&s, false, 9010) == LB_SERVICE_RESET);
        LB_ASSERT(lb_service_update(&s, false, 9020) == LB_SERVICE_IDLE);
    }

    LB_TEST("released too early: nothing");
    {
        lb_service_t s;
        lb_service_init(&s, LB_SERVICE_HOLD_MS);
        lb_service_update(&s, false, 0);
        lb_service_update(&s, true, 100);
        LB_ASSERT(lb_service_update(&s, false, 5000) == LB_SERVICE_IDLE);
    }

    LB_TEST("pressed at start-up: not armed until it has been seen released");
    {
        lb_service_t s;
        lb_service_init(&s, LB_SERVICE_HOLD_MS);
        /* read every 10 ms, as a board's loop does: held for a minute from power-on */
        for (uint32_t t = 0; t <= 60000; t += 10) {
            LB_ASSERT(lb_service_update(&s, true, t) == LB_SERVICE_IDLE);
        }
        LB_ASSERT(lb_service_update(&s, false, 60010) == LB_SERVICE_IDLE);
        /* and from now on it works */
        lb_service_update(&s, true, 61000);
        lb_service_update(&s, true, 66000);
        LB_ASSERT(lb_service_update(&s, false, 66010) == LB_SERVICE_RESET);
    }

    LB_TEST("the clock wrapping during a hold changes nothing");
    {
        lb_service_t s;
        lb_service_init(&s, LB_SERVICE_HOLD_MS);
        lb_service_update(&s, false, 0xFFFFF000u);
        lb_service_update(&s, true, 0xFFFFF000u);
        LB_ASSERT(lb_service_update(&s, true, 0x00000388u) == LB_SERVICE_HELD);
    }

    LB_SUMMARY();
}
