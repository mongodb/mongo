/*-
 * Public Domain 2014-2020 MongoDB, Inc.
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

#include "format.h"

void
track(const char *tag, uint64_t cnt, TINFO *tinfo)
{
    static size_t lastlen = 0;
    size_t len;
    char msg[128];

    if (g.c_quiet || tag == NULL)
        return;

    if (tinfo == NULL && cnt == 0)
        testutil_check(
          __wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s", g.run_cnt, tag));
    else if (tinfo == NULL)
        testutil_check(__wt_snprintf_len_set(
          msg, sizeof(msg), &len, "%4" PRIu32 ": %s: %" PRIu64, g.run_cnt, tag, cnt));
    else
        testutil_check(__wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s: "
                                                                     "search %" PRIu64 "%s, "
                                                                     "insert %" PRIu64 "%s, "
                                                                     "update %" PRIu64 "%s, "
                                                                     "remove %" PRIu64 "%s",
          g.run_cnt, tag, tinfo->search > M(9) ? tinfo->search / M(1) : tinfo->search,
          tinfo->search > M(9) ? "M" : "",
          tinfo->insert > M(9) ? tinfo->insert / M(1) : tinfo->insert,
          tinfo->insert > M(9) ? "M" : "",
          tinfo->update > M(9) ? tinfo->update / M(1) : tinfo->update,
          tinfo->update > M(9) ? "M" : "",
          tinfo->remove > M(9) ? tinfo->remove / M(1) : tinfo->remove,
          tinfo->remove > M(9) ? "M" : ""));

    if (lastlen > len) {
        memset(msg + len, ' ', (size_t)(lastlen - len));
        msg[lastlen] = '\0';
    }
    lastlen = len;

    if (printf("%s\r", msg) < 0)
        testutil_die(EIO, "printf");
    if (fflush(stdout) == EOF)
        testutil_die(errno, "fflush");
}

/*
 * path_setup --
 *     Build the standard paths and shell commands we use.
 */
void
path_setup(const char *home)
{
    size_t len;
    const char *name;

    /* Home directory. */
    g.home = dstrdup(home == NULL ? "RUNDIR" : home);

    /* Configuration file. */
    name = "CONFIG";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_config = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_config, len, "%s/%s", g.home, name));

    /* Key length configuration file. */
    name = "CONFIG.keylen";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_key = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_key, len, "%s/%s", g.home, name));

    /* RNG log file. */
    name = "CONFIG.rand";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_rand = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_rand, len, "%s/%s", g.home, name));

    /* Log file. */
    name = "OPERATIONS.log";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_log = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_log, len, "%s/%s", g.home, name));

    /* History store dump file. */
    name = "FAIL.HSdump";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_hsdump = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_hsdump, len, "%s/%s", g.home, name));

    /* Page dump file. */
    name = "FAIL.pagedump";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_pagedump = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_pagedump, len, "%s/%s", g.home, name));

    /* Statistics file. */
    name = "OPERATIONS.stats";
    len = strlen(g.home) + strlen(name) + 2;
    g.home_stats = dmalloc(len);
    testutil_check(__wt_snprintf(g.home_stats, len, "%s/%s", g.home, name));
}

/*
 * fp_readv --
 *     Read and return a value from a file.
 */
bool
fp_readv(FILE *fp, char *name, bool eof_ok, uint32_t *vp)
{
    u_long ulv;
    char *endptr, buf[100];

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        if (feof(g.randfp)) {
            if (eof_ok)
                return (true);
            testutil_die(errno, "%s: read-value EOF", name);
        }
        testutil_die(errno, "%s: read-value error", name);
    }

    errno = 0;
    ulv = strtoul(buf, &endptr, 10);
    testutil_assert(errno == 0 && endptr[0] == '\n');
    testutil_assert(ulv <= UINT32_MAX);
    *vp = (uint32_t)ulv;
    return (false);
}

/*
 * rng_slow --
 *     Return a random number, doing the real work.
 */
uint32_t
rng_slow(WT_RAND_STATE *rnd)
{
    uint32_t v;

    /*
     * We can reproduce a single-threaded run based on the random numbers used in the initial run,
     * plus the configuration files.
     */
    if (g.replay) {
        if (fp_readv(g.randfp, g.home_rand, true, &v)) {
            fprintf(stderr,
              "\n"
              "end of random number log reached\n");
            exit(EXIT_SUCCESS);
        }
        return (v);
    }

    v = __wt_random(rnd);

    /* Save and flush the random number so we're up-to-date on error. */
    (void)fprintf(g.randfp, "%" PRIu32 "\n", v);
    (void)fflush(g.randfp);

    return (v);
}

/*
 * handle_init --
 *     Initialize logging/random number handles for a run.
 */
void
handle_init(void)
{
    /* Open/truncate logging/random number handles. */
    if (g.logging && (g.logfp = fopen(g.home_log, "w")) == NULL)
        testutil_die(errno, "fopen: %s", g.home_log);
    if ((g.randfp = fopen(g.home_rand, g.replay ? "r" : "w")) == NULL)
        testutil_die(errno, "%s", g.home_rand);
}

/*
 * handle_teardown --
 *     Shutdown logging/random number handles for a run.
 */
void
handle_teardown(void)
{
    /* Flush/close logging/random number handles. */
    fclose_and_clear(&g.logfp);
    fclose_and_clear(&g.randfp);
}

/*
 * fclose_and_clear --
 *     Close a file and clear the handle so we don't close twice.
 */
void
fclose_and_clear(FILE **fpp)
{
    FILE *fp;

    if ((fp = *fpp) == NULL)
        return;
    *fpp = NULL;
    if (fclose(fp) != 0)
        testutil_die(errno, "fclose");
}

/*
 * timestamp_once --
 *     Update the timestamp once.
 */
void
timestamp_once(WT_SESSION *session)
{
    static const char *oldest_timestamp_str = "oldest_timestamp=";
    WT_CONNECTION *conn;
    WT_DECL_RET;
    char buf[WT_TS_HEX_STRING_SIZE + 64];

    conn = g.wts_conn;

    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s", oldest_timestamp_str));

    /*
     * Lock out transaction timestamp operations. The lock acts as a barrier ensuring we've checked
     * if the workers have finished, we don't want that line reordered. We can also be called from
     * places, such as bulk load, where we are single-threaded and the locks haven't been
     * initialized.
     */
    if (LOCK_INITIALIZED(&g.ts_lock))
        lock_writelock(session, &g.ts_lock);

    ret = conn->query_timestamp(conn, buf + strlen(oldest_timestamp_str), "get=all_durable");
    testutil_assert(ret == 0 || ret == WT_NOTFOUND);
    if (ret == 0)
        testutil_check(conn->set_timestamp(conn, buf));

    if (LOCK_INITIALIZED(&g.ts_lock))
        lock_writeunlock(session, &g.ts_lock);
}

/*
 * timestamp --
 *     Periodically update the oldest timestamp.
 */
WT_THREAD_RET
timestamp(void *arg)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    bool done;

    (void)(arg);
    conn = g.wts_conn;

    /* Locks need session */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Update the oldest timestamp at least once every 15 seconds. */
    done = false;
    do {
        /*
         * Do a final bump of the oldest timestamp as part of shutting down the worker threads,
         * otherwise recent operations can prevent verify from running.
         */
        if (g.workers_finished)
            done = true;
        else
            random_sleep(&g.rnd, 15);

        timestamp_once(session);

    } while (!done);

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * alter --
 *     Periodically alter a table's metadata.
 */
WT_THREAD_RET
alter(void *arg)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    u_int period;
    char buf[32];
    bool access_value;

    (void)(arg);
    conn = g.wts_conn;

    /*
     * Only alter the access pattern hint. If we alter the cache resident setting we may end up with
     * a setting that fills cache and doesn't allow it to be evicted.
     */
    access_value = false;

    /* Open a session */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    while (!g.workers_finished) {
        period = mmrand(NULL, 1, 10);

        testutil_check(__wt_snprintf(
          buf, sizeof(buf), "access_pattern_hint=%s", access_value ? "random" : "none"));
        access_value = !access_value;
        /*
         * Alter can return EBUSY if concurrent with other operations.
         */
        while ((ret = session->alter(session, g.uri, buf)) != 0 && ret != EBUSY)
            testutil_die(ret, "session.alter");
        while (period > 0 && !g.workers_finished) {
            --period;
            __wt_sleep(1, 0);
        }
    }

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * lock_init --
 *     Initialize abstract lock that can use either pthread of wt reader-writer locks.
 */
void
lock_init(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(lock->lock_type == LOCK_NONE);

    if (g.c_wt_mutex) {
        testutil_check(__wt_rwlock_init((WT_SESSION_IMPL *)session, &lock->l.wt));
        lock->lock_type = LOCK_WT;
    } else {
        testutil_check(pthread_rwlock_init(&lock->l.pthread, NULL));
        lock->lock_type = LOCK_PTHREAD;
    }
}

/*
 * lock_destroy --
 *     Destroy abstract lock.
 */
void
lock_destroy(WT_SESSION *session, RWLOCK *lock)
{
    testutil_assert(LOCK_INITIALIZED(lock));

    if (lock->lock_type == LOCK_WT) {
        __wt_rwlock_destroy((WT_SESSION_IMPL *)session, &lock->l.wt);
    } else {
        testutil_check(pthread_rwlock_destroy(&lock->l.pthread));
    }
    lock->lock_type = LOCK_NONE;
}
