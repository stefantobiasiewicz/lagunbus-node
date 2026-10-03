/* The status LED tool: the patterns, and that the pin is written on change only. */
#include "lagunbus/tools/led.h"
#include "lb_test.h"

#include <string.h>

static int writes = 0;
static bool lit = false;
static uint8_t rgb[3];

static void m_set(void *ctx, bool on)
{
    (void)ctx;
    writes++;
    lit = on;
}

static void m_rgb(void *ctx, uint8_t r, uint8_t g, uint8_t b)
{
    (void)ctx;
    writes++;
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
}

int main(void)
{
    LB_TEST("READY: lit, with a short dark blip every 3 s — dark means dead");
    {
        LB_ASSERT(lb_led_lit(LB_STATE_READY, 0));
        LB_ASSERT(lb_led_lit(LB_STATE_READY, 2899));
        LB_ASSERT(!lb_led_lit(LB_STATE_READY, 2900));
        LB_ASSERT(!lb_led_lit(LB_STATE_READY, 2999));
        LB_ASSERT(lb_led_lit(LB_STATE_READY, 3000));
    }

    LB_TEST("NO_ADDRESS: slow blink, 500 ms on, 500 ms off");
    {
        LB_ASSERT(lb_led_lit(LB_STATE_NO_ADDRESS, 0));
        LB_ASSERT(lb_led_lit(LB_STATE_NO_ADDRESS, 499));
        LB_ASSERT(!lb_led_lit(LB_STATE_NO_ADDRESS, 500));
        LB_ASSERT(lb_led_lit(LB_STATE_NO_ADDRESS, 1000));
    }

    LB_TEST("BUS_FAULT: a double flash every second");
    {
        LB_ASSERT(lb_led_lit(LB_STATE_BUS_FAULT, 50));
        LB_ASSERT(!lb_led_lit(LB_STATE_BUS_FAULT, 150));
        LB_ASSERT(lb_led_lit(LB_STATE_BUS_FAULT, 250));
        LB_ASSERT(!lb_led_lit(LB_STATE_BUS_FAULT, 350));
        LB_ASSERT(!lb_led_lit(LB_STATE_BUS_FAULT, 999));
        LB_ASSERT(lb_led_lit(LB_STATE_BUS_FAULT, 1050));
    }

    LB_TEST("IDENTIFY: fast, 100 ms on, 100 ms off — unlike anything else");
    {
        LB_ASSERT(lb_led_lit(LB_STATE_IDENTIFY, 0));
        LB_ASSERT(!lb_led_lit(LB_STATE_IDENTIFY, 100));
        LB_ASSERT(lb_led_lit(LB_STATE_IDENTIFY, 200));
    }

    LB_TEST("RESETTING: dark");
    {
        LB_ASSERT(!lb_led_lit(LB_STATE_RESETTING, 0));
        LB_ASSERT(!lb_led_lit(LB_STATE_RESETTING, 10000));
    }

    LB_TEST("the pattern starts from its beginning when the state changes");
    {
        lb_led_t led;
        lb_led_init(&led, m_set, NULL);
        lb_led_on_state(&led, LB_STATE_NO_ADDRESS);
        lb_led_tick(&led, 10000);
        LB_ASSERT(lit);           /* 0 ms into NO_ADDRESS: on */
        lb_led_tick(&led, 10500);
        LB_ASSERT(!lit);
        lb_led_on_state(&led, LB_STATE_IDENTIFY);
        lb_led_tick(&led, 10550);
        LB_ASSERT(lit);           /* 0 ms into IDENTIFY, not 550 */
    }

    LB_TEST("the pin is written on change only — not on every tick");
    {
        lb_led_t led;
        lb_led_init(&led, m_set, NULL);
        lb_led_on_state(&led, LB_STATE_READY);
        writes = 0;
        for (uint32_t t = 0; t < 2900; t += 2) {
            lb_led_tick(&led, t);
        }
        LB_ASSERT(writes == 1);
        lb_led_tick(&led, 2900);
        LB_ASSERT(writes == 2 && !lit);
    }

    LB_TEST("held past the threshold, the button turns the LED dark over any state");
    {
        lb_led_t led;
        lb_led_init(&led, m_set, NULL);
        lb_led_on_state(&led, LB_STATE_READY);
        lb_led_tick(&led, 0);
        LB_ASSERT(lit);
        lb_led_release(&led, true);
        lb_led_tick(&led, 10);
        LB_ASSERT(!lit);
        lb_led_release(&led, false);
        lb_led_tick(&led, 20);
        LB_ASSERT(lit);
    }

    LB_TEST("RGB: the same timing, a colour per state");
    {
        lb_led_t led;
        lb_led_init_rgb(&led, m_rgb, NULL);
        lb_led_on_state(&led, LB_STATE_READY);
        lb_led_tick(&led, 0);
        LB_ASSERT(rgb[0] == 0 && rgb[1] == 255 && rgb[2] == 0);
        lb_led_on_state(&led, LB_STATE_BUS_FAULT);
        lb_led_tick(&led, 1);
        LB_ASSERT(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0);
        lb_led_tick(&led, 151);
        LB_ASSERT(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
        lb_led_on_state(&led, LB_STATE_NO_ADDRESS);
        lb_led_tick(&led, 200);
        LB_ASSERT(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 255);
        lb_led_on_state(&led, LB_STATE_IDENTIFY);
        lb_led_tick(&led, 300);
        LB_ASSERT(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);
    }

    LB_SUMMARY();
}
