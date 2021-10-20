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

/*
 * read_op --
 *     Perform a read operation, waiting out prepare conflicts.
 */
static inline int
read_op(WT_CURSOR *cursor, read_operation op, int *exactp)
{
    WT_DECL_RET;

    /*
     * Read operations wait out prepare-conflicts. (As part of the snapshot isolation checks, we
     * repeat reads that succeeded before, they should be repeatable.)
     */
    switch (op) {
    case NEXT:
        while ((ret = cursor->next(cursor)) == WT_PREPARE_CONFLICT)
            __wt_yield();
        break;
    case PREV:
        while ((ret = cursor->prev(cursor)) == WT_PREPARE_CONFLICT)
            __wt_yield();
        break;
    case SEARCH:
        while ((ret = cursor->search(cursor)) == WT_PREPARE_CONFLICT)
            __wt_yield();
        break;
    case SEARCH_NEAR:
        while ((ret = cursor->search_near(cursor, exactp)) == WT_PREPARE_CONFLICT)
            __wt_yield();
        break;
    }
    return (ret);
}

/*
 * mmrand --
 *     Return a random value between a min/max pair, inclusive.
 */
static inline uint32_t
mmrand(WT_RAND_STATE *rnd, u_int min, u_int max)
{
    uint32_t v;
    u_int range;

    /*
     * Test runs with small row counts can easily pass a max of 0 (for example, "g.rows / 20").
     * Avoid the problem.
     */
    if (max <= min)
        return (min);

    v = rng(rnd);
    range = (max - min) + 1;
    v %= range;
    v += min;
    return (v);
}

static inline void
random_sleep(WT_RAND_STATE *rnd, u_int max_seconds)
{
    uint64_t i, micro_seconds;

    /*
     * We need a fast way to choose a sleep time. We want to sleep a short period most of the time,
     * but occasionally wait longer. Divide the maximum period of time into 10 buckets (where bucket
     * 0 doesn't sleep at all), and roll dice, advancing to the next bucket 50% of the time. That
     * means we'll hit the maximum roughly every 1K calls.
     */
    for (i = 0;;)
        if (rng(rnd) & 0x1 || ++i > 9)
            break;

    if (i == 0)
        __wt_yield();
    else {
        micro_seconds = (uint64_t)max_seconds * WT_MILLION;
        __wt_sleep(0, i * (micro_seconds / 10));
    }
}

static inline void
wiredtiger_begin_transaction(WT_SESSION *session, const char *config)
{
    WT_DECL_RET;

    /*
     * Keep trying to start a new transaction if it's timing out. There are no resources pinned, it
     * should succeed eventually.
     */
    while ((ret = session->begin_transaction(session, config)) == WT_CACHE_FULL)
        __wt_yield();
    testutil_check(ret);
}
