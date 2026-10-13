#include "lagunbus/types/display.h"

#include "lagunbus/registry.h"

#include <string.h>

static lb_attr_t *attr(const lb_display_t *field, uint8_t element)
{
    return lb_endpoint_attr(field->endpoint, element);
}

/** The expiry in seconds: the hub's `ContentTimeout`, or the device's own when the device
 * does not expose it (types/0x0022-display.md §5.5). */
static uint32_t timeout_s(const lb_display_t *field)
{
    const lb_attr_t *a = attr(field, LB_DSP_CONTENT_TIMEOUT);
    if (a == NULL || a->value.absent) {
        return field->own_timeout_s;
    }
    return (uint32_t)a->value.value;
}

/** Say what the field shows now: `Content` (reported when it changes), the expiry counted
 * from now, and the device told to draw. Every write of content comes through here —
 * the same value again included, which is what renews it (§5.5). */
static void shows(lb_display_t *field, uint8_t content)
{
    lb_endpoint_set(field->endpoint, LB_DSP_CONTENT, LB_NUM(content));
    field->written_ms = field->node->platform->millis(field->node->platform->ctx);
    /* A write handler runs before the reply is sent, so the report leaves before it —
     * as an output's does after a command. */
    lb_node_flush(field->node, field->endpoint);
    if (field->show != NULL) {
        field->show(field->ctx, field);
    }
}

/** Nothing to show: no value, no text. The other attribute of a two-feature field goes
 * with it, so a read never answers with something the screen no longer has (§5.1). */
static void forget(lb_display_t *field)
{
    lb_attr_t *value = attr(field, LB_DSP_VALUE);
    if (value != NULL) {
        value->value = LB_ABSENT;
    }
    field->text[0] = 0;
}

void lb_display_init(lb_display_t *field, lb_node_t *node, lb_endpoint_t *endpoint,
                     uint16_t own_timeout_s, lb_display_show_fn show, void *ctx)
{
    memset(field, 0, sizeof *field);
    field->node = node;
    field->endpoint = endpoint;
    field->show = show;
    field->ctx = ctx;
    field->own_timeout_s = own_timeout_s < LB_DSP_TIMEOUT_MIN_S ? (uint16_t)LB_DSP_TIMEOUT_MIN_S
                         : own_timeout_s > LB_DSP_TIMEOUT_MAX_S ? (uint16_t)LB_DSP_TIMEOUT_MAX_S
                                                                : own_timeout_s;

    /* Reported, every change — the type's contract, so the type marks it. Set directly:
     * the empty field a node starts with is not a change, and the hub learns of the start
     * from `Booted` (§5.7). */
    lb_attr_t *content = attr(field, LB_DSP_CONTENT);
    if (content != NULL) {
        content->access |= LB_REPORTED;
        content->value = LB_NUM(LB_DSP_SHOWS_NONE);
    }
    /* The wire type of `Value` is the quantity's (§2), never the board's to choose. */
    lb_attr_t *value = attr(field, LB_DSP_VALUE);
    const lb_attr_t *quantity = attr(field, LB_DSP_QUANTITY);
    if (value != NULL && quantity != NULL && !quantity->value.absent) {
        value->type = lb_quantity_type((uint8_t)quantity->value.value);
    }
    forget(field);
    /* Drawn empty at once — an e-paper still shows what it had before the power went, and
     * an old number without "stale" on it is worse than dashes (§5.7). */
    if (show != NULL) {
        show(ctx, field);
    }
}

uint8_t lb_display_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    if (element == LB_DSP_CONTENT_TIMEOUT && !value.absent && value.value != 0
        && (value.value < (int32_t)LB_DSP_TIMEOUT_MIN_S
            || value.value > (int32_t)LB_DSP_TIMEOUT_MAX_S)) {
        return LB_RESULT_INVALID_VALUE;
    }
    if (element == LB_DSP_CONTENT_TIMEOUT && value.absent) {
        return LB_RESULT_INVALID_VALUE; /* a setting, not a measurement: "absent" is no time */
    }
    return LB_RESULT_OK;
}

uint16_t lb_display_feature_of(uint8_t element)
{
    switch (element) {
    case LB_DSP_QUANTITY:
    case LB_DSP_VALUE:
        return LB_DSP_F_VALUE;
    case LB_DSP_TEXT_LENGTH:
    case LB_DSP_TEXT:
        return LB_DSP_F_TEXT;
    default:
        return 0;
    }
}

uint8_t lb_display_write(lb_display_t *field, uint8_t element, lb_value_t value)
{
    if (element != LB_DSP_VALUE) {
        return LB_RESULT_OK; /* `ContentTimeout` is read when it is used */
    }
    /* The value has landed; the text goes. "Absent" clears the field — never a zero on
     * the screen (§5.2). A number the screen cannot fit is still `VALUE`: dashes are the
     * device's drawing of it, not a refusal (§5.4). */
    field->text[0] = 0;
    shows(field, value.absent ? LB_DSP_SHOWS_NONE : LB_DSP_SHOWS_VALUE);
    return LB_RESULT_OK;
}

uint8_t lb_display_write_text(lb_display_t *field, uint8_t element,
                              const uint8_t *value, uint8_t len)
{
    if (element != LB_DSP_TEXT) {
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
    /* `TextLength` counts BYTES, not letters (§5.3): `ż` is two. The hub knows it from
     * enumeration, so a refusal is something it can avoid. */
    const lb_attr_t *capacity = attr(field, LB_DSP_TEXT_LENGTH);
    uint8_t text_len = (uint8_t)(len - LB_STRING_PREFIX);
    if (capacity == NULL || capacity->value.absent || text_len > capacity->value.value
        || len > sizeof field->text) {
        return LB_RESULT_INVALID_VALUE; /* refused whole — what is shown stays */
    }
    forget(field);
    memcpy(field->text, value, len);
    shows(field, text_len == 0 ? LB_DSP_SHOWS_NONE : LB_DSP_SHOWS_TEXT);
    return LB_RESULT_OK;
}

uint8_t lb_display_read_text(const lb_display_t *field, uint8_t element,
                             uint8_t *out, uint8_t cap, uint8_t *len)
{
    uint8_t whole = (uint8_t)(field->text[0] + LB_STRING_PREFIX);
    if (element != LB_DSP_TEXT || cap < whole) {
        return LB_RESULT_FAILURE;
    }
    memcpy(out, field->text, whole);
    *len = whole;
    return LB_RESULT_OK;
}

void lb_display_tick(lb_display_t *field, uint32_t now_ms)
{
    uint32_t limit = timeout_s(field);
    if (limit == 0 || lb_display_content(field) == LB_DSP_SHOWS_NONE
        || now_ms - field->written_ms < limit * 1000u) {
        return;
    }
    forget(field);
    shows(field, LB_DSP_SHOWS_NONE);
}

uint8_t lb_display_content(const lb_display_t *field)
{
    const lb_attr_t *content = attr(field, LB_DSP_CONTENT);
    return content == NULL || content->value.absent ? LB_DSP_SHOWS_NONE
                                                    : (uint8_t)content->value.value;
}

lb_value_t lb_display_value(const lb_display_t *field)
{
    const lb_attr_t *value = attr(field, LB_DSP_VALUE);
    return value == NULL ? LB_ABSENT : value->value;
}

const char *lb_display_text(const lb_display_t *field, uint8_t *len)
{
    *len = field->text[0];
    return (const char *)(field->text + LB_STRING_PREFIX);
}
