/* The identifier codec — the same vectors as on the hub side.
 *
 * The vectors come from lagunbus-spec/PRZYKLADY.md: worked out by hand from chapter
 * 05 and the only place in the specification where whole frames are visible at once.
 * The hub checks itself against them in tests/test_examples.py — **if both sides pass
 * the same vectors, they cannot drift apart without drifting from the specification.**
 */
#include "lagunbus/frame.h"
#include "lagunbus/registry.h"
#include "lb_test.h"

#include <string.h>

int main(void)
{
    printf("the identifier codec\n");

    LB_TEST("a group command: Recall scene 1 in group 5 (PRZYKLADY §2)");
    {
        uint32_t can_id = 0;
        LB_ASSERT(lb_encode_id(LB_KIND_GROUP_COMMAND, 0x005, 3, 0x01, &can_id));
        LB_EQ_U32(can_id, 0x08014301u);

        lb_fields_t f = lb_decode_id(can_id);
        LB_ASSERT(f.kind == LB_KIND_GROUP_COMMAND);
        LB_ASSERT(f.address == 0x005);
        LB_ASSERT(f.endpoint == 3);
        LB_ASSERT(f.element == 0x01);
    }

    LB_TEST("a command to Groups: hub → 0x020 ep2 (PRZYKLADY §1)");
    {
        uint32_t can_id = 0;
        LB_ASSERT(lb_encode_id(LB_KIND_COMMAND, 0x020, 2, 0x00, &can_id));
        LB_EQ_U32(can_id, 0x06080200u);
    }

    LB_TEST("a reply comes back from THE SAME address, only the kind differs");
    {
        uint32_t task = 0, respond = 0;
        lb_encode_id(LB_KIND_COMMAND, 0x020, 2, 0x00, &task);
        lb_encode_id(LB_KIND_RESPONSE, 0x020, 2, 0x00, &respond);
        LB_EQ_U32(respond, 0x0E080200u);
        /* The mask the hub uses to match a reply against its request. */
        LB_EQ_U32(task & 0x01FFFFFFu, respond & 0x01FFFFFFu);
    }

    LB_TEST("an event from a key: 0x012 ep4 (PRZYKLADY §4)");
    {
        uint32_t can_id = 0;
        LB_ASSERT(lb_encode_id(LB_KIND_EVENT, 0x012, 4, 0x03, &can_id));
        LB_EQ_U32(can_id, 0x02048403u);
    }

    LB_TEST("an identifier never runs past 29 bits");
    {
        uint32_t can_id = 0;
        LB_ASSERT(lb_encode_id(LB_KIND_JOIN, LB_ADDRESS_BROADCAST, LB_ENDPOINT_MAX, 0xFF,
                               &can_id));
        LB_ASSERT((can_id & ~LB_CAN_ID_MASK) == 0);
    }

    LB_TEST("a field out of range is REFUSED, not truncated");
    {
        uint32_t can_id = 0xDEADBEEF;
        LB_ASSERT(!lb_encode_id(0x10, 0, 0, 0, &can_id));          /* kind     > 4 bits  */
        LB_ASSERT(!lb_encode_id(0, 0x800, 0, 0, &can_id));         /* address  > 11 bits */
        LB_ASSERT(!lb_encode_id(0, 0, 64, 0, &can_id));            /* endpoint > 6 bits  */
        LB_EQ_U32(can_id, 0xDEADBEEFu);                            /* untouched */
    }

    printf("\njoining — an identifier layout of its own (05-mapowanie-can.md §4)\n");

    LB_TEST("K=1 is ANNOUNCEMENT, K=0 is ASSIGNMENT — that way round, not the other");
    {
        /* The direction is easy to flip and nobody will notice until the first
         * attempt on a real bus, because both frames look the same. Hence this test,
         * and hence the values written out by hand from the specification
         * (05-mapowanie-can.md §4.2). */
        uint32_t announce_frame = lb_join_id(0xA1B2C3, true);
        uint32_t assignment = lb_join_id(0xA1B2C3, false);
        LB_EQ_U32(announce_frame, 0x1FA1B2C3u);
        LB_EQ_U32(assignment, 0x1EA1B2C3u);
        LB_ASSERT(assignment < announce_frame); /* the assignment wins arbitration */
        LB_ASSERT(lb_is_join(announce_frame) && lb_is_join(assignment));
        LB_EQ_U32(lb_join_short_id(announce_frame), 0xA1B2C3u);
    }

    LB_TEST("an ordinary frame does not pretend to be a join frame");
    {
        uint32_t can_id = 0;
        lb_encode_id(LB_KIND_REPORT, 0x020, 4, 0x00, &can_id);
        LB_ASSERT(!lb_is_join(can_id));
    }

    printf("\nheader byte (05-mapowanie-can.md §5.1)\n");

    LB_TEST("the result and the Tid fit into one byte");
    {
        uint8_t tid = 0, result = 0;
        LB_ASSERT(lb_encode_header(1, LB_RESULT_OK) == 0x01);
        lb_decode_header(lb_encode_header(5, LB_RESULT_BUSY), &tid, &result);
        LB_ASSERT(tid == 5);
        LB_ASSERT(result == LB_RESULT_BUSY);
    }

    LB_TEST("the Tid has three bits and does not trample the result");
    {
        uint8_t tid = 0, result = 0;
        lb_decode_header(lb_encode_header(0xFF, 0x00), &tid, &result);
        LB_ASSERT(tid == 0x07);
        LB_ASSERT(result == 0x00);
    }

    LB_TEST("a pointer may be NULL — sometimes only one of the two matters");
    {
        uint8_t tid = 0;
        lb_decode_header(lb_encode_header(3, LB_RESULT_OK), &tid, NULL);
        LB_ASSERT(tid == 3);
    }

    printf("\nnames from the registry — for watching, not for logic\n");

    LB_TEST("kinds and results have names, and an unknown number says it is unknown");
    {
        LB_ASSERT(strcmp(lb_kind_name(LB_KIND_RESPONSE), "RESPONSE") == 0);
        LB_ASSERT(strcmp(lb_kind_name(LB_KIND_JOIN), "JOIN") == 0);
        LB_ASSERT(strcmp(lb_result_name(LB_RESULT_BUSY), "BUSY") == 0);
        LB_ASSERT(strcmp(lb_result_name(LB_RESULT_SEGMENTED), "SEGMENTED") == 0);
        /* 0x2 is RESERVED in the specification — it has no name and must say so. */
        LB_ASSERT(strcmp(lb_kind_name(0x2), "?") == 0);
        LB_ASSERT(strcmp(lb_result_name(0x1F), "?") == 0);
    }

    LB_SUMMARY();
}
