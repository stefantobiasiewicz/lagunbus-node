#include "lb_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Extended identifier. LagunBus uses nothing else (05-mapowanie-can.md §1). */
#define LB_CAN_EFF_FLAG 0x80000000u

typedef struct {
    int fd;
} lb_socketcan_t;

static bool socketcan_send(void *ctx, const lb_frame_t *frame)
{
    lb_socketcan_t *state = (lb_socketcan_t *)ctx;
    struct can_frame raw;
    memset(&raw, 0, sizeof raw);
    raw.can_id = (frame->can_id & LB_CAN_ID_MASK) | LB_CAN_EFF_FLAG;
    raw.can_dlc = frame->len;
    memcpy(raw.data, frame->data, frame->len);

    ssize_t written = write(state->fd, &raw, sizeof raw);
    if (written == (ssize_t)sizeof raw) {
        return true;
    }
    /* ENOBUFS means "kernel queue full" and IS NOT AN ERROR: the node drops the frame
     * and keeps counting. Waiting here would stall the handling of what is coming in. */
    return false;
}

static bool socketcan_recv(void *ctx, lb_frame_t *out)
{
    lb_socketcan_t *state = (lb_socketcan_t *)ctx;
    struct can_frame raw;
    ssize_t got = read(state->fd, &raw, sizeof raw);
    if (got != (ssize_t)sizeof raw) {
        return false; /* EAGAIN on silence — the socket is non-blocking */
    }
    /* Error frames and standard 11-bit ones are not LagunBus traffic. */
    if ((raw.can_id & CAN_ERR_FLAG) || !(raw.can_id & CAN_EFF_FLAG)) {
        return false;
    }
    out->can_id = raw.can_id & LB_CAN_ID_MASK;
    out->len = raw.can_dlc > LB_MAX_PAYLOAD ? LB_MAX_PAYLOAD : raw.can_dlc;
    memcpy(out->data, raw.data, out->len);
    return true;
}

bool lb_posix_has_socketcan(void)
{
    return true;
}

bool lb_posix_open(lb_transport_t *out, const char *channel, char *error, size_t error_len)
{
    if (out == NULL || channel == NULL) {
        return false;
    }
    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        snprintf(error, error_len, "cannot open a CAN socket: %s", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, channel, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        snprintf(error, error_len,
                 "no interface %s — bring it up: sudo ip link set %s up type can "
                 "bitrate 500000",
                 channel, channel);
        close(fd);
        return false;
    }

    struct sockaddr_can address;
    memset(&address, 0, sizeof address);
    address.can_family = AF_CAN;
    address.can_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        snprintf(error, error_len, "bind(%s) failed: %s", channel, strerror(errno));
        close(fd);
        return false;
    }

    /* Non-blocking, because the node loop has to return even on silence — otherwise
     * transaction deadlines would stall. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        snprintf(error, error_len, "cannot switch the socket to non-blocking mode: %s",
                 strerror(errno));
        close(fd);
        return false;
    }

    lb_socketcan_t *state = (lb_socketcan_t *)calloc(1, sizeof *state);
    if (state == NULL) {
        close(fd);
        return false;
    }
    state->fd = fd;
    out->send = socketcan_send;
    out->recv = socketcan_recv;
    out->ctx = state;
    return true;
}

void lb_posix_close(lb_transport_t *transport)
{
    if (transport == NULL || transport->ctx == NULL) {
        return;
    }
    lb_socketcan_t *state = (lb_socketcan_t *)transport->ctx;
    close(state->fd);
    free(state);
    transport->ctx = NULL;
    transport->send = NULL;
    transport->recv = NULL;
}

#else /* not Linux */

bool lb_posix_has_socketcan(void)
{
    return false;
}

bool lb_posix_open(lb_transport_t *out, const char *channel, char *error, size_t error_len)
{
    (void)out;
    (void)channel;
    snprintf(error, error_len,
             "SocketCAN exists only on Linux; run this node on a Raspberry Pi");
    return false;
}

void lb_posix_close(lb_transport_t *transport)
{
    (void)transport;
}

#endif
