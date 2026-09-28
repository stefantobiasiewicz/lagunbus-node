/* Port interfaces — a check that the library builds WITHOUT HARDWARE.
 *
 * It looks like a test about nothing, yet it guards the thing that is easiest to
 * break: that nothing in `c/lagunbus/` reaches for ESP-IDF or for a socket.
 * The first
 * `#include "esp_log.h"` put in "for now" breaks this build, not the build
 * on a board — so it shows up at once, not in three weeks.
 */
#include "lagunbus/platform.h"
#include "lagunbus/transport.h"
#include "lb_test.h"

/* A pretend port: just enough to show that the interface suffices. */
typedef struct {
    lb_frame_t queue[4];
    uint8_t    how_many;
    uint32_t   clock;
} mock_t;

static bool mock_send(void *ctx, const lb_frame_t *frame)
{
    mock_t *a = (mock_t *)ctx;
    if (a->how_many >= 4) {
        return false; /* queue full — this is NOT an error */
    }
    a->queue[a->how_many++] = *frame;
    return true;
}

static bool mock_recv(void *ctx, lb_frame_t *out)
{
    mock_t *a = (mock_t *)ctx;
    if (a->how_many == 0) {
        return false; /* silence — not an error either */
    }
    *out = a->queue[--a->how_many];
    return true;
}

static uint32_t mock_millis(void *ctx)
{
    return ((mock_t *)ctx)->clock;
}

int main(void)
{
    mock_t state = {0};
    lb_transport_t bus = {.send = mock_send, .recv = mock_recv, .ctx = &state};
    lb_platform_t system = {.millis = mock_millis, .ctx = &state};

    printf("port interfaces\n");

    LB_TEST("a port can be assembled from four pointers and a context");
    {
        lb_frame_t frame = {0x02048403u, {0x2C, 0x01}, 2};
        LB_ASSERT(bus.send(bus.ctx, &frame));
        LB_ASSERT(state.how_many == 1);

        lb_frame_t received_frame = {0};
        LB_ASSERT(bus.recv(bus.ctx, &received_frame));
        LB_EQ_U32(received_frame.can_id, 0x02048403u);
        LB_ASSERT(received_frame.len == 2);
    }

    LB_TEST("a full queue is a refusal, not a wait");
    {
        lb_frame_t frame = {0x01u, {0}, 0};
        for (int i = 0; i < 4; i++) {
            LB_ASSERT(bus.send(bus.ctx, &frame));
        }
        LB_ASSERT(!bus.send(bus.ctx, &frame));
    }

    LB_TEST("silence on the bus is not an error");
    {
        lb_frame_t received_frame = {0};
        while (bus.recv(bus.ctx, &received_frame)) { }
        LB_ASSERT(!bus.recv(bus.ctx, &received_frame));
    }

    LB_TEST("the clock comes from the platform, not from time.h");
    {
        state.clock = 1234;
        LB_ASSERT(system.millis(system.ctx) == 1234);
    }

    LB_SUMMARY();
}
