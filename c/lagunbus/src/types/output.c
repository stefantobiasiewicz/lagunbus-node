#include "lagunbus/types/output.h"

#include "lagunbus/registry.h"

#include <string.h>

/* The key in persistent storage: "output" followed by the endpoint number — "output4",
 * "output5", … One key PER OUTPUT: until 2026-09-30 every output shared the bare
 * "output", so on a node with several of them each restored the state of whichever had
 * switched last (zadania/zrobione/FEAT-02.md, F4; pinned by vectors/scenarios/output_channels.json).
 *
 * A node flashed with the old key does not find its stored state and comes up by
 * `PowerOnBehaviour` as on a first boot — once. No fallback to the old key: it would
 * be wrong for every output but one, and the price is a single switch-on.
 *
 * At most "output255", nine characters — well inside the 15 NVS allows. */
#define STATE_KEY_LEN sizeof "output255"

static void state_key(const lb_endpoint_t *endpoint, char out[STATE_KEY_LEN])
{
    static const char prefix[] = "output";
    memcpy(out, prefix, sizeof prefix - 1);
    size_t i = sizeof prefix - 1;
    uint8_t n = endpoint->number;
    if (n >= 100u) {
        out[i++] = (char)('0' + n / 100u);
    }
    if (n >= 10u) {
        out[i++] = (char)('0' + n / 10u % 10u);
    }
    out[i++] = (char)('0' + n % 10u);
    out[i] = '\0';
}

static int32_t setting(lb_endpoint_t *ep, uint8_t element, int32_t fallback)
{
    lb_attr_t *a = lb_endpoint_attr(ep, element);
    return (a == NULL || a->value.absent) ? fallback : a->value.value;
}

static bool is_on(const lb_output_t *out)
{
    return setting(out->endpoint, LB_OUT_ON_OFF, 0) != 0;
}

static uint32_t now_ms(const lb_output_t *out)
{
    return out->node->platform->millis(out->node->platform->ctx);
}

static void light_on(lb_output_t *out, uint16_t transition);
static void light_off(lb_output_t *out, uint16_t transition);
static void light_tick(lb_output_t *out, uint32_t now);

/** Store the state, but only when `PowerOnBehaviour` is `RESTORE`.
 *
 * Without that condition every switch would grind the persistent store — and a relay
 * in a hallway switches a dozen-odd times a day for twenty years.
 */
static void remember(lb_output_t *out)
{
    if (setting(out->endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF)
        != LB_POWER_ON_RESTORE) {
        return;
    }
    lb_platform_t *pl = out->node->platform;
    if (pl->store != NULL) {
        uint8_t b = is_on(out) ? 1u : 0u;
        char key[STATE_KEY_LEN];
        state_key(out->endpoint, key);
        pl->store(pl->ctx, key, &b, sizeof b);
    }
}

void lb_output_set(lb_output_t *out, bool fresh)
{
    if (out->light != NULL) {
        /* A light switched "directly" is switched at once — the caller asked for a
         * state, not for a fade. */
        if (fresh) {
            light_on(out, 0);
        } else {
            light_off(out, 0);
        }
        return;
    }
    lb_attr_t *on_off = lb_endpoint_attr(out->endpoint, LB_OUT_ON_OFF);
    if (on_off == NULL || (on_off->value.value != 0) == fresh) {
        return; /* nothing changed: nothing to report and nothing to grind */
    }
    lb_endpoint_set(out->endpoint, LB_OUT_ON_OFF, LB_NUM(fresh ? 1 : 0));

    bool invert = setting(out->endpoint, LB_OUT_INVERT, 0) != 0;
    if (out->drive != NULL) {
        out->drive(out->ctx, invert ? !fresh : fresh);
    }
    out->lit_ms = out->node->platform->millis(out->node->platform->ctx);
    remember(out);
    /* The report is the node's (zadania/zrobione/TASK-01.md N2): at once, or — within
     * `ReportMinGap` of the last one — held back and sent with the CURRENT state when
     * the gap has passed. A held-down key toggling the output does not flood the bus,
     * and the final state is never lost. */
    lb_node_flush(out->node, out->endpoint);
}

void lb_output_init(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                    lb_output_drive_fn drive, void *ctx)
{
    memset(out, 0, sizeof *out);
    out->node = node;
    out->endpoint = endpoint;
    out->drive = drive;
    out->ctx = ctx;

    int32_t behaviour = setting(endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
    bool target = behaviour == LB_POWER_ON_ON;
    if (behaviour == LB_POWER_ON_RESTORE && node->platform->load != NULL) {
        uint8_t b = 0;
        char key[STATE_KEY_LEN];
        state_key(endpoint, key);
        if (node->platform->load(node->platform->ctx, key, &b, sizeof b)) {
            target = b != 0;
        }
    }

    /* We ALWAYS drive the pin, even when the state does not differ from the
     * attribute's initial value: after a restart the hardware is in an undefined
     * state and has to be brought to whatever the node claims about itself. */
    lb_attr_t *on_off = lb_endpoint_attr(endpoint, LB_OUT_ON_OFF);
    if (on_off != NULL) {
        /* Reported, every change, no hysteresis — the type's contract
         * (types/0x0020-output.md §5.3), so the type marks it, not the device. Set
         * directly, not through `lb_endpoint_set`: the state an output starts in is not
         * a change, and nothing is reported before `Booted` (§5.7). */
        on_off->access |= LB_REPORTED;
        on_off->value = LB_NUM(target ? 1 : 0);
    }
    bool invert = setting(endpoint, LB_OUT_INVERT, 0) != 0;
    if (drive != NULL) {
        drive(ctx, invert ? !target : target);
    }
    out->lit_ms = node->platform->millis(node->platform->ctx);
}

uint8_t lb_output_write(lb_output_t *out, uint8_t element, lb_value_t value)
{
    if (element == LB_OUT_INVERT && out->drive != NULL) {
        bool on = is_on(out);
        out->drive(out->ctx, value.value != 0 ? !on : on);
    }
    return LB_RESULT_OK;
}

uint8_t lb_output_command(lb_output_t *out, uint8_t element)
{
    return lb_output_handle(out, element, NULL, 0);
}

static uint8_t relay_command(lb_output_t *out, uint8_t element)
{
    switch (element) {
    case LB_OUT_CMD_OFF:
        lb_output_set(out, false);
        return LB_RESULT_OK;
    case LB_OUT_CMD_ON:
        lb_output_set(out, true);
        return LB_RESULT_OK;
    case LB_OUT_CMD_TOGGLE:
        lb_output_set(out, !is_on(out));
        return LB_RESULT_OK;
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
}

void lb_output_tick(lb_output_t *out, uint32_t now)
{
    if (out->light != NULL) {
        light_tick(out, now);
        return;
    }
    uint32_t auto_off = (uint32_t)setting(out->endpoint, LB_OUT_AUTO_OFF_TIME, 0);
    if (auto_off > 0 && is_on(out) && now - out->lit_ms >= auto_off * 1000u) {
        lb_output_set(out, false);
    }
}

/* --- features ------------------------------------------------------------------------ */

uint16_t lb_output_feature_of(uint8_t element)
{
    switch (element) {
    case LB_OUT_LEVEL:
    case LB_OUT_TRANSITIONING:
    case LB_OUT_LAST_LEVEL:
    case LB_OUT_MIN_LEVEL:
    case LB_OUT_MAX_LEVEL:
    case LB_OUT_DEFAULT_FADE_TIME:
        return LB_OUT_F_LEVEL;
    case LB_OUT_RED:
    case LB_OUT_GREEN:
    case LB_OUT_BLUE:
    case LB_OUT_WHITE:
        return LB_OUT_F_COLOR_RGB;
    case 0x06u: /* ColorTemperature */
        return LB_OUT_F_COLOR_CCT;
    case 0x15u: /* PulseDuration */
        return LB_OUT_F_PULSE;
    default:
        return 0;
    }
}

/** The same for commands. A separate table because attributes and commands share
 * numbers and not meanings — attribute 0x03 is `Green`, command 0x03 is `SetLevel` —
 * and `feature_of` is asked about attributes only (endpoint.h). */
static uint16_t command_feature(uint8_t element)
{
    switch (element) {
    case LB_OUT_CMD_SET_LEVEL:
    case LB_OUT_CMD_STEP:
    case LB_OUT_CMD_STOP:
        return LB_OUT_F_LEVEL;
    case LB_OUT_CMD_SET_COLOR:
        return LB_OUT_F_COLOR_RGB;
    case LB_OUT_CMD_SET_COLOR_TEMPERATURE:
        return LB_OUT_F_COLOR_CCT;
    case LB_OUT_CMD_PULSE:
        return LB_OUT_F_PULSE;
    default:
        return 0;
    }
}

uint8_t lb_output_check(const lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    int32_t v = value.value;
    switch (element) {
    case LB_OUT_MIN_LEVEL:
    case LB_OUT_MAX_LEVEL:
        return v >= 1 && v <= 1000 ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_OUT_DEFAULT_FADE_TIME:
        return v <= 60000 ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    case LB_OUT_AUTO_OFF_TIME:
        return v <= 65534 ? LB_RESULT_OK : LB_RESULT_INVALID_VALUE;
    default:
        return LB_RESULT_OK;
    }
}

/* --- the light ------------------------------------------------------------------------
 *
 * `out->level` and `out->rgbw` are what shines now. Outside a transition the attributes
 * say exactly that. During one they are written QUIETLY — straight into the array, not
 * through `lb_endpoint_set` — so a read sees where the light is while nothing is owed:
 * the type reports a transition at its start and its end only (§5.3). At the end
 * `publish` sets them through the engine, which compares with what was last REPORTED,
 * so the round carries exactly what differs from the start.
 */

static bool has(const lb_output_t *out, uint16_t feature)
{
    return (out->endpoint->features & feature) != 0;
}

static void shine(lb_output_t *out)
{
    out->light(out->ctx, out->level, has(out, LB_OUT_F_COLOR_RGB) ? out->rgbw : NULL);
}

static void quiet(lb_output_t *out, uint8_t element, int32_t value)
{
    lb_attr_t *a = lb_endpoint_attr(out->endpoint, element);
    if (a != NULL) {
        a->value = LB_NUM(value);
    }
}

static void quiet_all(lb_output_t *out)
{
    quiet(out, LB_OUT_LEVEL, out->level);
    for (uint8_t i = 0; i < 4u; i++) {
        quiet(out, (uint8_t)(LB_OUT_RED + i), out->rgbw[i]);
    }
}

/** The state as it is now, through the reporting engine, as one round. */
static void publish(lb_output_t *out)
{
    lb_endpoint_t *ep = out->endpoint;
    lb_endpoint_set(ep, LB_OUT_ON_OFF, LB_NUM(out->level > 0 ? 1 : 0));
    lb_endpoint_set(ep, LB_OUT_LEVEL, LB_NUM(out->level));
    if (has(out, LB_OUT_F_COLOR_RGB)) {
        for (uint8_t i = 0; i < 4u; i++) {
            lb_endpoint_set(ep, (uint8_t)(LB_OUT_RED + i), LB_NUM(out->rgbw[i]));
        }
    }
    lb_endpoint_set(ep, LB_OUT_TRANSITIONING, LB_NUM(0));
    lb_node_flush(out->node, ep);
}

/** Where the transition is at `now` — level and colour, written quietly. */
static void advance(lb_output_t *out, uint32_t now)
{
    lb_output_fade_t *f = &out->fade;
    uint32_t elapsed = now - f->start_ms;
    if (elapsed > f->duration_ms) {
        elapsed = f->duration_ms;
    }
    int32_t e = (int32_t)elapsed;
    int32_t d = (int32_t)f->duration_ms;
    out->level = (uint16_t)(f->from_level + ((int32_t)f->to_level - f->from_level) * e / d);
    for (uint8_t i = 0; i < 4u; i++) {
        out->rgbw[i] = (uint8_t)(f->from_rgbw[i]
                                 + ((int32_t)f->to_rgbw[i] - f->from_rgbw[i]) * e / d);
    }
    quiet_all(out);
}

/** Go to `level` in `rgbw` over `duration` ms — from wherever the light is now, which is
 * how a new command interrupts a transition (§5.3, §5.8). */
static void go(lb_output_t *out, uint16_t level, const uint8_t rgbw[4], uint32_t duration)
{
    uint32_t now = now_ms(out);
    bool same = level == out->level && memcmp(rgbw, out->rgbw, 4) == 0;
    if (same || duration == 0) {
        /* At once — or already there, which ends a transition under way at the level
         * it has (and, outside one, changes nothing and so reports nothing, §5.4). */
        out->fade.active = false;
        out->level = level;
        memcpy(out->rgbw, rgbw, 4);
        shine(out);
        publish(out);
        return;
    }
    out->fade = (lb_output_fade_t){
        .active = true, .start_ms = now, .duration_ms = duration,
        .from_level = out->level, .to_level = level,
    };
    memcpy(out->fade.from_rgbw, out->rgbw, 4);
    memcpy(out->fade.to_rgbw, rgbw, 4);
    out->step_ms = now;
    shine(out);
    /* Only the start is reported: `Transitioning`, and `OnOff` when the light comes on —
     * `Level` is above zero from the first step, and `OnOff = (Level > 0)`. Going down,
     * `OnOff` stays 1 until the end, for the same reason. A second transition started
     * inside the first owes nothing: both are already 1. */
    lb_endpoint_set(out->endpoint, LB_OUT_TRANSITIONING, LB_NUM(1));
    if (level > 0) {
        lb_endpoint_set(out->endpoint, LB_OUT_ON_OFF, LB_NUM(1));
    }
    lb_node_flush(out->node, out->endpoint);
}

static uint32_t fade_time(lb_output_t *out, uint16_t transition)
{
    if (transition == LB_OUT_TRANSITION_DEFAULT) {
        return (uint32_t)setting(out->endpoint, LB_OUT_DEFAULT_FADE_TIME, 400);
    }
    return transition;
}

static uint16_t last_level(lb_output_t *out)
{
    return (uint16_t)setting(out->endpoint, LB_OUT_LAST_LEVEL, 1000);
}

/** `MinLevel`…`MaxLevel`, for a level above zero (§5.2). A minimum above the maximum —
 * two writes in the wrong order — gives way to the maximum. */
static uint16_t bounded(lb_output_t *out, int32_t level)
{
    int32_t max = setting(out->endpoint, LB_OUT_MAX_LEVEL, 1000);
    int32_t min = setting(out->endpoint, LB_OUT_MIN_LEVEL, 1);
    if (min > max) {
        min = max;
    }
    return (uint16_t)(level < min ? min : level > max ? max : level);
}

/** `Level ← v`, `LastLevel ← v` for `v > 0` (§5.1). */
static void light_level(lb_output_t *out, uint16_t level, uint16_t transition)
{
    lb_endpoint_set(out->endpoint, LB_OUT_LAST_LEVEL, LB_NUM(level));
    out->lit_ms = now_ms(out);
    go(out, level, out->rgbw, fade_time(out, transition));
}

static void light_on(lb_output_t *out, uint16_t transition)
{
    light_level(out, bounded(out, last_level(out)), transition);
}

static void light_off(lb_output_t *out, uint16_t transition)
{
    if (out->level > 0) {
        lb_endpoint_set(out->endpoint, LB_OUT_LAST_LEVEL, LB_NUM(out->level));
    }
    go(out, 0, out->rgbw, fade_time(out, transition));
}

static uint16_t u16_at(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static uint8_t light_command(lb_output_t *out, uint8_t element, const uint8_t *args,
                             uint8_t len)
{
    if (out->fade.active) {
        advance(out, now_ms(out)); /* a command starts from where the light IS */
    }
    switch (element) {
    case LB_OUT_CMD_OFF:
        light_off(out, LB_OUT_TRANSITION_DEFAULT);
        return LB_RESULT_OK;
    case LB_OUT_CMD_ON:
        light_on(out, LB_OUT_TRANSITION_DEFAULT);
        return LB_RESULT_OK;
    case LB_OUT_CMD_TOGGLE:
        if (out->level > 0) {
            light_off(out, LB_OUT_TRANSITION_DEFAULT);
        } else {
            light_on(out, LB_OUT_TRANSITION_DEFAULT);
        }
        return LB_RESULT_OK;
    case LB_OUT_CMD_SET_LEVEL: {
        if (len != 4u) {
            return LB_RESULT_INVALID_VALUE;
        }
        uint16_t level = u16_at(args);
        if (level > 1000u) {
            return LB_RESULT_INVALID_VALUE;
        }
        if (level == 0) {
            light_off(out, u16_at(args + 2));
        } else {
            light_level(out, bounded(out, level), u16_at(args + 2));
        }
        return LB_RESULT_OK;
    }
    case LB_OUT_CMD_STEP: {
        if (len != 4u) {
            return LB_RESULT_INVALID_VALUE;
        }
        int32_t delta = (int16_t)u16_at(args);
        /* Never through zero (§5.2): down from dark is nothing to dim; up from dark
         * lights at the step, at least `MinLevel` — and down from lit stops at
         * `MinLevel`. Putting it out is `Off`'s, `Toggle`'s or `SetLevel(0)`'s. */
        if (out->level == 0 && delta <= 0) {
            return LB_RESULT_OK;
        }
        light_level(out, bounded(out, (int32_t)out->level + delta), u16_at(args + 2));
        return LB_RESULT_OK;
    }
    case LB_OUT_CMD_STOP:
        /* Freezes where it is; no transition is no error (§5.3). The level it stopped at
         * is the one `On` comes back to. */
        if (out->fade.active) {
            out->fade.active = false;
            if (out->level > 0) {
                lb_endpoint_set(out->endpoint, LB_OUT_LAST_LEVEL, LB_NUM(out->level));
            }
            shine(out);
            publish(out);
        }
        return LB_RESULT_OK;
    case LB_OUT_CMD_SET_COLOR: {
        if (len != 6u) {
            return LB_RESULT_INVALID_VALUE;
        }
        if ((args[0] | args[1] | args[2] | args[3]) == 0) {
            return LB_RESULT_INVALID_VALUE; /* dark is Level's (§5.8) */
        }
        for (uint8_t i = 0; i < 4u; i++) {
            if (args[i] == 0xFFu) {
                return LB_RESULT_INVALID_VALUE; /* `u8`'s "absent", not a full channel (§3) */
            }
        }
        /* The level stays where it is — a transition of it ends here, as any new
         * command ends one. When dark there is nothing to see a fade on, so the colour
         * changes at once and nothing lights. */
        uint32_t duration = out->level == 0 ? 0 : fade_time(out, u16_at(args + 4));
        go(out, out->level, args, duration);
        return LB_RESULT_OK;
    }
    default:
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
}

uint8_t lb_output_handle(lb_output_t *out, uint8_t element, const uint8_t *args,
                         uint8_t len)
{
    uint16_t needs = command_feature(element);
    if (needs != 0 && (out->endpoint->features & needs) == 0) {
        return LB_RESULT_UNSUPPORTED_FEATURE;
    }
    if (out->light == NULL) {
        return relay_command(out, element);
    }
    return light_command(out, element, args, len);
}

/* --- storing a light ------------------------------------------------------------------
 *
 * The level (for `RESTORE`), `LastLevel` and the colour are stored together, once none
 * of them has changed for `LB_OUT_STORE_AFTER_MS` (§2.3, §5.8). What counts is where the
 * light is GOING — a transition under way does not move it at every step. */

static lb_output_stored_t stored_now(lb_output_t *out)
{
    lb_output_stored_t s = {.level = out->fade.active ? out->fade.to_level : out->level,
                  .last = last_level(out)};
    memcpy(s.rgbw, out->fade.active ? out->fade.to_rgbw : out->rgbw, 4);
    if (setting(out->endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF)
        != LB_POWER_ON_RESTORE) {
        s.level = 0; /* not stored, so not a change */
    }
    return s;
}

static void store_light(lb_output_t *out)
{
    lb_node_persist(out->node, out->endpoint, LB_OUT_LAST_LEVEL);
    if (has(out, LB_OUT_F_COLOR_RGB)) {
        for (uint8_t i = 0; i < 4u; i++) {
            lb_node_persist(out->node, out->endpoint, (uint8_t)(LB_OUT_RED + i));
        }
    }
    lb_platform_t *pl = out->node->platform;
    if (setting(out->endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF)
            == LB_POWER_ON_RESTORE && pl->store != NULL) {
        uint16_t level = out->fade.active ? out->fade.to_level : out->level;
        uint8_t b[2] = {(uint8_t)(level & 0xFFu), (uint8_t)(level >> 8)};
        char key[STATE_KEY_LEN];
        state_key(out->endpoint, key);
        pl->store(pl->ctx, key, b, sizeof b);
    }
}

static void light_tick(lb_output_t *out, uint32_t now)
{
    if (out->fade.active) {
        if (now - out->fade.start_ms >= out->fade.duration_ms) {
            out->fade.active = false;
            out->level = out->fade.to_level;
            memcpy(out->rgbw, out->fade.to_rgbw, 4);
            shine(out);
            publish(out);
        } else if (now - out->step_ms >= LB_OUT_STEP_MS) {
            out->step_ms = now;
            advance(out, now);
            shine(out);
        }
    }

    uint32_t auto_off = (uint32_t)setting(out->endpoint, LB_OUT_AUTO_OFF_TIME, 0);
    bool going_out = out->fade.active && out->fade.to_level == 0;
    if (auto_off > 0 && out->level > 0 && !going_out
        && now - out->lit_ms >= auto_off * 1000u) {
        /* Once: the fade it starts keeps `level` above zero until its end, and an `Off`
         * at every tick would start it again from where it is, forever. */
        light_off(out, LB_OUT_TRANSITION_DEFAULT);
    }

    lb_output_stored_t s = stored_now(out);
    if (memcmp(&s, &out->stored, sizeof s) != 0) {
        out->stored = s;
        out->dirty = true;
        out->changed_ms = now;
    } else if (out->dirty && now - out->changed_ms >= LB_OUT_STORE_AFTER_MS) {
        store_light(out);
        out->dirty = false;
    }
}

void lb_output_init_light(lb_output_t *out, lb_node_t *node, lb_endpoint_t *endpoint,
                          lb_output_light_fn light, void *ctx)
{
    memset(out, 0, sizeof *out);
    out->node = node;
    out->endpoint = endpoint;
    out->light = light;
    out->ctx = ctx;

    /* Reported, every change, no hysteresis — the type's contract (§5.3). */
    static const uint8_t reported[] = {
        LB_OUT_ON_OFF, LB_OUT_LEVEL, LB_OUT_RED, LB_OUT_GREEN, LB_OUT_BLUE, LB_OUT_WHITE,
        LB_OUT_TRANSITIONING,
    };
    for (size_t i = 0; i < sizeof reported; i++) {
        lb_attr_t *a = lb_endpoint_attr(endpoint, reported[i]);
        if (a != NULL) {
            a->access |= LB_REPORTED;
        }
    }

    /* The colour the node restored (or the white of a factory reset, §5.8); a light
     * without COLOR_RGB shines white. */
    static const uint8_t white[4] = {254, 254, 254, 0};
    for (uint8_t i = 0; i < 4u; i++) {
        out->rgbw[i] = (uint8_t)setting(endpoint, (uint8_t)(LB_OUT_RED + i), white[i]);
    }

    int32_t behaviour = setting(endpoint, LB_OUT_POWER_ON_BEHAVIOUR, LB_POWER_ON_OFF);
    uint16_t level = 0;
    if (behaviour == LB_POWER_ON_ON) {
        level = bounded(out, last_level(out));
    } else if (behaviour == LB_POWER_ON_RESTORE && node->platform->load != NULL) {
        uint8_t b[2];
        char key[STATE_KEY_LEN];
        state_key(endpoint, key);
        if (node->platform->load(node->platform->ctx, key, b, sizeof b)) {
            uint16_t v = u16_at(b);
            level = v == 0 ? 0 : bounded(out, v > 1000u ? 1000 : v);
        }
    }

    /* Set directly, not reported: the state an output starts in is not a change, and
     * nothing is reported before `Booted` (§5.7). Driven ALWAYS, as the relay. */
    out->level = level;
    quiet_all(out);
    quiet(out, LB_OUT_ON_OFF, level > 0 ? 1 : 0);
    quiet(out, LB_OUT_TRANSITIONING, 0);

    /* ...and where reporting counts from, as the node does for Diagnostics at start
     * (node.c, step 5 of `lb_node_start`). Without it the first change would owe every
     * reported attribute that has a value — `SetLevel` would report the colour and
     * `Transitioning` too, though neither moved. The relay does not need it: its only
     * reported attribute is the one that changes. */
    for (size_t i = 0; i < sizeof reported; i++) {
        lb_attr_t *a = lb_endpoint_attr(endpoint, reported[i]);
        lb_report_t *slot = a != NULL ? lb_endpoint_report(endpoint, reported[i], true) : NULL;
        if (slot != NULL) {
            slot->reported = a->value;
            slot->baseline = !a->value.absent;
            slot->due = false;
        }
    }
    shine(out);
    out->lit_ms = now_ms(out);
    out->stored = stored_now(out);
}
