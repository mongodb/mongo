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

#include "test_checkpoint.h"

static WT_THREAD_RET checkpointer(void *);
static WT_THREAD_RET clock_thread(void *);
static int compare_cursors(WT_CURSOR *, table_type, WT_CURSOR *, table_type);
static int diagnose_key_error(WT_CURSOR *, table_type, int, WT_CURSOR *, table_type, int);
static int real_checkpointer(THREAD_DATA *);

/*
 * set_stable --
 *     Set the given timestamp as the stable timestamp.
 */
static void
set_stable(uint64_t stable_ts)
{
    char buf[128];

    if (g.race_timestamps)
        testutil_snprintf(buf, sizeof(buf),
          "stable_timestamp=%" PRIx64 ",oldest_timestamp=%" PRIx64, stable_ts, stable_ts);
    else
        testutil_snprintf(buf, sizeof(buf), "stable_timestamp=%" PRIx64, stable_ts);
    testutil_check(g.conn->set_timestamp(g.conn, buf));
}

/*
 * start_threads --
 *     Responsible for creating the service threads.
 */
void
start_threads(void)
{
    /*
     * Ensure we are not going backwards and start with 1 as the stable as 0 is not a valid
     * timestamp.
     */
    set_stable(WT_MAX(1, g.ts_stable));

    /*
     * If there are N worker threads (0 - N-1), the checkpoint thread has an ID of N and the clock
     * thread an ID of N + 1.
     */
    testutil_check(__wt_thread_create(NULL, &g.checkpoint_thread, checkpointer, &g.td[g.nworkers]));
    if (g.use_timestamps) {
        testutil_check(__wt_rwlock_init(NULL, &g.clock_lock));
        testutil_check(
          __wt_thread_create(NULL, &g.clock_thread, clock_thread, &g.td[g.nworkers + 1]));
    }
}

/*
 * end_threads --
 *     Responsible for cleanly shutting down the service threads.
 */
void
end_threads(void)
{
    /* Shutdown checkpoint after flush thread completes because flush depends on checkpoint. */
    testutil_check(__wt_thread_join(NULL, &g.checkpoint_thread));

    if (g.use_timestamps) {
        /*
         * The clock lock is also used by the checkpoint thread. Now that it has exited it is safe
         * to destroy that lock.
         */
        testutil_check(__wt_thread_join(NULL, &g.clock_thread));
        __wt_rwlock_destroy(NULL, &g.clock_lock);
    }
}

/*
 * get_all_committed_ts --
 *     Returns the least of commit timestamps across all the threads. Returns UINT64_MAX if one of
 *     the threads has not yet started.
 */
static uint64_t
get_all_committed_ts(void)
{
    uint64_t ret;
    int i;

    ret = UINT64_MAX;
    for (i = 0; i < g.nworkers; ++i) {
        if (g.td[i].ts < ret)
            ret = g.td[i].ts;
        if (ret == 0)
            return (UINT64_MAX);
    }

    return (ret);
}

/*
 * clock_thread --
 *     Clock thread: ticks up timestamps.
 */
static WT_THREAD_RET
clock_thread(void *arg)
{
    WT_SESSION *wt_session;
    WT_SESSION_IMPL *session;
    THREAD_DATA *td;
    uint64_t delay, last_ts, oldest_ts;
    char tid[128];

    testutil_check(__wt_thread_str(tid, sizeof(tid)));
    printf("clock thread starting: tid: %s\n", tid);
    fflush(stdout);

    td = (THREAD_DATA *)arg;
    last_ts = 0;

    testutil_check(g.conn->open_session(g.conn, NULL, NULL, &wt_session));
    session = (WT_SESSION_IMPL *)wt_session;

    while (g.opts.running) {
        if (g.predictable_replay) {
            oldest_ts = get_all_committed_ts();
            if (oldest_ts != UINT64_MAX && oldest_ts - last_ts > PRED_REPLAY_STABLE_PERIOD) {
                /*
                 * If we are doing a predictable rerun, don't go past the provided stop timestamp.
                 */
                if (g.stop_ts > 0 && oldest_ts >= g.stop_ts) {
                    printf("Clock thread at %" PRIu64
                           " has reached the provided stop timestamp. "
                           "Stopping the clock.\n",
                      g.stop_ts);
                    set_stable(g.stop_ts);
                    break;
                }
                set_stable(oldest_ts);
                last_ts = oldest_ts;
            }
        } else {
            __wt_writelock(session, &g.clock_lock);
            if (g.prepare)
                /*
                 * Leave a gap between timestamps so prepared insert followed by remove don't
                 * overlap with stable timestamp.
                 */
                g.ts_stable += 5;
            else
                ++g.ts_stable;
            set_stable(g.ts_stable);
            if (g.ts_stable % 997 == 0) {
                /*
                 * Random value between 6 and 10 seconds.
                 */
                delay = __wt_random(&td->extra_rnd) % 5;
                __wt_sleep(delay + 6, 0);
            }
            __wt_writeunlock(session, &g.clock_lock);
        }
        /*
         * Random value between 5000 and 10000.
         */
        delay = __wt_random(&td->extra_rnd) % 5001;
        __wt_sleep(0, delay + 5 * WT_THOUSAND);
    }

    testutil_check(wt_session->close(wt_session, NULL));

    return (WT_THREAD_RET_VALUE);
}

/*
 * checkpointer --
 *     Checkpoint thread start function.
 */
static WT_THREAD_RET
checkpointer(void *arg)
{
    char tid[128];

    testutil_check(__wt_thread_str(tid, sizeof(tid)));
    printf("checkpointer thread starting: tid: %s\n", tid);
    fflush(stdout);

    (void)real_checkpointer((THREAD_DATA *)arg);
    return (WT_THREAD_RET_VALUE);
}

/*
 * set_flush_tier_delay --
 *     Set up a random delay for the next flush_tier.
 */
void
set_flush_tier_delay(WT_RAND_STATE *rnd)
{
    /*
     * When we are in sweep stress mode, we checkpoint between 4 and 8 seconds, so we'll flush
     * between 5 and 15 seconds (that is, 5 million and 15 million microseconds). When we aren't in
     * sweep stress mode, we are checkpointing constantly, and we'll do a flush tier with a random
     * delay between 0 - 10000 microseconds.
     */
    if (g.sweep_stress)
        g.opts.tiered_flush_interval_us = 5 * WT_MILLION + __wt_random(rnd) % (10 * WT_MILLION);
    else
        g.opts.tiered_flush_interval_us = __wt_random(rnd) % 10001;
}

/*
 * real_checkpointer --
 *     Do the work of creating checkpoints and then verifying them. Also responsible for finishing
 *     in a timely fashion.
 */
static int
real_checkpointer(THREAD_DATA *td)
{
    WT_SESSION *session;
    wt_timestamp_t stable_ts, oldest_ts, verify_ts;
    uint64_t delay, tmp_ts;
    int ret;
    char buf[128], flush_tier_config[128], timestamp_buf[64];
    const char *checkpoint_config, *ts_config;
    bool flush_tier;

    ts_config = "use_timestamp=false";
    verify_ts = WT_TS_NONE;
    flush_tier = false;

    while (g.ntables > g.ntables_created && g.opts.running)
        __wt_yield();

    if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0)
        return (log_print_err("conn.open_session", ret, 1));

    if (g.use_timestamps)
        ts_config = "use_timestamp=true";

    if (!WT_PREFIX_MATCH(g.checkpoint_name, "WiredTigerCheckpoint")) {
        testutil_snprintf(buf, sizeof(buf), "name=%s,%s", g.checkpoint_name, ts_config);
        checkpoint_config = buf;
    } else
        checkpoint_config = ts_config;

    testutil_snprintf(
      flush_tier_config, sizeof(flush_tier_config), "flush_tier=(enabled,force),%s", ts_config);

    /* Use the extra random generator as the tier delay doesn't affect the actual data content. */
    set_flush_tier_delay(&td->extra_rnd);

    while (g.opts.running) {
        /*
         * Check for consistency of online data, here we don't expect to see the version at the
         * checkpoint just a consistent view across all tables.
         */
        if ((ret = verify_consistency(session, WT_TS_NONE, false)) != 0)
            return (log_print_err("verify_consistency (online)", ret, 1));

        if (g.use_timestamps) {
            testutil_check(g.conn->query_timestamp(g.conn, timestamp_buf, "get=stable_timestamp"));
            stable_ts = testutil_timestamp_parse(timestamp_buf);
            oldest_ts = g.ts_oldest;
            if (stable_ts <= oldest_ts)
                verify_ts = stable_ts;
            else
                /* Use the extra random generator as the data is not getting modified. */
                verify_ts = __wt_random(&td->extra_rnd) % (stable_ts - oldest_ts + 1) + oldest_ts;
            if (g.predictable_replay) {
                tmp_ts = WT_MIN(get_all_committed_ts(), stable_ts);
                /* Update the oldest timestamp, but do not go past the provided stop timestamp. */
                if (tmp_ts != UINT64_MAX && (g.stop_ts == 0 || tmp_ts <= g.stop_ts))
                    g.ts_oldest = tmp_ts;
                if (g.stop_ts > 0 && stable_ts >= g.stop_ts) {
                    printf(
                      "The checkpoint thread has reached the stop timestamp of "
                      "%" PRIu64 ". Finish the test run.\n",
                      g.stop_ts);
                    g.opts.running = false;
                }
            } else {
                __wt_writelock((WT_SESSION_IMPL *)session, &g.clock_lock);
                g.ts_oldest = g.ts_stable;
                __wt_writeunlock((WT_SESSION_IMPL *)session, &g.clock_lock);
            }
        }

        /* Execute a checkpoint */
        if ((ret = session->checkpoint(
               session, flush_tier ? flush_tier_config : checkpoint_config)) != 0)
            return (log_print_err("session.checkpoint", ret, 1));
        printf("Finished a checkpoint\n");
        fflush(stdout);
        if (flush_tier) {
            /*
             * FIXME: when we change the API to notify that a flush_tier has completed, we'll need
             * to set up a general event handler and catch that notification, so we can pass the
             * flush_tier "cookie" to the test utility function.
             */
            testutil_tiered_flush_complete(&g.opts, session, NULL);
            flush_tier = false;
            printf("Finished a flush_tier\n");

            /*
             * Use the extra random generator as the tier delay doesn't affect the actual data
             * content.
             */
            set_flush_tier_delay(&td->extra_rnd);
        }

        if (!g.opts.running)
            goto done;

        /* Verify the checkpoint we just wrote. */
        /* FIXME-WT-15357 Disagg: Implement checkpoint cursors */
        if (!g.opts.disagg_storage) {
            if ((ret = verify_consistency(session, WT_TS_NONE, true)) != 0)
                return (log_print_err("verify_consistency (checkpoint)", ret, 1));
        }

        /* Verify the content of the database at the verify timestamp. */
        if (g.use_timestamps && (ret = verify_consistency(session, verify_ts, false)) != 0)
            return (log_print_err("verify_consistency (timestamps)", ret, 1));

        /* Advance the oldest timestamp to the most recently set stable timestamp. */
        if (g.use_timestamps && g.ts_oldest != 0) {
            testutil_snprintf(
              timestamp_buf, sizeof(timestamp_buf), "oldest_timestamp=%" PRIx64, g.ts_oldest);
            testutil_check(g.conn->set_timestamp(g.conn, timestamp_buf));
        }

        if (g.sweep_stress)
            /*
             * Random value between 4 and 8 seconds. Use the extra random generator as the tier
             * sleep delay doesn't affect the actual data content.
             */
            delay = __wt_random(&td->extra_rnd) % 5 + 4;
        else
            /* Just find out if we should flush_tier. */
            delay = 0;
        testutil_tiered_sleep(&g.opts, session, delay, &flush_tier);
    }

done:
    /* To be able to replay, print the stable timestamp the test stopped at. */
    if (g.predictable_replay && g.use_timestamps) {
        testutil_check(g.conn->query_timestamp(g.conn, timestamp_buf, "get=stable_timestamp"));
        stable_ts = testutil_timestamp_parse(timestamp_buf);
        printf("Test stopped at a stable timestamp of %" PRIu64 ".\n", stable_ts);
    }
    if ((ret = session->close(session, NULL)) != 0)
        return (log_print_err("session.close", ret, 1));

    return (0);
}

/*
 * do_cursor_next --
 *     Wrapper around cursor->next to handle retry cases.
 */
static int
do_cursor_next(table_type type, WT_CURSOR *cursor)
{
    uint8_t val;
    int ret;

    while ((ret = cursor->next(cursor)) != WT_NOTFOUND) {
        if (ret == 0) {
            /*
             * In FLCS deleted values read back as 0; skip over them. We've arranged to avoid
             * writing out any of our own zero values so this check won't generate false positives.
             */
            if (type == FIX) {
                ret = cursor->get_value(cursor, &val);
                if (ret != 0)
                    return (log_print_err("cursor->get_value", ret, 1));
                if (val == 0)
                    continue;
            }
            break;
        } else if (ret != WT_PREPARE_CONFLICT) {
            (void)log_print_err("cursor->next", ret, 1);
            return (ret);
        }
        __wt_yield();
    }

    return (ret);
}

/*
 * do_cursor_prev --
 *     Wrapper around cursor->prev to handle retry cases.
 */
static int
do_cursor_prev(table_type type, WT_CURSOR *cursor)
{
    uint8_t val;
    int ret;

    while ((ret = cursor->prev(cursor)) != WT_NOTFOUND) {
        if (ret == 0) {
            /*
             * In FLCS deleted values read back as 0; skip over them. We've arranged to avoid
             * writing out any of our own zero values so this check won't generate false positives.
             */
            if (type == FIX) {
                ret = cursor->get_value(cursor, &val);
                if (ret != 0)
                    return (log_print_err("cursor->get_value", ret, 1));
                if (val == 0)
                    continue;
            }
            break;
        } else if (ret != WT_PREPARE_CONFLICT) {
            (void)log_print_err("cursor->next", ret, 1);
            return (ret);
        }
        __wt_yield();
    }

    return (ret);
}

/*
 * verify_consistency --
 *     Open a cursor on each table at the last checkpoint and walk through the tables in parallel.
 *     The key/values should match across all tables.
 */
int
verify_consistency(WT_SESSION *session, wt_timestamp_t verify_ts, bool use_checkpoint)
{
    WT_CURSOR **cursors;
    uint64_t key_count;
    int i, reference_table, ret, t_ret;
    char cfg_buf[128], ckpt_buf[128], next_uri[128];
    const char *ckpt;

    ret = t_ret = 0;
    key_count = 0;
    cursors = calloc((size_t)g.ntables, sizeof(*cursors));
    if (cursors == NULL)
        return (log_print_err("verify_consistency", ENOMEM, 1));

    if (use_checkpoint) {
        testutil_snprintf(ckpt_buf, sizeof(ckpt_buf), "checkpoint=%s", g.checkpoint_name);
        ckpt = ckpt_buf;
    } else {
        ckpt = NULL;
        if (verify_ts != WT_TS_NONE)
            testutil_snprintf(cfg_buf, sizeof(cfg_buf),
              "isolation=snapshot,read_timestamp=%" PRIx64 ",roundup_timestamps=read", verify_ts);
        else
            testutil_snprintf(cfg_buf, sizeof(cfg_buf), "isolation=snapshot");
        testutil_check(session->begin_transaction(session, cfg_buf));
    }

    for (i = 0; i < g.ntables; i++) {
        testutil_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", i);
        if ((ret = session->open_cursor(session, next_uri, NULL, ckpt, &cursors[i])) != 0) {
            (void)log_print_err("verify_consistency:session.open_cursor", ret, 1);
            goto err;
        }
    }

    /* Pick a reference table: the first table that's not FLCS, if possible; else 0. */
    reference_table = 0;
    for (i = 0; i < g.ntables; i++)
        if (g.cookies[i].type != FIX) {
            reference_table = i;
            break;
        }

    while (ret == 0) {
        /* Advance the reference table's cursor. */
        ret = do_cursor_next(g.cookies[reference_table].type, cursors[reference_table]);
        if (ret != 0 && ret != WT_NOTFOUND)
            goto err;

        if (ret == 0)
            ++key_count;

        /*
         * Check to see that all remaining cursors have the same key/value pair.
         */
        for (i = 0; i < g.ntables; i++) {
            if (i == reference_table)
                continue;

            t_ret = do_cursor_next(g.cookies[i].type, cursors[i]);
            if (t_ret != 0 && t_ret != WT_NOTFOUND) {
                ret = t_ret;
                goto err;
            }

            if (ret == WT_NOTFOUND && t_ret == WT_NOTFOUND)
                continue;
            else if (ret == WT_NOTFOUND || t_ret == WT_NOTFOUND) {
                (void)log_print_err(
                  "verify_consistency tables with different amount of data", EFAULT, 1);
                goto err;
            }

            if ((ret = compare_cursors(cursors[reference_table], g.cookies[reference_table].type,
                   cursors[i], g.cookies[i].type)) != 0) {
                (void)diagnose_key_error(cursors[reference_table], g.cookies[reference_table].type,
                  reference_table, cursors[i], g.cookies[i].type, i);
                (void)log_print_err("verify_consistency - mismatching data", EFAULT, 1);
                goto err;
            }
        }
    }
    printf("Finished verifying%s with %d tables and %" PRIu64 " keys at timestamp %" PRIu64 "\n",
      use_checkpoint ? " a checkpoint" : "", g.ntables, key_count, verify_ts);
    fflush(stdout);

err:
    for (i = 0; i < g.ntables; i++) {
        if (cursors[i] != NULL && (ret = cursors[i]->close(cursors[i])) != 0)
            (void)log_print_err("verify_consistency:cursor close", ret, 1);
    }
    if (!use_checkpoint)
        testutil_check(session->commit_transaction(session, NULL));
    free(cursors);
    return (ret);
}

/*
 * compare_cursors --
 *     Compare the key/value pairs from two cursors, which might be different table types.
 */
static int
compare_cursors(WT_CURSOR *cursor1, table_type type1, WT_CURSOR *cursor2, table_type type2)
{
    uint64_t key1, key2;
    uint8_t fixval1, fixval2;
    int ret;
    char fixbuf1[4], fixbuf2[4], *strval1, *strval2;

    ret = 0;

    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        return (log_print_err("Error getting keys", EINVAL, 1));

    /*
     * Get the values. For all table types set both the string value (so we can print) and the FLCS
     * value.
     */

    if (type1 == FIX) {
        if (cursor1->get_value(cursor1, &fixval1) != 0)
            goto valuefail;
        testutil_snprintf(fixbuf1, sizeof(fixbuf1), "%" PRIu8, fixval1);
        strval1 = fixbuf1;
    } else {
        if (cursor1->get_value(cursor1, &strval1) != 0)
            goto valuefail;
        fixval1 = flcs_encode(strval1);
    }

    if (type2 == FIX) {
        if (cursor2->get_value(cursor2, &fixval2) != 0)
            goto valuefail;
        testutil_snprintf(fixbuf2, sizeof(fixbuf2), "%" PRIu8, fixval2);
        strval2 = fixbuf2;
    } else {
        if (cursor2->get_value(cursor2, &strval2) != 0)
            goto valuefail;
        fixval2 = flcs_encode(strval2);
    }

    if (g.logfp != NULL)
        fprintf(g.logfp, "k1: %" PRIu64 " k2: %" PRIu64 " val1: %s val2: %s \n", key1, key2,
          strval1, strval2);

    if (key1 != key2) {
        ret = ERR_KEY_MISMATCH;
        goto mismatch;
    }

    /*
     * The FLCS value encoding loses information, so if an FLCS table tells us FLCS_UNKNOWN we have
     * to treat it as matching any value from another table type.
     */
    if ((type1 == FIX && type2 != FIX && fixval1 == FLCS_UNKNOWN) ||
      (type1 != FIX && type2 == FIX && fixval2 == FLCS_UNKNOWN)) {
        return (0);
    }

    /* If either table is FLCS, compare the 8-bit values; otherwise the strings. */
    if (((type1 == FIX || type2 == FIX) && fixval1 != fixval2) ||
      (type1 != FIX && type2 != FIX &&
        (strlen(strval1) != strlen(strval2) || strcmp(strval1, strval2) != 0))) {
        ret = ERR_DATA_MISMATCH;
        goto mismatch;
    }

    return (0);

mismatch:
    printf("Key/value mismatch: %" PRIu64 "/%s (%" PRIu8 ") from a %s table is not %" PRIu64
           "/%s (%" PRIu8 ") from a %s table\n",
      key1, strval1, fixval1, type_to_string(type1), key2, strval2, fixval2, type_to_string(type2));
    fflush(stdout);

    return (ret);

valuefail:
    return (log_print_err("Error getting values", EINVAL, 1));
}

/*
 * diagnose_key_error --
 *     Dig a bit deeper on failure. Continue after some failures here to extract as much information
 *     as we can.
 */
static int
diagnose_key_error(WT_CURSOR *cursor1, table_type type1, int index1, WT_CURSOR *cursor2,
  table_type type2, int index2)
{
    WT_CURSOR *c;
    WT_SESSION *session;
    uint64_t key1, key1_orig, key2, key2_orig;
    int ret;
    char ckpt[128], next_uri[128];

    /* Hack to avoid passing session as parameter. */
    session = cursor1->session;
    key1_orig = key2_orig = 0;

    /* FIXME-WT-15357: Checkpoint cursors are not compatible with disagg for now. */
    if (!g.opts.disagg_storage)
        testutil_snprintf(ckpt, sizeof(ckpt), "checkpoint=%s", g.checkpoint_name);

    /* Save the failed keys. */
    if (cursor1->get_key(cursor1, &key1_orig) != 0 || cursor2->get_key(cursor2, &key2_orig) != 0) {
        (void)log_print_err("Error retrieving key.", EINVAL, 0);
        goto live_check;
    }

    if (key1_orig == key2_orig)
        goto live_check;

    /*
     * Note: for now the code below hasn't been adapted for FLCS (where it would need to skip zero
     * values when searching forward and backward) because that's a fairly large nuisance and it's
     * not, at least for the moment, all that helpful. FUTURE.
     */

    /* See if previous values are still valid. */
    if (do_cursor_prev(type1, cursor1) != 0 || do_cursor_prev(type2, cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 != key2)
        (void)log_print_err("Now previous keys don't match", EINVAL, 0);

    if (do_cursor_next(type1, cursor1) != 0 || do_cursor_next(type2, cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 == key2)
        (void)log_print_err("After prev/next keys match", EINVAL, 0);

    if (do_cursor_next(type1, cursor1) != 0 || do_cursor_next(type2, cursor2) != 0)
        return (1);
    if (cursor1->get_key(cursor1, &key1) != 0 || cursor2->get_key(cursor2, &key2) != 0)
        (void)log_print_err("Error decoding key", EINVAL, 1);
    else if (key1 == key2)
        (void)log_print_err("After prev/next/next keys match", EINVAL, 0);

    /*
     * Now try opening new cursors on the checkpoints and see if we get the same missing key via
     * searching.
     */
    testutil_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index1);
    if (session->open_cursor(session, next_uri, NULL, ckpt, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 1st key", ret, 0);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    testutil_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index2);
    if (session->open_cursor(session, next_uri, NULL, ckpt, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 1st key", ret, 0);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

live_check:
    /*
     * Now try opening cursors on the live checkpoint to see if we get the same missing key via
     * searching.
     */
    testutil_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index1);
    if (session->open_cursor(session, next_uri, NULL, NULL, &c) != 0)
        return (1);
    c->set_key(c, key1_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("1st cursor didn't find 1st key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    testutil_snprintf(next_uri, sizeof(next_uri), "table:__wt%04d", index2);
    if (session->open_cursor(session, next_uri, NULL, NULL, &c) != 0)
        return (1);
    c->set_key(c, key2_orig);
    if ((ret = c->search(c)) != 0)
        (void)log_print_err("2nd cursor didn't find 2nd key", ret, 0);
    if (c->close(c) != 0)
        return (1);

    return (0);
}
