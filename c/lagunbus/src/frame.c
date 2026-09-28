#include "lagunbus/frame.h"

bool lb_encode_id(uint8_t kind, uint16_t address, uint8_t endpoint, uint8_t element,
                  uint32_t *out)
{
    /* We check EVERY field instead of truncating. A truncated address would land on
     * somebody else's node and switch on the wrong light — and the bug would only
     * become visible at that point. */
    if (kind > LB_KIND_MASK || address > LB_ADDRESS_MASK || endpoint > LB_ENDPOINT_MASK) {
        return false;
    }
    if (out == NULL) {
        return false;
    }
    *out = ((uint32_t)kind << LB_KIND_SHIFT)
         | ((uint32_t)address << LB_ADDRESS_SHIFT)
         | ((uint32_t)endpoint << LB_ENDPOINT_SHIFT)
         | (uint32_t)element;
    return true;
}

lb_fields_t lb_decode_id(uint32_t can_id)
{
    lb_fields_t out;
    out.kind = (uint8_t)((can_id >> LB_KIND_SHIFT) & LB_KIND_MASK);
    out.address = (uint16_t)((can_id >> LB_ADDRESS_SHIFT) & LB_ADDRESS_MASK);
    out.endpoint = (uint8_t)((can_id >> LB_ENDPOINT_SHIFT) & LB_ENDPOINT_MASK);
    out.element = (uint8_t)(can_id & LB_ELEMENT_MASK);
    return out;
}

bool lb_is_join(uint32_t can_id)
{
    return ((can_id >> LB_KIND_SHIFT) & LB_KIND_MASK) == LB_JOIN_KIND;
}

uint32_t lb_join_id(uint32_t short_id, bool announce)
{
    return ((uint32_t)LB_JOIN_KIND << LB_KIND_SHIFT)
         | ((uint32_t)(announce ? 1u : 0u) << LB_JOIN_DIRECTION_SHIFT)
         | (short_id & LB_JOIN_SHORT_ID_MASK);
}

uint32_t lb_join_short_id(uint32_t can_id)
{
    return can_id & LB_JOIN_SHORT_ID_MASK;
}

uint8_t lb_encode_header(uint8_t tid, uint8_t result)
{
    return (uint8_t)(((result & LB_RESULT_MASK) << LB_RESULT_SHIFT) | (tid & LB_TID_MASK));
}

void lb_decode_header(uint8_t header, uint8_t *tid, uint8_t *result)
{
    if (tid != NULL) {
        *tid = (uint8_t)(header & LB_TID_MASK);
    }
    if (result != NULL) {
        *result = (uint8_t)((header >> LB_RESULT_SHIFT) & LB_RESULT_MASK);
    }
}
