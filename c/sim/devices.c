/* The catalogue of simulated devices — what `lagun-sim --help` lists and what the
 * scenarios in vectors/scenarios/ name as their `profile`.
 *
 * One list for both on purpose (zadania/zrobione/FEAT-02.md, N10): a device offered by the
 * simulator is a device the conformance tests can check, and a profile a scenario needs
 * is a device anyone can run next to the hub.
 *
 * A device here is what a board's application is minus the hardware: its endpoints with
 * the attributes it chooses to expose, and the decisions of its board (bounce window,
 * polarity, the key's own timings). `key`, `key3`, `output` and `out8` copy the boards in
 * `devices/apps/` where noted; the rest are variants a scenario or a demonstration needs.
 *
 * Adding a device: a function below and a line in `lbs_devices`. Nothing else — the
 * endpoint numbers (4, 5, …), channels and hooks come from `lbs_add_key` /
 * `lbs_add_output` / `lbs_add_display` / `lbs_add_sense`.
 */
#include "sim.h"

#include "lagunbus/registry.h"

/* `channels` is used by the devices that come in several sizes (`max_channels` in the
 * table); the fixed ones ignore it. */

/* --- boards ------------------------------------------------------------------------- */

/** A key board: a key shorted to ground (a LOW pin is a press), filtered for 15 ms,
 * the type's default timings. The DEVICE's decisions, not attributes
 * (types/0x0010-key-input.md §5.7). */
static void key_board(lbs_t *sim)
{
    sim->device_type = 0x0010;
    sim->debounce_ms = 15;
    sim->active_low = true;
}

static void output_board(lbs_t *sim) { sim->device_type = 0x0020; }

/* --- keys --------------------------------------------------------------------------- */

/** Every feature, every timing tunable from the hub. */
static const lb_attr_t key_full[] = {
    LB_KEY_ATTR_PRESS_COUNT(),
    LB_KEY_ATTR_EVENT_MODE(),
    LB_KEY_ATTR_MULTI_CLICK_WINDOW(350),
    LB_KEY_ATTR_MAX_CLICKS(3),
    LB_KEY_ATTR_HOLD_ENABLED(),
    LB_KEY_ATTR_HOLD_THRESHOLD(500),
    LB_KEY_ATTR_HOLD_TICK_INTERVAL(200),
};
#define KEY_FULL_FEATURES (LB_KEY_F_MULTICLICK | LB_KEY_F_HOLD | LB_KEY_F_RAW)

/** 1 … 8 keys, each with every feature; one is the default. */
static void key(lbs_t *sim, uint8_t channels)
{
    key_board(sim);
    for (uint8_t i = 0; i < channels; i++) {
        lbs_add_key(sim, KEY_FULL_FEATURES, key_full, LBS_COUNT(key_full));
    }
}

/** A doorbell: no features, no attributes — the core alone. */
static void bell(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    key_board(sim);
    lbs_add_key(sim, 0, NULL, 0);
}

/** HOLD and MULTICLICK, but the timings are the device's own: the hub may switch the
 * mode and hold on or off, and tune nothing. Values unlike the type's recommendations,
 * so that a scenario shows whose they are. */
static void key_fixed(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t attrs[] = {
        LB_KEY_ATTR_EVENT_MODE(),
        LB_KEY_ATTR_HOLD_ENABLED(),
    };
    key_board(sim);
    lbs_add_key(sim, LB_KEY_F_HOLD | LB_KEY_F_MULTICLICK, attrs, LBS_COUNT(attrs));
    sim->key_config.hold_threshold_ms = 800;
    sim->key_config.hold_tick_ms = 400;
    sim->key_config.multi_click_window_ms = 250;
    sim->key_config.max_clicks = 2;
}

/** The most ordinary wall switch: three keys, each a full `key`. */
static void panel3(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    key_board(sim);
    for (int i = 0; i < 3; i++) {
        lbs_add_key(sim, KEY_FULL_FEATURES, key_full, LBS_COUNT(key_full));
    }
}

/** As `devices/apps/key3`: three keys with RAW and HOLD and no multi-click — a
 * combination no other device here has, which is why it has a scenario of its own
 * (`key3.json`). A model of its own too: `DeviceType` names the model, not the
 * endpoint type (types/0x0001-device.md §2.6; zadania/zrobione/FEAT-09.md, N3). */
static void key3(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t attrs[] = {
        LB_KEY_ATTR_PRESS_COUNT(),
        LB_KEY_ATTR_EVENT_MODE(),
        LB_KEY_ATTR_HOLD_ENABLED(),
        LB_KEY_ATTR_HOLD_THRESHOLD(500),
        LB_KEY_ATTR_HOLD_TICK_INTERVAL(200),
    };
    key_board(sim);
    sim->device_type = 0x0011;
    for (int i = 0; i < 3; i++) {
        lbs_add_key(sim, LB_KEY_F_RAW | LB_KEY_F_HOLD, attrs, LBS_COUNT(attrs));
    }
}

/* --- outputs ------------------------------------------------------------------------ */

/** Reports no more often than every 200 ms: a held-down key toggling an output must not
 * flood the bus. A common setting, so it is the endpoint's `.common`, not a row. */
static void min_gap_200(lb_endpoint_t *ep)
{
    if (ep != NULL) {
        ep->common.report_min_gap = 200;
    }
}

/** As `devices/apps/output`: a relay module that reports no more often than every
 * 200 ms — 1 … 8 of them; one, as the board, is the default. Its relay switches on a LOW
 * pin, but that is build: the application inverts the pin, the library drives the coil
 * state, and there is no `Invert` for the hub to overwrite
 * (zadania/zrobione/CR-04.md, N7). */
static void output(lbs_t *sim, uint8_t channels)
{
    static const lb_attr_t attrs[] = {
        {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
        {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
        {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
        {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)},
    };
    output_board(sim);
    for (uint8_t i = 0; i < channels; i++) {
        min_gap_200(lbs_add_output(sim, attrs, LBS_COUNT(attrs)));
    }
}

/** Outputs that restore their state after a power cut, no `ReportMinGap`. A module with
 * the normally-closed contact on its terminals, so it has `Invert` — an installation
 * setting (types/0x0020-output.md §5.6) — off by default. */
static const lb_attr_t output_restore[] = {
    {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
    {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_RESTORE)},
    {LB_OUT_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)},
};

/** Two of them — enough for a scenario to see each keep its own state. */
static void output2(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    output_board(sim);
    for (int i = 0; i < 2; i++) {
        lbs_add_output(sim, output_restore, LBS_COUNT(output_restore));
    }
}

/** An eight-relay board, as a distribution box has. */
static void output8(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    output_board(sim);
    for (int i = 0; i < 8; i++) {
        lbs_add_output(sim, output_restore, LBS_COUNT(output_restore));
    }
}

/** As `devices/apps/out8`: eight outputs on an MCP23017, LEDs for now, lit by a HIGH pin;
 * otherwise the settings of `output`, without `Invert`. No scenario: eight outputs are
 * `output8.json`'s, these settings `output.json`'s — nothing here is a new case. */
static void out8(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t attrs[] = {
        {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
        {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
        {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
        {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)},
    };
    output_board(sim);
    sim->device_type = 0x0021;
    for (int i = 0; i < 8; i++) {
        min_gap_200(lbs_add_output(sim, attrs, LBS_COUNT(attrs)));
    }
}

/* --- displays ----------------------------------------------------------------------- */

/** Three fields, one of each kind a screen has (types/0x0022-display.md §6): a
 * temperature with `ContentTimeout`, a 16-byte text line that keeps its own 120 s, and a
 * humidity that can show a short text instead — the type's whole range, not a board
 * (the board is `epd`, below). `DeviceType` 0x0024 is the simulator's, not a model's. */
static void display(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t temperature[] = {
        LB_DSP_ATTR_CONTENT(),
        LB_DSP_ATTR_QUANTITY(LB_QUANTITY_TEMPERATURE),
        LB_DSP_ATTR_VALUE(),
        LB_DSP_ATTR_CONTENT_TIMEOUT(LB_DSP_TIMEOUT_DEFAULT_S),
    };
    static const lb_attr_t line[] = {
        LB_DSP_ATTR_CONTENT(),
        LB_DSP_ATTR_TEXT_LENGTH(16),
        LB_DSP_ATTR_TEXT(),
    };
    static const lb_attr_t humidity[] = {
        LB_DSP_ATTR_CONTENT(),
        LB_DSP_ATTR_QUANTITY(LB_QUANTITY_HUMIDITY),
        LB_DSP_ATTR_VALUE(),
        LB_DSP_ATTR_TEXT_LENGTH(32),
        LB_DSP_ATTR_TEXT(),
        LB_DSP_ATTR_CONTENT_TIMEOUT(LB_DSP_TIMEOUT_DEFAULT_S),
    };
    sim->device_type = 0x0024;
    lbs_add_display(sim, LB_DSP_F_VALUE, temperature, LBS_COUNT(temperature), 0);
    lbs_add_display(sim, LB_DSP_F_TEXT, line, LBS_COUNT(line), 120);
    lbs_add_display(sim, LB_DSP_F_VALUE | LB_DSP_F_TEXT, humidity, LBS_COUNT(humidity), 0);
}

/** The board `epd` (devices/apps/epd, zadania/zrobione/DEVICE-04.md): the Waveshare 1.9″
 * segment e-paper — a temperature in "188.8 °C" on ep4, a humidity in "88.8 %" on ep5,
 * VALUE only, each with `ContentTimeout` (N6). The glass is paced as the board paces it —
 * at most every 60 s, a full refresh after 10 partial ones (N3); the board takes both from
 * Kconfig, these are its defaults. The layouts are the board's too (N1): −9.9 … 199.9 °C,
 * 0.0 … 99.9 %. `DeviceType` 0x0023 (N8). */
static void epd(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t temperature[] = {
        LB_DSP_ATTR_CONTENT(),
        LB_DSP_ATTR_QUANTITY(LB_QUANTITY_TEMPERATURE),
        LB_DSP_ATTR_VALUE(),
        LB_DSP_ATTR_CONTENT_TIMEOUT(LB_DSP_TIMEOUT_DEFAULT_S),
    };
    static const lb_attr_t humidity[] = {
        LB_DSP_ATTR_CONTENT(),
        LB_DSP_ATTR_QUANTITY(LB_QUANTITY_HUMIDITY),
        LB_DSP_ATTR_VALUE(),
        LB_DSP_ATTR_CONTENT_TIMEOUT(LB_DSP_TIMEOUT_DEFAULT_S),
    };
    static const lb_digits_t degrees = {.whole = 2, .leading_one = true, .decimals = 1,
                                        .minus = true};
    static const lb_digits_t percent = {.whole = 2, .leading_one = false, .decimals = 1,
                                        .minus = false};
    sim->device_type = 0x0023;
    lbs_epaper(sim, 60000u, 10u);
    lbs_display_digits(sim, lbs_add_display(sim, LB_DSP_F_VALUE, temperature,
                                            LBS_COUNT(temperature), 0),
                       &degrees);
    lbs_display_digits(sim, lbs_add_display(sim, LB_DSP_F_VALUE, humidity,
                                            LBS_COUNT(humidity), 0),
                       &percent);
}

/* --- sensors ------------------------------------------------------------------------ */

/** The board `env` (devices/apps/env, zadania/zrobione/DEVICE-01.md): a temperature on
 * ep4, a humidity on ep5, a light on ep6 — SHT40 and VEML7700 — each with `MIN_MAX`,
 * `THRESHOLD` and `Offset`, reporting with the quantities' defaults: 0.20 °C, 1.00 %,
 * 50 lx or 10 %, every 60 s. What it measures `lagun-sim` sets by hand. A sensor, so it
 * joins from the sensor address block; `DeviceType` 0x0030 (DEVICE-01 N9). */
#define ENV_ROWS(quantity)                     \
    {                                          \
        LB_SNS_ATTR_VALUE(),                   \
        LB_SNS_ATTR_QUANTITY(quantity),        \
        LB_SNS_ATTR_MIN_MEASURED(),            \
        LB_SNS_ATTR_MAX_MEASURED(),            \
        LB_SNS_ATTR_OFFSET(),                  \
        LB_SNS_ATTR_THRESHOLD_LOW(),           \
        LB_SNS_ATTR_THRESHOLD_HIGH(),          \
        LB_SNS_ATTR_THRESHOLD_HYSTERESIS(),    \
    }

static void env(lbs_t *sim, uint8_t channels)
{
    (void)channels;
    static const lb_attr_t temperature[] = ENV_ROWS(LB_QUANTITY_TEMPERATURE);
    static const lb_attr_t humidity[] = ENV_ROWS(LB_QUANTITY_HUMIDITY);
    static const lb_attr_t light[] = ENV_ROWS(LB_QUANTITY_ILLUMINANCE);
    const uint16_t features = LB_SNS_F_MIN_MAX | LB_SNS_F_THRESHOLD;
    sim->device_type = 0x0030;
    sim->node_class = LB_CLASS_SENSOR;
    lbs_add_sense(sim, LB_QUANTITY_TEMPERATURE, features, temperature, LBS_COUNT(temperature));
    lbs_add_sense(sim, LB_QUANTITY_HUMIDITY, features, humidity, LBS_COUNT(humidity));
    lbs_add_sense(sim, LB_QUANTITY_ILLUMINANCE, features, light, LBS_COUNT(light));
}

/* --- the catalogue ------------------------------------------------------------------ */

const lbs_device_t lbs_devices[] = {
    {"key",       "keys: click, multiclick, hold, raw; every timing tunable", key,       LBS_ENDPOINTS},
    {"bell",      "a doorbell key: one event per press, nothing to tune",    bell,      0},
    {"key_fixed", "one key with hold and multiclick on its own timings",     key_fixed, 0},
    {"panel3",    "a wall switch with three keys",                           panel3,    0},
    {"key3",      "three keys as devices/apps/key3: click, raw, hold",       key3,      0},
    {"output",    "relays as devices/apps/output",                           output,    LBS_ENDPOINTS},
    {"output2",   "two relays that remember their state",                    output2,   0},
    {"output8",   "eight relays that remember their state",                  output8,   0},
    {"out8",      "eight outputs as devices/apps/out8 (MCP23017)",           out8,      0},
    {"display",   "a display: temperature, a text line, humidity or text",   display,   0},
    {"epd",       "e-paper as devices/apps/epd: temperature and humidity",   epd,       0},
    {"env",       "a room sensor: temperature, humidity, light",             env,       0},
};

const size_t lbs_device_total = sizeof lbs_devices / sizeof lbs_devices[0];
