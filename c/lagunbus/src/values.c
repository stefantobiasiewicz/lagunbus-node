#include "lagunbus/values.h"

#include <string.h>

typedef struct {
    uint8_t  size;
    bool     is_signed;
    bool     has_absent;
    uint32_t absent;
} lb_spec_t;

/* Table from 02-typy-danych.md §1. The order must match `lb_type_t`. */
static const lb_spec_t SPECS[LB_T_COUNT] = {
    [LB_T_BOOL]   = {1, false, true,  0xFFu},
    [LB_T_U8]     = {1, false, true,  0xFFu},
    [LB_T_U16]    = {2, false, true,  0xFFFFu},
    [LB_T_U32]    = {4, false, true,  0xFFFFFFFFu},
    [LB_T_I8]     = {1, true,  true,  0x80u},
    [LB_T_I16]    = {2, true,  true,  0x8000u},
    [LB_T_I32]    = {4, true,  true,  0x80000000u},
    [LB_T_ENUM8]  = {1, false, true,  0xFFu},
    [LB_T_ENUM16] = {2, false, true,  0xFFFFu},
    [LB_T_MAP8]   = {1, false, false, 0u},
    [LB_T_MAP16]  = {2, false, false, 0u},
    [LB_T_MAP32]  = {4, false, false, 0u},
};

static bool is_known(lb_type_t type)
{
    return type >= 0 && type < LB_T_COUNT && SPECS[type].size != 0;
}

uint8_t lb_size_of(lb_type_t type)
{
    return is_known(type) ? SPECS[type].size : 0u;
}

bool lb_has_absent(lb_type_t type)
{
    return is_known(type) && SPECS[type].has_absent;
}

int32_t lb_lo(lb_type_t type)
{
    if (!is_known(type) || !SPECS[type].is_signed) {
        return 0;
    }
    /* The smallest value is one above the type's range: the extreme is taken by the
     * "absent" pattern (02-typy-danych.md §1.3). */
    int32_t edge = -(int32_t)(1u << (SPECS[type].size * 8u - 1u));
    return edge + 1;
}

int32_t lb_hi(lb_type_t type)
{
    if (!is_known(type)) {
        return 0;
    }
    const lb_spec_t *spec = &SPECS[type];
    if (spec->is_signed) {
        return (int32_t)((1u << (spec->size * 8u - 1u)) - 1u);
    }
    /* u32 does not fit into int32_t — and it does not fit into LagunBus as a signed
     * value either. We clamp to what can actually be carried. */
    if (spec->size == 4u) {
        return INT32_MAX;
    }
    uint32_t full = (1u << (spec->size * 8u)) - 1u;
    return (int32_t)(spec->has_absent ? full - 1u : full);
}

bool lb_in_range(lb_type_t type, lb_value_t value)
{
    if (!is_known(type)) {
        return false;
    }
    if (value.absent) {
        return SPECS[type].has_absent;
    }
    if (type == LB_T_BOOL) {
        return value.value == 0 || value.value == 1;
    }
    return value.value >= lb_lo(type) && value.value <= lb_hi(type);
}

uint8_t lb_encode(lb_type_t type, lb_value_t value, uint8_t *out, size_t cap)
{
    if (!is_known(type) || out == NULL) {
        return 0;
    }
    const lb_spec_t *spec = &SPECS[type];
    if (cap < spec->size) {
        return 0;
    }
    if (value.absent && !spec->has_absent) {
        return 0;
    }
    uint32_t raw = value.absent ? spec->absent : (uint32_t)value.value;
    for (uint8_t i = 0; i < spec->size; i++) {
        out[i] = (uint8_t)((raw >> (8u * i)) & 0xFFu); /* little-endian, 02 §1 */
    }
    return spec->size;
}

bool lb_decode(lb_type_t type, const uint8_t *data, size_t len, lb_value_t *out)
{
    if (!is_known(type) || data == NULL || out == NULL || len != SPECS[type].size) {
        return false;
    }
    const lb_spec_t *spec = &SPECS[type];
    uint32_t raw = 0;
    for (uint8_t i = 0; i < spec->size; i++) {
        raw |= (uint32_t)data[i] << (8u * i);
    }
    if (spec->has_absent && raw == spec->absent) {
        *out = LB_ABSENT;
        return true;
    }
    if (spec->is_signed && spec->size < 4u) {
        /* Sign extension: without it, -1 out of an i16 would come out as 65535. */
        uint32_t bit = 1u << (spec->size * 8u - 1u);
        if (raw & bit) {
            raw |= ~((1u << (spec->size * 8u)) - 1u);
        }
    }
    out->value = (int32_t)raw;
    out->absent = false;
    return true;
}

size_t lb_encode_blob(const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    if (out == NULL || (data == NULL && len > 0) || len > 0xFFFFu) {
        return 0;
    }
    if (cap < len + LB_BLOB_PREFIX) {
        return 0;
    }
    out[0] = (uint8_t)(len & 0xFFu);
    out[1] = (uint8_t)((len >> 8u) & 0xFFu);
    if (len > 0) {
        memcpy(out + LB_BLOB_PREFIX, data, len);
    }
    return len + LB_BLOB_PREFIX;
}

bool lb_decode_blob(const uint8_t *data, size_t len, const uint8_t **out, size_t *out_len)
{
    if (data == NULL || out == NULL || out_len == NULL || len < LB_BLOB_PREFIX) {
        return false;
    }
    size_t announced = (size_t)data[0] | ((size_t)data[1] << 8u);
    if (announced + LB_BLOB_PREFIX != len) {
        return false; /* the prefix lies — better to refuse than to guess */
    }
    *out = data + LB_BLOB_PREFIX;
    *out_len = announced;
    return true;
}
