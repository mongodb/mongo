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
    uint64_t start, now;

    /*
     * Read operations wait out prepare-conflicts. (As part of the snapshot isolation checks, we
     * repeat reads that succeeded before, they should be repeatable.)
     */
    __wt_seconds(NULL, &start);
    switch (op) {
    case NEXT:
        while ((ret = cursor->next(cursor)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < 60,
              "%s: timed out with prepare-conflict", "WT_CURSOR.next");
        }
        break;
    case PREV:
        while ((ret = cursor->prev(cursor)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < 60,
              "%s: timed out with prepare-conflict", "WT_CURSOR.prev");
        }
        break;
    case SEARCH:
        while ((ret = cursor->search(cursor)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < 60,
              "%s: timed out with prepare-conflict", "WT_CURSOR.search");
        }
        break;
    case SEARCH_NEAR:
        while ((ret = cursor->search_near(cursor, exactp)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < 60,
              "%s: timed out with prepare-conflict", "WT_CURSOR.search_near");
        }
        break;
    }
    return (ret);
}

/*
 * rng --
 *     Return a random number.
 */
static inline uint32_t
rng(WT_RAND_STATE *rnd)
{
    /* Threaded operations have their own RNG information, otherwise we use the default. */
    if (rnd == NULL)
        rnd = &g.rnd;

    return (__wt_random(rnd));
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

/*
 * key_gen --
 *     Generate a key for lookup.
 */
static inline void
key_gen(WT_ITEM *key, uint64_t keyno)
{
    key_gen_common(key, keyno, "00");
}

/*
 * key_gen_insert --
 *     Generate a key for insertion.
 */
static inline void
key_gen_insert(WT_RAND_STATE *rnd, WT_ITEM *key, uint64_t keyno)
{
    static const char *const suffix[15] = {
      "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15"};

    key_gen_common(key, keyno, suffix[mmrand(rnd, 0, 14)]);
}

/*
 * lock_try_writelock
 *     Try to get exclusive lock.  Fail immediately if not available.
 */
static inline int
lock_try_writelock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        return (__wt_try_writelock((WT_SESSION_IMPL *)session, &lock->l.wt));
    } else {
        return (pthread_rwlock_trywrlock(&lock->l.pthread));
    }
}

/*
 * lock_writelock --
 *     Wait to get exclusive lock.
 */
static inline void
lock_writelock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        __wt_writelock((WT_SESSION_IMPL *)session, &lock->l.wt);
    } else {
        testutil_check(pthread_rwlock_wrlock(&lock->l.pthread));
    }
}

/*
 * lock_writeunlock --
 *     Release an exclusive lock.
 */
static inline void
lock_writeunlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        __wt_writeunlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    } else {
        testutil_check(pthread_rwlock_unlock(&lock->l.pthread));
    }
}

/*
 * lock_readlock --
 *     Wait to get read lock.
 */
static inline void
lock_readlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        __wt_readlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    } else {
        testutil_check(pthread_rwlock_rdlock(&lock->l.pthread));
    }
}

/*
 * lock_writeunlock --
 *     Release an exclusive lock.
 */
static inline void
lock_readunlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        __wt_readunlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    } else {
        testutil_check(pthread_rwlock_unlock(&lock->l.pthread));
    }
}

#define trace_msg(fmt, ...)                                                                        \
    do {                                                                                           \
        if (g.trace) {                                                                             \
            struct timespec __ts;                                                                  \
            WT_SESSION *__s = g.trace_session;                                                     \
            __wt_epoch((WT_SESSION_IMPL *)__s, &__ts);                                             \
            testutil_check(                                                                        \
              __s->log_printf(__s, "[%" PRIuMAX ":%" PRIuMAX "][%s] " fmt, (uintmax_t)__ts.tv_sec, \
                (uintmax_t)__ts.tv_nsec / WT_THOUSAND, g.tidbuf, __VA_ARGS__));                    \
        }                                                                                          \
    } while (0)
#define trace_op(tinfo, fmt, ...)                                                                  \
    do {                                                                                           \
        if (g.trace) {                                                                             \
            struct timespec __ts;                                                                  \
            WT_SESSION *__s = (tinfo)->trace;                                                      \
            __wt_epoch((WT_SESSION_IMPL *)__s, &__ts);                                             \
            testutil_check(                                                                        \
              __s->log_printf(__s, "[%" PRIuMAX ":%" PRIuMAX "][%s] " fmt, (uintmax_t)__ts.tv_sec, \
                (uintmax_t)__ts.tv_nsec / WT_THOUSAND, tinfo->tidbuf, __VA_ARGS__));               \
        }                                                                                          \
    } while (0)

/*
 * trace_bytes --
 *     Return a byte string formatted for display.
 */
static inline const char *
trace_bytes(TINFO *tinfo, const uint8_t *data, size_t size)
{
    testutil_check(
      __wt_raw_to_esc_hex((WT_SESSION_IMPL *)tinfo->session, data, size, &tinfo->vprint));
    return (tinfo->vprint.mem);
}
#define trace_item(tinfo, buf) trace_bytes(tinfo, (buf)->data, (buf)->size)
