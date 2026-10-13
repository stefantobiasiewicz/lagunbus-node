#include "lb_posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char catalog[256];
    bool persistent;
} lb_posix_state_t;

static uint32_t posix_millis(void *ctx)
{
    (void)ctx;
    struct timespec now;
    /* MONOTONIC, not REALTIME: transaction deadlines must not step backwards just
     * because `ntp` kicked in (04-interakcje.md §2.1). On an RPi with no real-time
     * clock this is not hypothetical — the machine comes up in 1970 and jumps back to
     * "now". */
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
}

static void key_path(const lb_posix_state_t *state, const char *key, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", state->catalog, key);
}

static bool posix_store(void *ctx, const char *key, const void *data, size_t len)
{
    lb_posix_state_t *state = (lb_posix_state_t *)ctx;
    if (!state->persistent) {
        return false;
    }
    char file_handle[320];
    key_path(state, key, file_handle, sizeof file_handle);
    FILE *f = fopen(file_handle, "wb");
    if (f == NULL) {
        return false;
    }
    size_t written_ok = fwrite(data, 1, len, f);
    fclose(f);
    return written_ok == len;
}

static bool posix_load(void *ctx, const char *key, void *out, size_t len)
{
    lb_posix_state_t *state = (lb_posix_state_t *)ctx;
    if (!state->persistent) {
        return false;
    }
    char file_handle[320];
    key_path(state, key, file_handle, sizeof file_handle);
    FILE *f = fopen(file_handle, "rb");
    if (f == NULL) {
        return false; /* no file is the NORMAL state of a first run */
    }
    size_t read_ok = fread(out, 1, len, f);
    fclose(f);
    return read_ok == len;
}

static void posix_erase(void *ctx, const char *key)
{
    lb_posix_state_t *state = (lb_posix_state_t *)ctx;
    if (!state->persistent) {
        return;
    }
    char file_handle[320];
    key_path(state, key, file_handle, sizeof file_handle);
    remove(file_handle); /* no file is fine: the key was never stored */
}

static uint32_t posix_random(void *ctx)
{
    (void)ctx;
    /* Seed from the clock and a stack address. This is not cryptography — it only has
     * to be enough that forty nodes coming back after a power cut do NOT announce in
     * the same millisecond (03-adresacja §5). */
    static bool seeded = false;
    if (!seeded) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        srand((unsigned)(now.tv_nsec ^ (long)(uintptr_t)&now));
        seeded = true;
    }
    return (uint32_t)rand();
}

bool lb_posix_platform(lb_platform_t *out, const char *catalog)
{
    if (out == NULL) {
        return false;
    }
    lb_posix_state_t *state = (lb_posix_state_t *)calloc(1, sizeof *state);
    if (state == NULL) {
        return false;
    }
    if (catalog != NULL) {
        strncpy(state->catalog, catalog, sizeof state->catalog - 1);
        state->persistent = true;
    }
    out->millis = posix_millis;
    out->store = posix_store;
    out->load = posix_load;
    out->erase = posix_erase;
    out->random = posix_random;
    out->ctx = state;
    return true;
}

void lb_posix_platform_free(lb_platform_t *platform)
{
    if (platform == NULL || platform->ctx == NULL) {
        return;
    }
    free(platform->ctx);
    platform->ctx = NULL;
}
