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

#define FORMAT_PREPARE_TIMEOUT 120

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
            testutil_assertfmt(now < start || now - start < FORMAT_PREPARE_TIMEOUT,
              "%s: timed out with prepare-conflict", "WT_CURSOR.next");
        }
        break;
    case PREV:
        while ((ret = cursor->prev(cursor)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < FORMAT_PREPARE_TIMEOUT,
              "%s: timed out with prepare-conflict", "WT_CURSOR.prev");
        }
        break;
    case SEARCH:
        while ((ret = cursor->search(cursor)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < FORMAT_PREPARE_TIMEOUT,
              "%s: timed out with prepare-conflict", "WT_CURSOR.search");
        }
        break;
    case SEARCH_NEAR:
        while ((ret = cursor->search_near(cursor, exactp)) == WT_PREPARE_CONFLICT) {
            __wt_yield();

            /* Ignore clock reset. */
            __wt_seconds(NULL, &now);
            testutil_assertfmt(now < start || now - start < FORMAT_PREPARE_TIMEOUT,
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

/*
 * tables_apply -
 *	Call an underlying function on all tables.
 */
static inline void
tables_apply(void (*func)(TABLE *, void *), void *arg)
{
    u_int i;

    if (ntables == 0)
        func(tables[0], arg);
    else
        for (i = 1; i <= ntables; ++i)
            func(tables[i], arg);
}

/*
 * table_maxv --
 *     Return the maximum value for a table configuration variable.
 */
static inline uint32_t
table_maxv(u_int off)
{
    uint32_t v;
    u_int i;

    if (ntables == 0)
        return (tables[0]->v[off].v);

    for (v = 0, i = 1; i <= ntables; ++i)
        v = WT_MAX(v, tables[i]->v[off].v);
    return (v);
}

/*
 * table_sumv --
 *     Return the summed value for a table configuration variable.
 */
static inline uint32_t
table_sumv(u_int off)
{
    uint32_t v;
    u_int i;

    if (ntables == 0)
        return (tables[0]->v[off].v);

    for (v = 0, i = 1; i <= ntables; ++i)
        v += tables[i]->v[off].v;
    return (v);
}

/*
 * table_select --
 *     Randomly select a table.
 */
static inline TABLE *
table_select(TINFO *tinfo)
{
    if (ntables == 0)
        return (tables[0]);

    return (tables[mmrand(tinfo == NULL ? NULL : &tinfo->rnd, 1, ntables)]);
}

/*
 * table_select_type --
 *     Randomly select a table of a specific type.
 */
static inline TABLE *
table_select_type(table_type type)
{
    u_int i;

    if (ntables == 0)
        return (tables[0]->type == type ? tables[0] : NULL);

    for (i = mmrand(NULL, 1, ntables);; ++i) {
        if (i > ntables)
            i = 1;
        if (tables[i]->type == type)
            break;
    }
    return (tables[i]);
}

/*
 * wiredtiger_open_cursor --
 *     Open a WiredTiger cursor.
 */
static inline void
wiredtiger_open_cursor(
  WT_SESSION *session, const char *uri, const char *config, WT_CURSOR **cursorp)
{
    WT_DECL_RET;

    *cursorp = NULL;

    /* WT_SESSION.open_cursor can return EBUSY if concurrent with a metadata operation, retry. */
    while ((ret = session->open_cursor(session, uri, NULL, config, cursorp)) == EBUSY)
        __wt_yield();
    testutil_checkfmt(ret, "%s", uri);
}

/*
 * table_cursor --
 *     Return the cursor for a table, opening a new one if necessary.
 */
static inline WT_CURSOR *
table_cursor(TINFO *tinfo, u_int id)
{
    TABLE *table;
    const char *config;

    testutil_assert(id > 0);

    /* The table ID is 1-based, the cursor array is 0-based. */
    table = tables[ntables == 0 ? 0 : id];
    --id;

    if (tinfo->cursors[id] == NULL) {
        /* Configure "append", in the case of column stores, we append when inserting new rows. */
        config = table->type == ROW ? NULL : "append";
        wiredtiger_open_cursor(tinfo->session, table->uri, config, &tinfo->cursors[id]);
    }
    return (tinfo->cursors[id]);
}

/*
 * wiredtiger_begin_transaction --
 *     Start a WiredTiger transaction.
 */
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
key_gen(TABLE *table, WT_ITEM *key, uint64_t keyno)
{
    key_gen_common(table, key, keyno, "00");
}

/*
 * key_gen_insert --
 *     Generate a key for insertion.
 */
static inline void
key_gen_insert(TABLE *table, WT_RAND_STATE *rnd, WT_ITEM *key, uint64_t keyno)
{
    static const char *const suffix[15] = {
      "01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13", "14", "15"};

    key_gen_common(table, key, keyno, suffix[mmrand(rnd, 0, 14)]);
}

/*
 * lock_try_writelock
 *     Try to get exclusive lock.  Fail immediately if not available.
 */
static inline int
lock_try_writelock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT)
        return (__wt_try_writelock((WT_SESSION_IMPL *)session, &lock->l.wt));
    return (pthread_rwlock_trywrlock(&lock->l.pthread));
}

/*
 * lock_writelock --
 *     Wait to get exclusive lock.
 */
static inline void
lock_writelock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT)
        __wt_writelock((WT_SESSION_IMPL *)session, &lock->l.wt);
    else
        testutil_check(pthread_rwlock_wrlock(&lock->l.pthread));
}

/*
 * lock_writeunlock --
 *     Release an exclusive lock.
 */
static inline void
lock_writeunlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT)
        __wt_writeunlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    else
        testutil_check(pthread_rwlock_unlock(&lock->l.pthread));
}

/*
 * lock_readlock --
 *     Wait to get read lock.
 */
static inline void
lock_readlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT)
        __wt_readlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    else
        testutil_check(pthread_rwlock_rdlock(&lock->l.pthread));
}

/*
 * lock_writeunlock --
 *     Release an exclusive lock.
 */
static inline void
lock_readunlock(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT)
        __wt_readunlock((WT_SESSION_IMPL *)session, &lock->l.wt);
    else
        testutil_check(pthread_rwlock_unlock(&lock->l.pthread));
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
#define trace_op(tinfo, fmt, ...)                                                             \
    do {                                                                                      \
        if (g.trace) {                                                                        \
            struct timespec __ts;                                                             \
            WT_SESSION *__s = (tinfo)->trace;                                                 \
            __wt_epoch((WT_SESSION_IMPL *)__s, &__ts);                                        \
            testutil_check(__s->log_printf(__s, "[%" PRIuMAX ":%" PRIuMAX "][%s]:%s " fmt,    \
              (uintmax_t)__ts.tv_sec, (uintmax_t)__ts.tv_nsec / WT_THOUSAND, (tinfo)->tidbuf, \
              (tinfo)->table->uri, __VA_ARGS__));                                             \
        }                                                                                     \
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
