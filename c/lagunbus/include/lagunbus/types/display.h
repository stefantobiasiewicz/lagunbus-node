/* One field of a display — type 0x0022 Display (types/0x0022-display.md).
 *
 * As with the key and the output: what the field shows and when it stops showing it lives
 * here, drawing it lives in the application. The library keeps `Content`, the value, the
 * text and the expiry, and tells the device through `show` whenever any of them changes —
 * the device then draws `lb_display_value` or `lb_display_text`, however its screen does
 * (digits, a font, `?` for a glyph it lacks, dashes for a number too wide; §5.3–5.4).
 *
 * A field is a TARGET: the hub writes, the field shows. Nothing here is measured, so there
 * are no events and no commands (§3–4). The one thing a field does by itself is expire:
 * content the hub has not renewed for `ContentTimeout` is cleared (§5.5).
 *
 * The inputs of §4a (asking the hub for `Value` after a start) are not here: the library
 * has no `QUERY` yet for any type. `QueryEnabled` is stored as for every endpoint and
 * nothing asks — which the type allows (§4a: without an answer the field shows `NONE`).
 */
#ifndef LAGUNBUS_DISPLAY_H
#define LAGUNBUS_DISPLAY_H

#include "lagunbus/node.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_DSP_CONTENT          0x00u
#define LB_DSP_QUANTITY         0x01u
#define LB_DSP_VALUE            0x02u
#define LB_DSP_TEXT_LENGTH      0x03u
#define LB_DSP_TEXT             0x04u
#define LB_DSP_CONTENT_TIMEOUT  0x10u

/** Features (§1). A field has one or both; none is a device error. */
#define LB_DSP_F_VALUE  0x0001u
#define LB_DSP_F_TEXT   0x0002u

/** `Content` (§2.3). */
#define LB_DSP_SHOWS_NONE   0u
#define LB_DSP_SHOWS_VALUE  1u
#define LB_DSP_SHOWS_TEXT   2u

/** `ContentTimeout`: 0 = never, otherwise 60–3600 s; 600 by default (§2.2). A field that
 * does not expose it keeps a time of its own from 60–3600 — never 0 (§5.5). */
#define LB_DSP_TIMEOUT_DEFAULT_S  600u
#define LB_DSP_TIMEOUT_MIN_S      60u
#define LB_DSP_TIMEOUT_MAX_S      3600u

/* --- attribute rows ----------------------------------------------------------------
 * `Content` is always there; the rest by feature. `Quantity` and `TextLength` are the
 * device's declaration — constants, read-only, not stored. `Value` gets its wire type from
 * `Quantity` in `lb_display_init`, so a board cannot give a temperature field a `u16`.
 * `Text` holds nothing in the row: its bytes are in `lb_display_t`. */
#define LB_DSP_ATTR_CONTENT() \
    {LB_DSP_CONTENT, LB_T_ENUM8, LB_RO, LB_NUM(LB_DSP_SHOWS_NONE)}
#define LB_DSP_ATTR_QUANTITY(quantity) \
    {LB_DSP_QUANTITY, LB_T_ENUM8, LB_RO, LB_NUM(quantity)}
#define LB_DSP_ATTR_VALUE() \
    {LB_DSP_VALUE, LB_T_COUNT, LB_RW, LB_ABSENT}
#define LB_DSP_ATTR_TEXT_LENGTH(bytes) \
    {LB_DSP_TEXT_LENGTH, LB_T_U8, LB_RO, LB_NUM(bytes)}
#define LB_DSP_ATTR_TEXT() \
    {LB_DSP_TEXT, LB_T_STRING, LB_RW, LB_ABSENT}
#define LB_DSP_ATTR_CONTENT_TIMEOUT(seconds) \
    {LB_DSP_CONTENT_TIMEOUT, LB_T_U16, LB_RW | LB_PERSIST, LB_NUM(seconds)}

struct lb_display;

/** What the field shows has changed — draw it. Called from inside `lb_node_tick` (a
 * write) or `lb_display_tick` (expiry), so it must not block: an e-paper that takes 1.5 s
 * to refresh marks itself dirty here and refreshes from its own loop. */
typedef void (*lb_display_show_fn)(void *ctx, const struct lb_display *field);

typedef struct lb_display {
    lb_endpoint_t     *endpoint;
    lb_node_t         *node;
    lb_display_show_fn show;
    void              *ctx;

    uint16_t own_timeout_s; /**< the device's own expiry when it has no `ContentTimeout` */
    uint32_t written_ms;    /**< the last write of content — what expiry counts from */

    /** `Text` with its length prefix. Sized by the node's long-write buffer: a text that
     * arrived fits here (zadania/zrobione/FEAT-12.md, N9). */
    uint8_t text[LB_LONG_WRITE_MAX];
} lb_display_t;

/** Prepare the field: empty, `Content = NONE` (§5.7), `Value` typed by `Quantity`.
 * `own_timeout_s` is used only when the device does not list `ContentTimeout`; it is held
 * to 60–3600. Call it after `lb_node_start()`, as the other types. A screen that keeps its
 * picture without power gets `show` here, to clear it (§5.7). */
void lb_display_init(lb_display_t *field, lb_node_t *node, lb_endpoint_t *endpoint,
                     uint16_t own_timeout_s, lb_display_show_fn show, void *ctx);

/** The endpoint hooks. `on_check`: `ContentTimeout` is 0 or 60–3600. `feature_of`:
 * `Quantity`/`Value` need `VALUE`, `TextLength`/`Text` need `TEXT`. */
uint8_t  lb_display_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value);
uint16_t lb_display_feature_of(uint8_t element);

/** `on_write`: a `Value` that has landed replaces whatever was shown (§5.1). */
uint8_t lb_display_write(lb_display_t *field, uint8_t element, lb_value_t value);

/** `on_write_long`: a whole `Text` — refused past `TextLength` bytes, otherwise shown. */
uint8_t lb_display_write_text(lb_display_t *field, uint8_t element,
                              const uint8_t *value, uint8_t len);

/** `on_read_long`: `Text` with its prefix. */
uint8_t lb_display_read_text(const lb_display_t *field, uint8_t element,
                             uint8_t *out, uint8_t cap, uint8_t *len);

/** Counts down the expiry. Call it in the loop. */
void lb_display_tick(lb_display_t *field, uint32_t now_ms);

/** What to draw: `Content`, the value (absent when there is none) and the text — UTF-8,
 * not terminated, `len` bytes. */
uint8_t     lb_display_content(const lb_display_t *field);
lb_value_t  lb_display_value(const lb_display_t *field);
const char *lb_display_text(const lb_display_t *field, uint8_t *len);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_DISPLAY_H */
