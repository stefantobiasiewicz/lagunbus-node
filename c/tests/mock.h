/* A shared bus and platform mock for the tests.
 *
 * The clock is a field, not a system call: tests of time-dependent behaviour — contact
 * bounce, hold, AutoOffTime — have to be able to jump two seconds in a single line.
 * Waiting for real would make the tests slow and flaky.
 */
#ifndef MOCK_H
#define MOCK_H

#include "lagunbus/node.h"

#include <string.h>

/* The header is shared, and not every test uses every helper. Without this,
 * `-Werror=unused-function` breaks the build because test_output never looks
 * at events. */
#define LB_MOCK __attribute__((unused))

#define MOCK_CAPACITY 64

typedef struct {
    lb_frame_t to_node[16];
    uint8_t    pending;
    lb_frame_t from_node[MOCK_CAPACITY];
    uint8_t    sent;
    uint32_t   clock;
    uint32_t   jitter;

    /* Persistent storage. There has to be room for several keys, because the node
     * stores AT LEAST three things at once: the address, the boot counter and the
     * output state. A mock with a single key silently lost one in favour of another. */
    struct {
        char    key_name[16];
        uint8_t data[8];
        uint8_t length;
        bool    taken;
    } memory[16]; /* since TASK-01 every persistent setting has a key of its own */
} mock_t;

LB_MOCK static bool mock_send(void *ctx, const lb_frame_t *f)
{
    mock_t *a = (mock_t *)ctx;
    if (a->sent >= MOCK_CAPACITY) {
        return false;
    }
    a->from_node[a->sent++] = *f;
    return true;
}

LB_MOCK static bool mock_recv(void *ctx, lb_frame_t *out)
{
    mock_t *a = (mock_t *)ctx;
    if (a->pending == 0) {
        return false;
    }
    *out = a->to_node[0];
    memmove(a->to_node, a->to_node + 1, sizeof(lb_frame_t) * (size_t)(--a->pending));
    return true;
}

LB_MOCK static uint32_t mock_millis(void *ctx) { return ((mock_t *)ctx)->clock; }
LB_MOCK static uint32_t mock_random(void *ctx) { return ((mock_t *)ctx)->jitter; }

LB_MOCK static bool mock_store(void *ctx, const char *key_name, const void *data, size_t len)
{
    mock_t *a = (mock_t *)ctx;
    if (len > sizeof a->memory[0].data) {
        return false;
    }
    size_t free_slots = sizeof a->memory / sizeof a->memory[0];
    for (size_t i = 0; i < sizeof a->memory / sizeof a->memory[0]; i++) {
        if (a->memory[i].taken && strcmp(a->memory[i].key_name, key_name) == 0) {
            free_slots = i;
            break;
        }
        if (!a->memory[i].taken && free_slots == sizeof a->memory / sizeof a->memory[0]) {
            free_slots = i;
        }
    }
    if (free_slots >= sizeof a->memory / sizeof a->memory[0]) {
        return false;
    }
    strncpy(a->memory[free_slots].key_name, key_name, sizeof a->memory[free_slots].key_name - 1);
    memcpy(a->memory[free_slots].data, data, len);
    a->memory[free_slots].length = (uint8_t)len;
    a->memory[free_slots].taken = true;
    return true;
}

LB_MOCK static void mock_erase(void *ctx, const char *key_name)
{
    mock_t *a = (mock_t *)ctx;
    for (size_t i = 0; i < sizeof a->memory / sizeof a->memory[0]; i++) {
        if (a->memory[i].taken && strcmp(a->memory[i].key_name, key_name) == 0) {
            memset(&a->memory[i], 0, sizeof a->memory[i]);
        }
    }
}

LB_MOCK static bool mock_load(void *ctx, const char *key_name, void *out, size_t len)
{
    mock_t *a = (mock_t *)ctx;
    for (size_t i = 0; i < sizeof a->memory / sizeof a->memory[0]; i++) {
        if (a->memory[i].taken && strcmp(a->memory[i].key_name, key_name) == 0
            && a->memory[i].length == len) {
            memcpy(out, a->memory[i].data, len);
            return true;
        }
    }
    return false;
}

/** Whether anything has been stored under this key. */
LB_MOCK static bool mock_has(const mock_t *a, const char *key_name)
{
    for (size_t i = 0; i < sizeof a->memory / sizeof a->memory[0]; i++) {
        if (a->memory[i].taken && strcmp(a->memory[i].key_name, key_name) == 0) {
            return true;
        }
    }
    return false;
}

/** How many events of a given kind the node has sent. */
LB_MOCK static uint8_t event_count(const mock_t *a, uint8_t element)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < a->sent; i++) {
        lb_fields_t f = lb_decode_id(a->from_node[i].can_id);
        if (f.kind == LB_KIND_EVENT && f.element == element) {
            n++;
        }
    }
    return n;
}

/** The first event of a given kind, or NULL. */
LB_MOCK static const lb_frame_t *event_num(const mock_t *a, uint8_t element, uint8_t which)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < a->sent; i++) {
        lb_fields_t f = lb_decode_id(a->from_node[i].can_id);
        if (f.kind == LB_KIND_EVENT && f.element == element && n++ == which) {
            return &a->from_node[i];
        }
    }
    return NULL;
}

#endif /* MOCK_H */
