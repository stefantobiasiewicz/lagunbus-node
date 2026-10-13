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

/** System endpoints (ep0–ep2) have the full common set, but two of its values are
 * fixed by the specification (01-model-danych.md §4.1). */
static bool is_system(const lb_endpoint_t *endpoint)
{
    return endpoint->number < LB_EP_FIRST_USER;
}

bool lb_endpoint_common_setting(uint8_t element)
{
    switch (element) {
    case LB_COMMON_ENABLED:
    case LB_COMMON_QUERYENABLED:
    case LB_COMMON_REPORTINTERVAL:
    case LB_COMMON_REPORTMINGAP:
    case LB_COMMON_REPORTHYSTERESIS:
        return true;
    default:
        return false;
    }
}

/** The common attributes are read from struct fields, not from the array.
 *
 * Keeping them next to the ordinary attributes would mean two descriptions of the
 * same thing — `TypeId` in the array and `type_id` in the struct — with every
 * opportunity to drift apart. Until TASK-01 the output's `ReportMinGap` was exactly
 * that: a row in the array that only `output.c` read.
 */
static bool is_common(const lb_endpoint_t *endpoint, uint8_t element,
                    lb_type_t *type, lb_value_t *value)
{
    const lb_common_t *common = &endpoint->common;
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
    case LB_COMMON_ENABLED:
        *type = LB_T_BOOL;
        *value = LB_NUM(common->disabled ? 0 : 1);
        return true;
    case LB_COMMON_QUERYENABLED:
        *type = LB_T_MAP16;
        *value = LB_NUM(common->query_enabled);
        return true;
    case LB_COMMON_REPORTINTERVAL:
        *type = LB_T_U16;
        *value = LB_NUM(common->report_interval);
        return true;
    case LB_COMMON_REPORTMINGAP:
        *type = LB_T_U16;
        *value = LB_NUM(common->report_min_gap);
        return true;
    case LB_COMMON_REPORTHYSTERESIS:
        *type = LB_T_U16;
        *value = LB_NUM(common->report_hysteresis);
        return true;
    default:
        return false;
    }
}

/** `UNSUPPORTED_FEATURE` for an element of the type outside the declared features.
 *
 * Checked before the array, so the answer is the same whether or not the device
 * happens to list the attribute: what decides is the declaration, not the array.
 */
static uint8_t feature_gate(const lb_endpoint_t *endpoint, uint8_t element)
{
    if (endpoint->feature_of == NULL) {
        return LB_RESULT_OK;
    }
    uint16_t needs = endpoint->feature_of(element);
    if (needs != 0 && (endpoint->features & needs) == 0) {
        return LB_RESULT_UNSUPPORTED_FEATURE;
    }
    return LB_RESULT_OK;
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

    uint8_t gate = feature_gate(endpoint, element);
    if (gate != LB_RESULT_OK) {
        return gate;
    }
    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!(attr->access & LB_RO)) {
        /* The attribute exists but is write-only — a different thing from "absent". */
        return LB_RESULT_WRITE_ONLY;
    }
    if (attr->type == LB_T_STRING) {
        /* An announcement, not the value: the node sends the bytes as segments right
         * after the reply (04-interakcje.md §3). */
        return endpoint->on_read_long != NULL ? LB_RESULT_SEGMENTED : LB_RESULT_FAILURE;
    }
    *len = lb_encode(attr->type, attr->value, out, cap);
    return *len ? LB_RESULT_OK : LB_RESULT_FAILURE;
}

uint8_t lb_endpoint_read_long(lb_endpoint_t *endpoint, uint8_t element,
                              uint8_t *out, uint8_t cap, uint8_t *len)
{
    uint8_t scratch[1];
    uint8_t ignored = 0;
    uint8_t verdict = lb_endpoint_read(endpoint, element, scratch, sizeof scratch, &ignored);
    if (verdict != LB_RESULT_SEGMENTED) {
        return verdict == LB_RESULT_OK ? LB_RESULT_FAILURE : verdict;
    }
    *len = 0;
    return endpoint->on_read_long(endpoint, element, out, cap, len);
}

/** Write a common setting. `TypeId` and friends describe the device and cannot be
 * changed from outside.
 *
 * On a system endpoint `Enabled` accepts 1, which changes nothing, and refuses 0 with
 * `INVALID_VALUE` — the attribute is writable, just not to that value: a disabled
 * `Device` would cut the hub off from the node's identity, a disabled `Diagnostics` from
 * noticing that it has gone quiet. `QueryEnabled` the same: the system types declare no
 * inputs, so the only valid map is 0 (01-model-danych.md §4.1). */
static uint8_t common_write(lb_endpoint_t *endpoint, uint8_t element, lb_type_t type,
                            const uint8_t *data, uint8_t len)
{
    if (!lb_endpoint_common_setting(element)) {
        return LB_RESULT_READ_ONLY;
    }
    lb_value_t v;
    if (!lb_decode(type, data, len, &v) || v.absent || !lb_in_range(type, v)) {
        return LB_RESULT_INVALID_VALUE;
    }
    if (is_system(endpoint)
        && ((element == LB_COMMON_ENABLED && v.value == 0)
            || (element == LB_COMMON_QUERYENABLED && v.value != 0))) {
        return LB_RESULT_INVALID_VALUE;
    }
    lb_common_t *common = &endpoint->common;
    uint16_t value = (uint16_t)v.value;
    switch (element) {
    case LB_COMMON_ENABLED:          common->disabled = value == 0; break;
    case LB_COMMON_QUERYENABLED:     common->query_enabled = value; break;
    case LB_COMMON_REPORTINTERVAL:   common->report_interval = value; break;
    case LB_COMMON_REPORTMINGAP:     common->report_min_gap = value; break;
    default:                         common->report_hysteresis = value; break;
    }
    return LB_RESULT_OK;
}

/** The write itself. `restoring` is the value coming back from the persistent store:
 * no access check, no `on_write` (`lb_endpoint_restore` says why). */
static uint8_t write_value(lb_endpoint_t *endpoint, uint8_t element,
                           const uint8_t *data, uint8_t len, bool restoring)
{
    if (endpoint == NULL || data == NULL) {
        return LB_RESULT_FAILURE;
    }
    lb_type_t type;
    lb_value_t irrelevant;
    if (is_common(endpoint, element, &type, &irrelevant)) {
        return common_write(endpoint, element, type, data, len);
    }

    uint8_t gate = feature_gate(endpoint, element);
    if (gate != LB_RESULT_OK) {
        return gate;
    }
    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!restoring && !(attr->access & LB_WO)) {
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
    if (endpoint->on_check != NULL) {
        uint8_t verdict = endpoint->on_check(endpoint, element, value);
        if (verdict != LB_RESULT_OK) {
            return verdict; /* refused whole — the old value stays */
        }
    }

    attr->value = value;
    if (!restoring && endpoint->on_write != NULL) {
        return endpoint->on_write(endpoint, element, value);
    }
    return LB_RESULT_OK;
}

uint8_t lb_endpoint_write(lb_endpoint_t *endpoint, uint8_t element,
                          const uint8_t *data, uint8_t len)
{
    return write_value(endpoint, element, data, len, false);
}

uint8_t lb_endpoint_write_long(lb_endpoint_t *endpoint, uint8_t element,
                               const uint8_t *value, uint8_t len)
{
    if (endpoint == NULL || value == NULL) {
        return LB_RESULT_FAILURE;
    }
    lb_type_t type;
    lb_value_t irrelevant;
    if (is_common(endpoint, element, &type, &irrelevant)) {
        /* Every common attribute is a scalar: the settable ones refuse the form, the
         * description refuses the write itself. */
        return lb_endpoint_common_setting(element) ? LB_RESULT_INVALID_VALUE
                                                   : LB_RESULT_READ_ONLY;
    }
    uint8_t gate = feature_gate(endpoint, element);
    if (gate != LB_RESULT_OK) {
        return gate;
    }
    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    if (!(attr->access & LB_WO)) {
        return LB_RESULT_READ_ONLY;
    }
    if (attr->type != LB_T_STRING || !lb_string_whole(value, len)) {
        return LB_RESULT_INVALID_VALUE;
    }
    return endpoint->on_write_long != NULL ? endpoint->on_write_long(endpoint, element, value, len)
                                           : LB_RESULT_FAILURE;
}

uint8_t lb_endpoint_restore(lb_endpoint_t *endpoint, uint8_t element,
                            const uint8_t *data, uint8_t len)
{
    return write_value(endpoint, element, data, len, true);
}

/** The value as a number on the line, for hysteresis: `u32` and `map32` travel in an
 * `int32_t` and would turn negative above 2^31. */
static int64_t numeric(lb_type_t type, lb_value_t value)
{
    if (type == LB_T_U32 || type == LB_T_MAP32) {
        return (int64_t)(uint32_t)value.value;
    }
    return value.value;
}

/** Is `fresh` a change worth a report against what was last reported?
 * (01-model-danych.md §6.2.) "Absent" coming or going is always one — a sensor that
 * failed is news. With `LB_HYST` a number has to move by `ReportHysteresis`, and a
 * hysteresis of 0 means any change at all; without it any change counts. */
static bool changed(const lb_endpoint_t *endpoint, const lb_attr_t *attr,
                    const lb_report_t *slot, lb_value_t fresh)
{
    if (fresh.absent || slot->reported.absent) {
        return fresh.absent != slot->reported.absent;
    }
    int64_t diff = numeric(attr->type, fresh) - numeric(attr->type, slot->reported);
    if (diff == 0) {
        return false;
    }
    if (!(attr->access & LB_HYST)) {
        return true;
    }
    int64_t magnitude = diff < 0 ? -diff : diff;
    return magnitude >= (int64_t)endpoint->common.report_hysteresis;
}

bool lb_endpoint_set(lb_endpoint_t *endpoint, uint8_t element, lb_value_t value)
{
    lb_attr_t *attr = lb_endpoint_attr(endpoint, element);
    if (attr == NULL) {
        return false;
    }
    attr->value = value;
    if (!(attr->access & LB_REPORTED)) {
        return true;
    }
    lb_report_t *slot = lb_endpoint_report(endpoint, element, true);
    if (slot == NULL) {
        return true; /* more reported attributes than `LB_REPORTED_MAX` (endpoint.h) */
    }
    /* `due` stays once set, even when the value comes back before the round goes out:
     * the held-back report then carries the CURRENT value, and a hub that saw nothing
     * change learns nothing wrong. Until TASK-01 `output.c` worked the same way. */
    if (slot->baseline ? changed(endpoint, attr, slot, value) : !value.absent) {
        slot->due = true;
    }
    return true;
}

lb_report_t *lb_endpoint_report(lb_endpoint_t *endpoint, uint8_t element, bool create)
{
    lb_report_t *free_slot = NULL;
    for (unsigned i = 0; i < LB_REPORTED_MAX; i++) {
        lb_report_t *slot = &endpoint->reports[i];
        if (slot->used && slot->id == element) {
            return slot;
        }
        if (!slot->used && free_slot == NULL) {
            free_slot = slot;
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    *free_slot = (lb_report_t){.id = element, .used = true, .reported = LB_ABSENT};
    return free_slot;
}
