/* When an e-paper screen refreshes, and how — a tool, not a type
 * (zadania/zrobione/DEVICE-04.md, N3, N12).
 *
 * E-paper keeps its picture without power, takes 0.3 s to change it partially and 1.5 s
 * fully, and wears with every refresh: the maker asks for minutes between them, and for a
 * full refresh every few partial ones, or the partial ones leave ghosts. A field changes
 * whenever the hub writes it. Between those two sits this pacer — pure arithmetic, tested
 * on a computer, used by the board and by its simulated twin alike.
 *
 * The device keeps the picture it would draw now and the one on the glass; each turn of
 * its loop, when the screen is not busy, it asks:
 *
 *     kind = lb_epaper_due(&pacer, now, differs);
 *     if (kind != LB_EPAPER_NONE) {
 *         start the refresh (kind);
 *         lb_epaper_started(&pacer, now, kind, has_content);
 *     }
 *
 * The rules:
 *
 *   - the very first refresh is FULL and comes at once, whatever the picture — the screen
 *     still shows what it showed before the power went, and a field is empty after a start
 *     (types/0x0022-display.md §5.7, zadania/zrobione/FEAT-12.md, N13);
 *   - nothing happens while the picture equals the glass — a write of the same value
 *     costs no refresh;
 *   - a refresh comes at most every `min_gap_ms`, counted from the start of the last one
 *     — EXCEPT while the glass shows no number (blank or dashes): a value that arrives
 *     after a restart or an expiry is shown at once, not a minute later;
 *   - after `full_every` partial refreshes the next one is FULL.
 *
 * Why the exception is keyed to the glass and not to a "first time" flag: after a start
 * the fields are empty, so the first picture is dashes, and the value comes seconds later
 * — a flag would be spent on the dashes.
 */
#ifndef LAGUNBUS_EPAPER_H
#define LAGUNBUS_EPAPER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LB_EPAPER_NONE = 0,
    LB_EPAPER_PARTIAL,
    LB_EPAPER_FULL,
} lb_epaper_kind_t;

typedef struct {
    uint32_t min_gap_ms;
    uint8_t  full_every;
    bool     started;  /**< any refresh yet — the first one clears the screen */
    bool     content;  /**< the glass shows a number */
    uint32_t last_ms;  /**< the start of the last refresh */
    uint8_t  partials; /**< since the last full one */
} lb_epaper_t;

void lb_epaper_init(lb_epaper_t *pacer, uint32_t min_gap_ms, uint8_t full_every);

/** What to do now: `differs` — the picture is not what the glass shows. */
lb_epaper_kind_t lb_epaper_due(const lb_epaper_t *pacer, uint32_t now_ms, bool differs);

/** A refresh of `kind` began at `now_ms`; `content` — the picture it draws has a number. */
void lb_epaper_started(lb_epaper_t *pacer, uint32_t now_ms, lb_epaper_kind_t kind,
                       bool content);

#ifdef __cplusplus
}
#endif

#endif /* LAGUNBUS_EPAPER_H */
