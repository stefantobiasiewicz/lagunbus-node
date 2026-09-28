/* The LagunBus frame and the codec for the 29-bit identifier.
 *
 * The field layout and every constant come from lagunbus-spec/05-mapowanie-can.md.
 * Nothing in this file knows about TWAI or SocketCAN — this is a pure codec, the same
 * one as on the hub side (lagunhub/protocol/frame.py).
 */
#ifndef LAGUNBUS_FRAME_H
#define LAGUNBUS_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- identifier layout, 05-mapowanie-can.md §1 ----------------------------------
 *
 *   28 27 26 25 │ 24 … 14 │ 13 … 8 │ 7 … 0
 *      KIND     │ ADDRESS │   EP   │ ELEM
 *       (4)     │   (11)  │   (6)  │  (8)
 */
#define LB_KIND_SHIFT      25
#define LB_KIND_MASK       0x0Fu
#define LB_ADDRESS_SHIFT   14
#define LB_ADDRESS_MASK    0x7FFu
#define LB_ENDPOINT_SHIFT  8
#define LB_ENDPOINT_MASK   0x3Fu
#define LB_ELEMENT_MASK    0xFFu

/* 29 bits, without a single spare one. */
#define LB_CAN_ID_MASK     0x1FFFFFFFu
#define LB_MAX_PAYLOAD     8

/* --- addresses, 03-adresacja.md §2 --------------------------------------------- */
#define LB_ADDRESS_UNASSIGNED  0x000u
#define LB_ADDRESS_NODE_MIN    0x001u
#define LB_ADDRESS_NODE_MAX    0x7FDu
#define LB_ADDRESS_HUB         0x7FEu
#define LB_ADDRESS_BROADCAST   0x7FFu
#define LB_ENDPOINT_MAX        63u

/* --- the join frame has a layout of its own, 05-mapowanie-can.md §4 -------------
 *
 *   28 27 26 25 │ 24 │ 23 … 0
 *      0xF      │ K  │ ShortId[23:0]
 *
 * Bit K carries the direction: 0 is an announcement from a node, 1 an assignment
 * from the hub.
 */
#define LB_JOIN_KIND             0x0Fu
#define LB_JOIN_DIRECTION_SHIFT  24
#define LB_JOIN_SHORT_ID_MASK    0x00FFFFFFu

/* --- header byte of transactional messages, 05-mapowanie-can.md §5.1 ------------
 *
 *   bit 7 6 5 4 3 │ 2 1 0
 *       RESULT    │  Tid
 */
#define LB_TID_MASK      0x07u
#define LB_RESULT_SHIFT  3
#define LB_RESULT_MASK   0x1Fu

/** A frame exactly as it goes over the wire. */
typedef struct {
    uint32_t can_id;                   /**< 29 bits; the upper ones are always zero. */
    uint8_t  data[LB_MAX_PAYLOAD];
    uint8_t  len;
} lb_frame_t;

/** The identifier fields once taken apart. */
typedef struct {
    uint8_t  kind;
    uint16_t address;
    uint8_t  endpoint;
    uint8_t  element;
} lb_fields_t;

/** Build an identifier. Returns false when any field does not fit into its own. */
bool lb_encode_id(uint8_t kind, uint16_t address, uint8_t endpoint, uint8_t element,
                  uint32_t *out);

/** Take an identifier apart. Checks nothing — any 29 bits can be taken apart. */
lb_fields_t lb_decode_id(uint32_t can_id);

/** Whether this is a join frame. It has a layout of its own, so this is asked BEFORE
 * taking the identifier apart. */
bool lb_is_join(uint32_t can_id);

/** Identifier of a join frame.
 *
 * `announce = true` is an **ANNOUNCEMENT** (node → hub, K=1), `false` an
 * **ASSIGNMENT** (hub → node, K=0). The order is not arbitrary: an assignment has the
 * lower identifier, so in arbitration it goes ahead of further announcements
 * (05-mapowanie-can.md §4.4).
 */
uint32_t lb_join_id(uint32_t short_id, bool announce);

/** `ShortId` out of a join frame. The result is meaningless for a frame that is not
 * one. */
uint32_t lb_join_short_id(uint32_t can_id);

/** The header byte: result and `Tid` in one. */
uint8_t lb_encode_header(uint8_t tid, uint8_t result);

/** Take the header byte apart. Either pointer may be NULL. */
void lb_decode_header(uint8_t header, uint8_t *tid, uint8_t *result);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_FRAME_H */
