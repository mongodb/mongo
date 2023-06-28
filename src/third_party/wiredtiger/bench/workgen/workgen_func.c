/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "wiredtiger.h"
#include "test_util.h"
#include "workgen_func.h"

/*
 * This data symbol is also declared in the WiredTiger library. Since it is not initialized in
 * either place, it is legal (as a "common symbol") to be declared in both. If we do not declare it
 * in the workgen library, there are circumstances where it will be undefined at link time.
 */
WT_PROCESS __wt_process;

/* workgen_random_state is used as an opaque type handle. */
typedef struct workgen_random_state {
    WT_RAND_STATE state;
} workgen_random_state;

/*
 * These functions call their WiredTiger equivalents.
 */
uint32_t
workgen_atomic_add32(uint32_t *vp, uint32_t v)
{
    return (__wt_atomic_add32(vp, v));
}

uint64_t
workgen_atomic_add64(uint64_t *vp, uint64_t v)
{
    return (__wt_atomic_add64(vp, v));
}

uint32_t
workgen_atomic_sub32(uint32_t *vp, uint32_t v)
{
    return (__wt_atomic_sub32(vp, v));
}

void
workgen_clock(uint64_t *clockp)
{
    *clockp = __wt_clock(NULL);
}

void
workgen_epoch(struct timespec *tsp)
{
    __wt_epoch(NULL, tsp);
}

uint32_t
workgen_random(workgen_random_state volatile *rnd_state)
{
    return (__wt_random(&rnd_state->state));
}

int
workgen_random_alloc(WT_SESSION *session, workgen_random_state **rnd_state)
{
    workgen_random_state *state;

    state = malloc(sizeof(workgen_random_state));
    if (state == NULL) {
        *rnd_state = NULL;
        return (ENOMEM);
    }
    __wt_random_init_seed((WT_SESSION_IMPL *)session, &state->state);
    *rnd_state = state;
    return (0);
}

void
workgen_random_free(workgen_random_state *rnd_state)
{
    free(rnd_state);
}

extern void
workgen_u64_to_string_zf(uint64_t n, char *buf, size_t len)
{
    u64_to_string_zf(n, buf, len);
}

#define WORKGEN_VERSION_PREFIX "workgen-"
extern void
workgen_version(char *buf, size_t len)
{
    size_t prefix_len;

    prefix_len = strlen(WORKGEN_VERSION_PREFIX);
    (void)strncpy(buf, WORKGEN_VERSION_PREFIX, len);
    if (len > prefix_len)
        (void)strncpy(&buf[prefix_len], WIREDTIGER_VERSION_STRING, len - prefix_len);
}
