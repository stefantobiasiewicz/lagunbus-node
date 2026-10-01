/* The simulated device — the C library behind a flat API for `ctypes`.
 *
 * One API for two users: the conformance tests (python/tests/conformance/) and the
 * simulator `lagun-sim` (zadania/zrobione/FEAT-02.md). Both drive the SAME code that goes onto a
 * board, through `lagunbus_node.sim`, and both pick a device from the SAME catalogue
 * (`devices.c`) — so every device the simulator offers is one the scenarios can check.
 *
 * Why a flat API and not the structs through `ctypes`: `lb_node_t` is a large struct
 * that changes with every feature, and a Python mirror that drifts from it by one field
 * corrupts memory silently. Here the structs never cross the boundary.
 *
 * Why no callbacks into Python: this file plays the port. The clock, the store and the
 * frame queues live on the C side, and Python sets the clock, hands in frames and takes
 * out what the node sent. The fault overlay (`lagunbus_node.faults`) therefore works on
 * a C node unchanged — every frame passes through Python anyway.
 *
 * This is host code: it allocates, which the library itself never does.
 */
#ifndef LAGUNBUS_SIM_H
#define LAGUNBUS_SIM_H

#include "lagunbus/node.h"
#include "lagunbus/tools/debounce.h"
#include "lagunbus/types/key.h"
#include "lagunbus/types/output.h"

#include <stddef.h>

#define LBS_ENDPOINTS 8   /**< functional endpoints per device: 4 … 11 */
#define LBS_ATTRS     10  /**< attributes per endpoint */
#define LBS_QUEUE     256 /**< frames each way between two calls from Python */
#define LBS_SLOTS     32  /**< keys in the persistent store */
#define LBS_VALUE     8   /**< bytes per stored value — the largest is a u32 */

#define LBS_COUNT(array) ((uint8_t)(sizeof(array) / sizeof((array)[0])))

typedef struct lbs lbs_t;

typedef enum { LBS_NONE = 0, LBS_KEY, LBS_OUTPUT } lbs_role_t;

/** What an output's callbacks get as `ctx`: the device and WHICH output. */
typedef struct {
    lbs_t  *sim;
    uint8_t index;
} lbs_channel_t;

typedef struct {
    char    key[16];
    uint8_t data[LBS_VALUE];
    uint8_t len;
    bool    taken;
} lbs_slot_t;

struct lbs {
    lb_transport_t bus;
    lb_platform_t  platform;
    lb_node_t      node;
    uint16_t       device_type;

    /* Endpoint i is number 4 + i. `role` says what drives it, `index` which input
     * (bit in the pin mask) or which output it is. */
    lb_endpoint_t  endpoints[LBS_ENDPOINTS];
    lb_attr_t      attrs[LBS_ENDPOINTS][LBS_ATTRS];
    lbs_role_t     role[LBS_ENDPOINTS];
    uint8_t        index[LBS_ENDPOINTS];
    uint8_t        endpoint_count;

    /* Keys — the board's decisions, not attributes (types/0x0010-key-input.md §5.7). */
    uint8_t         input_count;
    lb_key_t        keys[LBS_ENDPOINTS];
    lb_debounce_t   debounce[LBS_ENDPOINTS];
    lb_key_config_t key_config;
    uint32_t        debounce_ms;
    bool            active_low; /**< a key shorts to ground: a LOW pin is a press */

    uint8_t       output_count;
    lb_output_t   outputs[LBS_ENDPOINTS];
    lbs_channel_t channels[LBS_ENDPOINTS];
    int           driven[LBS_ENDPOINTS]; /**< last level each output drove; -1 before any */

    uint32_t clock;
    uint32_t jitter;

    lb_frame_t inbox[LBS_QUEUE];
    uint16_t   inbox_len;
    lb_frame_t outbox[LBS_QUEUE];
    uint16_t   outbox_len;

    lbs_slot_t store[LBS_SLOTS];
    uint32_t   store_version; /**< bumped on every write, so Python saves only changes */

    bool restart_requested; /**< the node called `restart` — Python boots a new one */
};

/* --- for device definitions (devices.c) -------------------------------------------- */

/** One entry of the catalogue. `define` fills the device's endpoints and board
 * settings; the node is set up after it, from what it defined.
 *
 * `max_channels` > 0: the device comes with 1 … `max_channels` channels, chosen at
 * creation, and `define` gets the number (1 when none was asked for — so that the
 * default is the board it copies). 0: the device is fixed and `define` gets 0. */
typedef struct {
    const char *name;
    const char *about;
    void (*define)(lbs_t *sim, uint8_t channels);
    uint8_t max_channels;
} lbs_device_t;

extern const lbs_device_t lbs_devices[];
extern const size_t lbs_device_total;

/** Add a key endpoint: the next input, the next endpoint number, channel = input + 1.
 * The attribute rows are copied; the type's hooks are set. Returns false when full. */
bool lbs_add_key(lbs_t *sim, uint16_t features, const lb_attr_t *attrs, uint8_t count);

/** Add an output endpoint, the same way; commands go to `lb_output_command`. */
bool lbs_add_output(lbs_t *sim, const lb_attr_t *attrs, uint8_t count);

/* --- the API for Python ------------------------------------------------------------- */

size_t      lbs_device_count(void);
const char *lbs_device_name(size_t i);
const char *lbs_device_about(size_t i);
int         lbs_device_max_channels(size_t i);

/** A device from the catalogue, or NULL for an unknown name or a channel count it does
 * not come in. `channels` 0 is the device's default. `jitter` is what the platform's
 * `random` returns — the announce delay (03-adresacja.md §5). */
lbs_t *lbs_create(const char *name, const uint8_t *uid, uint32_t jitter, uint8_t channels);
void   lbs_destroy(lbs_t *sim);

void lbs_set_clock(lbs_t *sim, uint32_t ms);

/** Give the node a `restart`, as a board has (`esp_restart()`). Without it — the
 * default, and the path factory_reset.json checks — `FactoryReset` only drops the
 * address and rejoins. With it the node stops after the reset and
 * `lbs_restart_requested` turns true: the caller is to boot a new node from the store,
 * which is what `lagun-sim` does. */
void lbs_allow_restart(lbs_t *sim);
bool lbs_restart_requested(const lbs_t *sim);

bool     lbs_store_put(lbs_t *sim, const char *key, const uint8_t *data, uint8_t len);
int      lbs_store_get(lbs_t *sim, const char *key, uint8_t *out, uint8_t cap);
int      lbs_store_at(const lbs_t *sim, int slot, char *key, uint8_t *out);
int      lbs_store_slots(void);
uint32_t lbs_store_version(const lbs_t *sim);

/** Start in the order a board does: the node, then the endpoints. `pins` — bit i is
 * the level of input i, as a board reads its GPIO. */
void lbs_start(lbs_t *sim, uint32_t pins);
void lbs_rx(lbs_t *sim, uint32_t can_id, const uint8_t *data, uint8_t len);
/** One turn of a board's loop: the node, then every endpoint. */
void lbs_loop(lbs_t *sim, uint32_t pins);

int  lbs_tx_count(const lbs_t *sim);
void lbs_tx_get(const lbs_t *sim, int i, uint32_t *can_id, uint8_t *data, uint8_t *len);
void lbs_tx_clear(lbs_t *sim);

int      lbs_input_count(const lbs_t *sim);
uint32_t lbs_inputs_idle(const lbs_t *sim);
int      lbs_output_count(const lbs_t *sim);
int      lbs_driven(const lbs_t *sim, int output);
int      lbs_output_on(const lbs_t *sim, int output);

uint16_t lbs_address(const lbs_t *sim);
uint32_t lbs_short_id(const lbs_t *sim);

#endif /* LAGUNBUS_SIM_H */
