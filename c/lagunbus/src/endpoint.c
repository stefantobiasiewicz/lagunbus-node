#include "lagunbus/endpoint.h"

#include "lagunbus/registry.h"

lb_attr_t *lb_endpoint_attr(lb_endpoint_t *endpoint, uint8_t element)
{
    if (endpoint == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < endpoint->attr_count; i++) {
        if (endpoint->attrs[i].id == element) {
            return &endpoint->attrs[i];
        }
    }
    return NULL;
}

/** The common attributes are read from struct fields, not from the array.
 *
 * Keeping them next to the ordinary attributes would mean two descriptions of the
 * same thing — `TypeId` in the array and `type_id` in the struct — with every
 * opportunity to drift apart.
 */
static bool is_common(const lb_endpoint_t *endpoint, uint8_t element,
                    lb_type_t *type, lb_value_t *value)
{
    switch (element) {
    case LB_COMMON_TYPEID:
        *type = LB_T_U16;
        *value = LB_NUM(endpoint->type_id);
        return true;
    case LB_COMMON_TYPEREVISION:
        *type = LB_T_U8;
        *value = LB_NUM(endpoint->revision);
        return true;
    case LB_COMMON_FEATURES:
        *type = LB_T_MAP16;
        *value = LB_NUM(endpoint->features);
        return true;
    case LB_COMMON_CHANNEL:
        *type = LB_T_U8;
        *value = LB_NUM(endpoint->channel);
        return true;
    default:
        return false;
    }
}

uint8_t lb_endpoint_read(lb_endpoint_t *endpoint, uint8_t element,
                         uint8_t *out, uint8_t cap, uint8_t *len)
{
    if (endpoint == NULL || out == NULL || len == NULL) {
        return LB_RESULT_FAILURE;
    }
    *len = 0;

    lb_type_t type;
    lb_value_t value;
    if (is_common(endpoint, element, &type, &value)) {
        *len = lb_encode(type, value, out, cap);
        return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
    }

    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!(attr->access & LB_RO)) {
        /* The attribute exists but is write-only — a different thing from "absent". */
        return LB_RESULT_WRITE_ONLY;
    }
    *len = lb_encode(attr->type, attr->value, out, cap);
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

uint8_t lb_endpoint_write(lb_endpoint_t *endpoint, uint8_t element,
                          const uint8_t *data, uint8_t len)
{
    if (endpoint == NULL || data == NULL) {
        return LB_RESULT_FAILURE;
    }
    lb_type_t type;
    lb_value_t irrelevant;
    if (is_common(endpoint, element, &type, &irrelevant)) {
        /* TypeId and friends describe the device and cannot be changed from outside. */
        return LB_RESULT_READ_ONLY;
    }

    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!(attr->access & LB_WO)) {
        return LB_RESULT_READ_ONLY;
    }

    lb_value_t value;
    if (!lb_decode(attr->type, data, len, &value)) {
        /* A wrong length is a wrong value — the node does not guess what the sender
         * meant. */
        return LB_RESULT_INVALID_VALUE;
    }
    if (!lb_in_range(attr->type, value)) {
        return LB_RESULT_INVALID_VALUE;
    }

    attr->value = value;
    if (endpoint->on_write != NULL) {
        return endpoint->on_write(endpoint, element, value);
    }
    return LB_RESULT_OK;
}
