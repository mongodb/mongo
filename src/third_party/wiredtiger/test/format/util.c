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

/*
 * track_ts_diff --
 *     Return a one character descriptor of relative timestamp values.
 */
static const char *
track_ts_diff(uint64_t left_ts, uint64_t right_ts)
{
    if (left_ts < right_ts)
        return "+";
    else if (left_ts == right_ts)
        return "=";
    else
        return "-";
}

/*
 * track_ts_dots --
 *     Return an entry in the time stamp progress indicator.
 */
static const char *
track_ts_dots(u_int dot_count)
{
    static const char *dots[] = {"   ", ".  ", ".. ", "..."};

    return (dots[dot_count % WT_ELEMENTS(dots)]);
}

/*
 * track --
 *     Show a status line of operations and time stamp progress.
 */
void
track(const char *tag, uint64_t cnt, TINFO *tinfo)
{
    static size_t last_len;
    static uint64_t last_cur, last_old, last_stable;
    static u_int cur_dot_cnt, old_dot_cnt, stable_dot_cnt;
    size_t len;
    uint64_t cur_ts, old_ts, stable_ts;
    char msg[128], ts_msg[64];

    if (g.c_quiet || tag == NULL)
        return;

    if (tinfo == NULL && cnt == 0)
        testutil_check(
          __wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s", g.run_cnt, tag));
    else if (tinfo == NULL)
        testutil_check(__wt_snprintf_len_set(
          msg, sizeof(msg), &len, "%4" PRIu32 ": %s: %" PRIu64, g.run_cnt, tag, cnt));
    else {
        ts_msg[0] = '\0';
        if (g.c_txn_timestamps) {
            /*
             * Don't worry about having a completely consistent set of timestamps.
             */
            old_ts = g.oldest_timestamp;
            stable_ts = g.stable_timestamp;
            cur_ts = g.timestamp;

            if (old_ts != last_old) {
                ++old_dot_cnt;
                last_old = old_ts;
            }
            if (stable_ts != last_stable) {
                ++stable_dot_cnt;
                last_stable = stable_ts;
            }
            if (cur_ts != last_cur) {
                ++cur_dot_cnt;
                last_cur = cur_ts;
            }

            if (g.c_txn_rollback_to_stable)
                testutil_check(__wt_snprintf(ts_msg, sizeof(ts_msg),
                  " old%s"
                  "stb%s%s"
                  "ts%s%s",
                  track_ts_dots(old_dot_cnt), track_ts_diff(old_ts, stable_ts),
                  track_ts_dots(stable_dot_cnt), track_ts_diff(stable_ts, cur_ts),
                  track_ts_dots(cur_dot_cnt)));
            else
                testutil_check(__wt_snprintf(ts_msg, sizeof(ts_msg),
                  " old%s"
                  "ts%s%s",
                  track_ts_dots(old_dot_cnt), track_ts_diff(old_ts, cur_ts),
                  track_ts_dots(cur_dot_cnt)));
        }
        testutil_check(__wt_snprintf_len_set(msg, sizeof(msg), &len, "%4" PRIu32 ": %s: "
                                                                     "S %" PRIu64 "%s, "
                                                                     "I %" PRIu64 "%s, "
                                                                     "U %" PRIu64 "%s, "
                                                                     "R %" PRIu64 "%s%s",
          g.run_cnt, tag, tinfo->search > M(9) ? tinfo->search / M(1) : tinfo->search,
          tinfo->search > M(9) ? "M" : "",
          tinfo->insert > M(9) ? tinfo->insert / M(1) : tinfo->insert,
          tinfo->insert > M(9) ? "M" : "",
          tinfo->update > M(9) ? tinfo->update / M(1) : tinfo->update,
          tinfo->update > M(9) ? "M" : "",
          tinfo->remove > M(9) ? tinfo->remove / M(1) : tinfo->remove,
          tinfo->remove > M(9) ? "M" : "", ts_msg));
    }
    if (last_len > len) {
        memset(msg + len, ' ', (size_t)(last_len - len));
        msg[last_len] = '\0';
    }
    last_len = len;

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
fp_readv(FILE *fp, char *name, uint32_t *vp)
{
    u_long ulv;
    char *endptr, buf[100];

    if (fgets(buf, sizeof(buf), fp) == NULL)
        testutil_die(errno, "%s: read-value error", name);

    errno = 0;
    ulv = strtoul(buf, &endptr, 10);
    testutil_assert(errno == 0 && endptr[0] == '\n');
    testutil_assert(ulv <= UINT32_MAX);
    *vp = (uint32_t)ulv;
    return (false);
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
timestamp_once(WT_SESSION *session, bool allow_lag)
{
    static const char *oldest_timestamp_str = "oldest_timestamp=";
    static const char *stable_timestamp_str = "stable_timestamp=";
    WT_CONNECTION *conn;
    WT_DECL_RET;
    size_t len;
    uint64_t all_durable, stable;
    char buf[WT_TS_HEX_STRING_SIZE * 2 + 64], tsbuf[WT_TS_HEX_STRING_SIZE];

    conn = g.wts_conn;
    stable = 0ULL;

    /*
     * Lock out transaction timestamp operations. The lock acts as a barrier ensuring we've checked
     * if the workers have finished, we don't want that line reordered. We can also be called from
     * places, such as bulk load, where we are single-threaded and the locks haven't been
     * initialized.
     */
    if (LOCK_INITIALIZED(&g.ts_lock))
        lock_writelock(session, &g.ts_lock);

    if ((ret = conn->query_timestamp(conn, tsbuf, "get=all_durable")) == 0) {
        timestamp_parse(session, tsbuf, &all_durable);

        /*
         * If a lag is permitted, move the oldest timestamp half the way to the current
         * "all_durable" timestamp.
         */
        if (allow_lag)
            g.oldest_timestamp = (all_durable + g.oldest_timestamp) / 2;
        else
            g.oldest_timestamp = all_durable;
        testutil_check(
          __wt_snprintf(buf, sizeof(buf), "%s%" PRIx64, oldest_timestamp_str, g.oldest_timestamp));

        /*
         * When we're doing rollback to stable operations, we'll advance the stable timestamp to the
         * current timestamp value.
         */
        if (g.c_txn_rollback_to_stable) {
            stable = g.timestamp;
            len = strlen(buf);
            WT_ASSERT((WT_SESSION_IMPL *)session, len < sizeof(buf));
            testutil_check(__wt_snprintf(
              buf + len, sizeof(buf) - len, ",%s%" PRIx64, stable_timestamp_str, stable));
        }
        testutil_check(conn->set_timestamp(conn, buf));
        trace_msg("%-10s oldest=%" PRIu64 ", stable=%" PRIu64, "setts", g.oldest_timestamp, stable);
        if (g.c_txn_rollback_to_stable)
            g.stable_timestamp = stable;

    } else
        testutil_assert(ret == WT_NOTFOUND);

    if (LOCK_INITIALIZED(&g.ts_lock))
        lock_writeunlock(session, &g.ts_lock);
}

/*
 * timestamp_parse --
 *     Parse a timestamp to an integral value.
 */
void
timestamp_parse(WT_SESSION *session, const char *str, uint64_t *tsp)
{
    char *p;

    *tsp = strtoull(str, &p, 16);
    WT_ASSERT((WT_SESSION_IMPL *)session, p - str <= 16);
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
        random_sleep(&g.rnd, 15);

        /*
         * If running without rollback_to_stable, do a final bump of the oldest timestamp as part of
         * shutting down the worker threads, otherwise recent operations can prevent verify from
         * running.
         *
         * With rollback_to_stable configured, don't do a bump at the end of the run. We need the
         * worker threads to have time to see any changes in the stable timestamp, so they can stash
         * their stable state - if we bump they will have no time to do that. And when we rollback,
         * we'd like to see a reasonable amount of data changed. So we don't bump the stable
         * timestamp, and we can't bump the oldest timestamp as well, as it would get ahead of the
         * stable timestamp, which is not allowed.
         */
        if (g.workers_finished)
            done = true;

        if (!done || !g.c_txn_rollback_to_stable) {
            timestamp_once(session, true);
            if (done)
                timestamp_once(session, true);
        }

    } while (!done);

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
