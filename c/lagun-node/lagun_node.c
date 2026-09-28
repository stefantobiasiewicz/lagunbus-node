/* `lagun-node` — a LagunBus node over SocketCAN.
 *
 * This is **not a simulator**. It is the very code that will run on an ESP32, plugged
 * into a SocketCAN socket instead of a TWAI controller: to the hub on the other side
 * it looks like a device, because it is one — only without an enclosure and without a
 * relay.
 *
 * Two modes:
 *   default    — the node answers reads and writes,
 *   `--sniff`  — it only watches, printing traffic decoded by our own codec.
 *
 * Why `--sniff` when there is `candump`: because `candump` shows bytes, while this shows
 * MEANING — and, more importantly, **it goes through our codec**. If something looks
 * different here than in `candump`, the fault is in the library, not on the bus.
 *
 * The node announces itself and waits for an address, so to take it in the hub must
 * have **joining open** (`accept` in the `tui`). Without that the announcement is set
 * aside and nothing happens — which the output shows.
 */
#include "lagunbus/node.h"
#include "lagunbus/registry.h"
#include "lb_posix.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t end_ms = 0;

static void on_signal(int signal)
{
    (void)signal;
    end_ms = 1;
}

/* --- watch mode ----------------------------------------------------------------- */

static void print_frame(const lb_frame_t *frame, double since_start)
{
    if (lb_is_join(frame->can_id)) {
        printf("%8.3f  JOIN        ShortId 0x%06X  %s\n",
               since_start,
               lb_join_short_id(frame->can_id),
               (frame->can_id >> LB_JOIN_DIRECTION_SHIFT) & 1u ? "announce" : "assign");
        return;
    }

    lb_fields_t f = lb_decode_id(frame->can_id);
    printf("%8.3f  %-12s 0x%03X ep%-2u el 0x%02X ",
           since_start, lb_kind_name(f.kind), f.address, f.endpoint, f.element);

    /* Only transactional messages have a header byte — for the rest it is data. */
    bool transactional = f.kind == LB_KIND_COMMAND || f.kind == LB_KIND_WRITE
                     || f.kind == LB_KIND_READ || f.kind == LB_KIND_RESPONSE
                     || f.kind == LB_KIND_LONG_RESPONSE || f.kind == LB_KIND_LONG_WRITE;
    if (transactional && frame->len > 0) {
        uint8_t tid = 0, result = 0;
        lb_decode_header(frame->data[0], &tid, &result);
        printf("tid=%u ", tid);
        if (f.kind == LB_KIND_RESPONSE || f.kind == LB_KIND_LONG_RESPONSE) {
            printf("%s ", lb_result_name(result));
        }
    }

    for (uint8_t i = 0; i < frame->len; i++) {
        printf("%02X", frame->data[i]);
    }
    printf("\n");
}

static int sniff(lb_transport_t *bus, const char *channel)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    printf("listening on %s; Ctrl-C quits\n", channel);

    unsigned long counted = 0;
    while (!end_ms) {
        lb_frame_t frame = {0};
        if (bus->recv(bus->ctx, &frame)) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double since_start = (double)(now.tv_sec - start.tv_sec)
                             + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
            print_frame(&frame, since_start);
            counted++;
        } else {
            /* Silence. The socket is non-blocking, so without this we would spin at
             * full tilt — on an RPi that shows up on the fan immediately. */
            usleep(2000);
        }
    }

    printf("\nframes received: %lu\n", counted);
    return 0;
}

/* --- endpoint: an output -------------------------------------------------------- */

/* Elements of type 0x0020 Output. Just enough to have something to flick. */
#define OUT_ON_OFF              0x00u
#define OUT_POWER_ON_BEHAVIOUR  0x13u
#define OUT_INVERT              0x14u
#define OUT_PURPOSE             0x17u
#define OUT_CMD_OFF             0x00u
#define OUT_CMD_ON              0x01u
#define OUT_CMD_TOGGLE          0x02u

static lb_attr_t output_attrs[] = {
    {OUT_ON_OFF,             LB_T_BOOL, LB_RO,               LB_NUM(0)},
    {OUT_POWER_ON_BEHAVIOUR, LB_T_U8,   LB_RW | LB_PERSIST,  LB_NUM(0)},
    {OUT_INVERT,             LB_T_BOOL, LB_RW | LB_PERSIST,  LB_NUM(0)},
    {OUT_PURPOSE,            LB_T_U8,   LB_RW | LB_PERSIST,  LB_NUM(1)}, /* Light */
};

/** A flick. On a board a relay sits here; here it is a line on the screen. */
static void set_attr(lb_endpoint_t *self, int state)
{
    lb_attr_t *on_off = lb_endpoint_attr(self, OUT_ON_OFF);
    if (on_off == NULL || on_off->value.value == state) {
        return;
    }
    on_off->value = LB_NUM(state);
    printf("  output ep%u -> %s\n", self->number, state ? "ON" : "off");
    fflush(stdout);
}

static uint8_t output_command(lb_endpoint_t *self, uint8_t element,
                               const uint8_t *args, uint8_t len)
{
    (void)args;
    (void)len;
    lb_attr_t *on_off = lb_endpoint_attr(self, OUT_ON_OFF);

    switch (element) {
    case OUT_CMD_OFF:
        set_attr(self, 0);
        return LB_RESULT_OK;
    case OUT_CMD_ON:
        set_attr(self, 1);
        return LB_RESULT_OK;
    case OUT_CMD_TOGGLE:
        set_attr(self, on_off != NULL && on_off->value.value ? 0 : 1);
        return LB_RESULT_OK;
    default:
        /* The remaining `Output` commands belong to features this output does not
         * have: SetLevel without LEVEL, SetColor without COLOR_RGB. We do not pretend
         * we can. */
        return LB_RESULT_UNSUPPORTED_ELEMENT;
    }
}

static uint8_t output_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    printf("  write el 0x%02X = %d\n", element, value.value);
    fflush(stdout);
    return LB_RESULT_OK;
}

static lb_endpoint_t endpoints[] = {{
    .number = 4,
    .type_id = LB_TYPE_OUTPUT,
    .revision = 1,
    .features = 0, /* no LEVEL and no colour — a plain relay */
    .channel = 1,
    .attrs = output_attrs,
    .attr_count = (uint8_t)(sizeof output_attrs / sizeof output_attrs[0]),
    .on_command = output_command,
    .on_write = output_write,
}};

/* --- node mode ------------------------------------------------------------------ */

/** A `Uid` from four bytes given by a human plus a fixed tail.
 *
 * On a board the `Uid` comes from eFuse and is genuinely unique. Here it is supplied,
 * because two processes on one PC have to differ somehow — and guessing it from the
 * PID would give a node that is somebody else after every restart.
 */
static void build_uid(uint8_t *uid, uint32_t seed)
{
    uid[0] = (uint8_t)(seed >> 24);
    uid[1] = (uint8_t)(seed >> 16);
    uid[2] = (uint8_t)(seed >> 8);
    uid[3] = (uint8_t)seed;
    uid[4] = 'P';  /* POSIX — so a dump shows at a glance where this node is from */
    uid[5] = 'O';
    uid[6] = 'S';
    uid[7] = 'X';
}

static int node_inst(lb_transport_t *bus, const char *channel, const char *catalog,
                 uint16_t address, uint32_t seed)
{
    lb_platform_t platform_impl = {0};
    if (!lb_posix_platform(&platform_impl, catalog)) {
        fprintf(stderr, "could not set the platform up (directory: %s)\n",
                catalog != NULL ? catalog : "none");
        return 1;
    }

    lb_identity_t identity_cfg = {
        .device_type = 0x0020,     /* a single output */
        .node_class = LB_CLASS_CONTROL, /* it controls something -> block 0x001..0x0FF */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,    /* 0.1.0 */
        .hardware = 1,
        .protocol = 1,
    };
    build_uid(identity_cfg.uid, seed);

    lb_node_t node;
    lb_node_init(&node, bus, &platform_impl, &identity_cfg, endpoints,
                 (uint8_t)(sizeof endpoints / sizeof endpoints[0]));
    node.address = address;

    printf("node on %s\n"
           "  ShortId  0x%08X\n"
           "  endpoint 4: Output, channel 1\n"
           "  persistence %s\n",
           channel, lb_node_short_id(&node),
           catalog != NULL ? catalog : "OFF (--state turns it on)");
    if (address != 0) {
        printf("  address  0x%03X — given by hand, so NO announcement\n"
               "           The hub will not recognise it unless it assigned that address itself.\n",
               address);
    } else {
        printf("  address  none — announcing and waiting for an assignment\n"
               "           The hub must have JOINING OPEN, or it will set the announcement aside.\n");
    }
    printf("Ctrl-C quits.\n\n");

    lb_node_start(&node);

    uint32_t last_seen = 0;
    uint16_t had_address = node.address;
    uint16_t attempts_made = 0;
    while (!end_ms) {
        lb_node_tick(&node);

        if (node.address != had_address) {
            had_address = node.address;
            printf("  ASSIGNMENT: address 0x%03X, Booted announced\n", node.address);
            fflush(stdout);
        }
        if (node.join_attempts != attempts_made) {
            attempts_made = node.join_attempts;
            printf("  announcement #%u; next in ~%u s\n",
                   node.join_attempts, node.join_backoff_s);
            fflush(stdout);
        }

        /* We show the counter only when it changed — a node that keeps quiet is to
         * keep quiet on screen as well. */
        if (node.received != last_seen) {
            last_seen = node.received;
            printf("  frames for me: %u, replies: %u, dropped: %u\n",
                   node.received, node.sent, node.dropped);
            fflush(stdout);
        }
        usleep(2000);
    }

    printf("\nreceived %u, sent %u, dropped %u\n",
           node.received, node.sent, node.dropped);
    lb_posix_platform_free(&platform_impl);
    return 0;
}

/* --- command line --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *channel = "can0";
    const char *catalog = NULL;
    bool sniff_only = false;
    uint16_t address = 0; /* 0 = no address, i.e. the normal route: announcing */
    uint32_t seed = 0xA1B2C3D4u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bus") == 0 && i + 1 < argc) {
            channel = argv[++i];
        } else if (strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
            unsigned long a = strtoul(argv[++i], NULL, 0);
            if (a == 0 || a > 0x7FF) {
                fprintf(stderr, "the address must fit in 1..0x7FF (0 is broadcast)\n");
                return 2;
            }
            address = (uint16_t)a;
        } else if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            catalog = argv[++i];
        } else if (strcmp(argv[i], "--sniff") == 0) {
            sniff_only = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: lagun-node [--bus can0] [--uid 0xA1B2C3D4] [--address 0x042]\n"
                   "                   [--state DIRECTORY] [--sniff]\n"
                   "\n"
                   "  A LagunBus node over SocketCAN — the same code that runs on an ESP32.\n"
                   "\n"
                   "  --address  force an address and SKIP announcing. For trying things\n"
                   "             with a hub that already knows this node; normally needless\n"
                   "  --uid      the four high bytes of Uid; two nodes on one bus\n"
                   "             must have different ones\n"
                   "  --state    directory for persistent things; without it the node\n"
                   "             remembers nothing after a restart\n"
                   "  --sniff    do not answer, only print the traffic\n");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    lb_transport_t bus = {0};
    char error[256] = {0};
    if (!lb_posix_open(&bus, channel, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int result = sniff_only ? sniff(&bus, channel)
                            : node_inst(&bus, channel, catalog, address, seed);
    lb_posix_close(&bus);
    return result;
}
