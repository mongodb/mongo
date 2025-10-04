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

#define MAX_MODIFY_ENTRIES 5

static char modify_repl[256];
static int real_worker(THREAD_DATA *);
static WT_THREAD_RET worker(void *);

/*
 * create_table --
 *     Create a WiredTiger table of the configured type for this cookie.
 */
static int
create_table(WT_SESSION *session, COOKIE *cookie)
{
    int ret;
    char config[256];
    const char *kf, *vf;

    kf = cookie->type == COL || cookie->type == FIX ? "r" : "q";
    vf = cookie->type == FIX ? "8t" : "S";

    /*
     * If we're using timestamps, turn off logging for the table.
     */
    if (g.use_timestamps) {
        testutil_snprintf(config, sizeof(config),
          "key_format=%s,value_format=%s,allocation_size=512,"
          "leaf_page_max=1KB,internal_page_max=1KB,"
          "memory_page_max=64KB,log=(enabled=false)",
          kf, vf);
        if (g.opts.disagg_storage)
            testutil_strcat(config, sizeof(config), ",type=layered,block_manager=disagg");
    } else
        testutil_snprintf(config, sizeof(config), "key_format=%s,value_format=%s", kf, vf);

    if ((ret = session->create(session, cookie->uri, config)) != 0)
        if (ret != EEXIST)
            return (log_print_err("session.create", ret, 1));
    ++g.ntables_created;
    return (0);
}

/*
 * modify_repl_init --
 *     Initialize the replacement information.
 */
static void
modify_repl_init(void)
{
    size_t i;

    for (i = 0; i < sizeof(modify_repl); ++i)
        modify_repl[i] = "0123456789"[i % 10];
}

/*
 * start_workers --
 *     Setup the configuration for the tables being populated, then start the worker thread(s) and
 *     wait for them to finish.
 */
int
start_workers(void)
{
    struct timeval start, stop;
    WT_SESSION *session;
    wt_thread_t *tids;
    double seconds;
    int i, ret;

    ret = 0;

    modify_repl_init();

    /* Create statistics and thread structures. */
    if ((tids = calloc((size_t)(g.nworkers), sizeof(*tids))) == NULL)
        return (log_print_err("calloc", errno, 1));

    if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0) {
        (void)log_print_err("conn.open_session", ret, 1);
        goto err;
    }

    /* Create tables */
    for (i = 0; i < g.ntables; ++i) {
        /* Should probably be atomic to avoid races. */
        if ((ret = create_table(session, &g.cookies[i])) != 0)
            goto err;
    }

    testutil_check(session->close(session, NULL));

    (void)gettimeofday(&start, NULL);

    /* Create threads. The N workers have ID 0 to N - 1. */
    for (i = 0; i < g.nworkers; ++i)
        testutil_check(__wt_thread_create(NULL, &tids[i], worker, &g.td[i]));

    /* Wait for the threads. */
    for (i = 0; i < g.nworkers; ++i)
        testutil_check(__wt_thread_join(NULL, &tids[i]));

    (void)gettimeofday(&stop, NULL);
    seconds = (stop.tv_sec - start.tv_sec) + (stop.tv_usec - start.tv_usec) * 1e-6;
    printf("Ran workers for: %f seconds\n", seconds);
    fflush(stdout);

err:
    free(tids);

    return (ret);
}

/*
 * modify_build --
 *     Generate a set of modify vectors.
 */
static void
modify_build(WT_MODIFY *entries, int *nentriesp, u_int seed)
{
    int i, nentries;

    /* Deterministically generate modifies based on the seed. */
    nentries = (int)seed % MAX_MODIFY_ENTRIES + 1;
    for (i = 0; i < nentries; ++i) {
        entries[i].data.data = modify_repl + seed % 10;
        entries[i].data.size = seed % 8 + 1;
        entries[i].offset = seed % 40;
        entries[i].size = seed % 10 + 1;
    }

    *nentriesp = (int)nentries;
}

/*
 * worker_no_ts_delete --
 *     Delete a key without setting a timestamp.
 */
static inline int
worker_no_ts_delete(WT_CURSOR *cursor, uint64_t keyno)
{
    int ret;

    cursor->set_key(cursor, keyno);
    ret = cursor->search(cursor);
    if (ret == 0)
        ret = cursor->remove(cursor);
    if (ret == WT_NOTFOUND)
        ret = 0;

    return (ret);
}

/*
 * cursor_fix_at_zero --
 *     Check if we're on a zero (deleted) value. FLCS only.
 */
static bool
cursor_fix_at_zero(WT_CURSOR *cursor)
{
    uint8_t val;

    testutil_check(cursor->get_value(cursor, &val));
    return (val == 0);
}

/*
 * worker_op --
 *     Write operation.
 */
static inline int
worker_op(WT_CURSOR *cursor, table_type type, uint64_t keyno, u_int new_val)
{
    WT_MODIFY entries[MAX_MODIFY_ENTRIES];
    uint8_t val8;
    int cmp, ret;
    int nentries;
    char valuebuf[64];

    cursor->set_key(cursor, keyno);
    /* Roughly half inserts, then balanced inserts / range removes. */
    if (new_val > g.nops / 2 && new_val % 39 == 0) {
        if ((ret = cursor->search_near(cursor, &cmp)) != 0) {
            if (ret == WT_NOTFOUND)
                return (0);
            if (ret == WT_ROLLBACK || ret == WT_PREPARE_CONFLICT)
                return (WT_ROLLBACK);
            return (log_print_err("cursor.search_near", ret, 1));
        }

        /* Retry the result of search_near again to confirm the result. */
        if (new_val % 2 == 0) {
            if ((ret = cursor->search(cursor)) != 0) {
                if (ret == WT_ROLLBACK)
                    return (WT_ROLLBACK);
                return (log_print_err("cursor.search", ret, 1));
            }
        }

        if (cmp < 0) {
            /* Advance to the next key that exists. */
            if ((ret = cursor->next(cursor)) != 0) {
                if (ret == WT_NOTFOUND)
                    return (0);
                if (ret == WT_ROLLBACK)
                    return (WT_ROLLBACK);
                return (log_print_err("cursor.next", ret, 1));
            }
        } else if (type == FIX) {
            /* To match what happens in VAR and ROW, advance to the next nonzero key. */
            while (cursor_fix_at_zero(cursor))
                if ((ret = cursor->next(cursor)) != 0) {
                    if (ret == WT_NOTFOUND)
                        return (0);
                    if (ret == WT_ROLLBACK)
                        return (WT_ROLLBACK);
                    return (log_print_err("cursor.next", ret, 1));
                }
        }

        for (int i = 10; i > 0; i--) {
            if ((ret = cursor->remove(cursor)) != 0) {
                if (ret == WT_NOTFOUND)
                    return (0);
                if (ret == WT_ROLLBACK)
                    return (WT_ROLLBACK);
                return (log_print_err("cursor.remove", ret, 1));
            }
            if ((ret = cursor->next(cursor)) != 0) {
                if (ret == WT_NOTFOUND)
                    return (0);
                if (ret == WT_ROLLBACK)
                    return (WT_ROLLBACK);
                return (log_print_err("cursor.next", ret, 1));
            }
            if (type == FIX) {
                /* To match what happens in VAR and ROW, advance to the next nonzero key. */
                while (cursor_fix_at_zero(cursor))
                    if ((ret = cursor->next(cursor)) != 0) {
                        if (ret == WT_NOTFOUND)
                            return (0);
                        if (ret == WT_ROLLBACK)
                            return (WT_ROLLBACK);
                        return (log_print_err("cursor.next", ret, 1));
                    }
            }
        }
        if (g.sweep_stress)
            testutil_check(cursor->reset(cursor));
    } else if (new_val % 39 < 10) {
        if ((ret = cursor->search(cursor)) != 0 && ret != WT_NOTFOUND) {
            if (ret == WT_ROLLBACK || ret == WT_PREPARE_CONFLICT)
                return (WT_ROLLBACK);
            return (log_print_err("cursor.search", ret, 1));
        }
        if (g.sweep_stress)
            testutil_check(cursor->reset(cursor));
    } else {
        /* FIXME-WT-14467 should fix cursor->modify for layered tables. */
        if (new_val % 39 < 30 && !g.opts.disagg_storage) {
            /* Do modify. */
            ret = cursor->search(cursor);
            if (ret == 0 && (type != FIX || !cursor_fix_at_zero(cursor))) {
                modify_build(entries, &nentries, new_val);
                if (type == FIX) {
                    /* Deleted (including not-yet-written) values read back as 0; accommodate. */
                    ret = cursor->get_value(cursor, &val8);
                    if (ret != 0)
                        return (log_print_err("cursor.get_value", ret, 1));
                    cursor->set_value(cursor, flcs_modify(entries, nentries, val8));
                    ret = cursor->update(cursor);
                } else
                    ret = cursor->modify(cursor, entries, nentries);
                if (ret != 0) {
                    if (ret == WT_ROLLBACK)
                        return (WT_ROLLBACK);
                    return (log_print_err("cursor.modify", ret, 1));
                }
                return (0);
            } else if (ret != 0 && ret != WT_NOTFOUND) {
                if (ret == WT_ROLLBACK || ret == WT_PREPARE_CONFLICT)
                    return (WT_ROLLBACK);
                return (log_print_err("cursor.search", ret, 1));
            }
        }

        /* If key doesn't exist, turn modify into an insert. */
        testutil_snprintf(valuebuf, sizeof(valuebuf), "%052u", new_val);
        if (type == FIX)
            cursor->set_value(cursor, flcs_encode(valuebuf));
        else
            cursor->set_value(cursor, valuebuf);
        if ((ret = cursor->insert(cursor)) != 0) {
            if (ret == WT_ROLLBACK)
                return (WT_ROLLBACK);
            return (log_print_err("cursor.insert", ret, 1));
        }
    }

    return (0);
}

/*
 * worker --
 *     Worker thread start function.
 */
static WT_THREAD_RET
worker(void *arg)
{
    THREAD_DATA *td;
    char tid[128];

    td = (THREAD_DATA *)arg;

    testutil_check(__wt_thread_str(tid, sizeof(tid)));
    printf("worker thread starting: tid: %s key-range: %" PRIu32 " - %" PRIu32 "\n", tid,
      td->start_key, td->start_key + td->key_range);
    fflush(stdout);

    (void)real_worker(td);
    return (WT_THREAD_RET_VALUE);
}

/*
 * real_worker --
 *     A single worker thread that transactionally updates all tables with consistent values.
 */
static int
real_worker(THREAD_DATA *td)
{
    WT_CURSOR **cursors;
    WT_SESSION *session;
    uint64_t base_ts, prepared_id;
    u_int i, keyno, next_rnd;
    int j, ret, t_ret;
    char buf[128];
    const char *begin_cfg, *rollback_cfg;
    bool prepared, reopen_cursors, new_txn, start_txn;

    ret = t_ret = 0;
    reopen_cursors = false;
    start_txn = true;
    new_txn = false;
    prepared = false;

    if ((cursors = calloc((size_t)(g.ntables), sizeof(WT_CURSOR *))) == NULL)
        return (log_print_err("malloc", ENOMEM, 1));

    if ((ret = g.conn->open_session(g.conn, NULL, "isolation=snapshot", &session)) != 0) {
        (void)log_print_err("conn.open_session", ret, 1);
        goto err;
    }

    begin_cfg = NULL;
    if (g.use_timestamps) {
        if (g.no_ts_deletes)
            begin_cfg = "no_timestamp=true,read_timestamp=1,roundup_timestamps=(read=true)";
        else if (!g.predictable_replay)
            begin_cfg = "read_timestamp=1,roundup_timestamps=(read=true)";
        /*
         * Note: For predictable replays we do not specify a read timestamp, hence reading the
         * latest committed values. This is important for a predictable outcome as reading at the
         * oldest timestamp depends on where the clock and the checkpoint threads have placed the
         * oldest at this moment.
         */
    }

    for (j = 0; j < g.ntables; j++)
        if ((ret = session->open_cursor(session, g.cookies[j].uri, NULL, NULL, &cursors[j])) != 0) {
            (void)log_print_err("session.open_cursor", ret, 1);
            goto err;
        }

    for (i = 0; g.opts.running; ++i, __wt_yield()) {
        /*
         * If a stop timestamp has been provided, the workers will continue to run until the clock
         * thread reaches a stable equal to the stop timestamp. Ignore the provided operation count
         * in such a case.
         */
        if (g.stop_ts == 0 && i >= g.nops)
            break;

        if (i > 0 && i % (5 * WT_THOUSAND) == 0)
            printf("Worker %u of %u ops\n", i, g.nops);
        if (start_txn) {
            if ((ret = session->begin_transaction(session, begin_cfg)) != 0) {
                (void)log_print_err("real_worker:begin_transaction", ret, 1);
                goto err;
            }
            new_txn = true;
            start_txn = false;
            prepared = false;
        }
        keyno = __wt_random(&td->data_rnd) % td->key_range + td->start_key;
        /* If we have specified to run with mix mode deletes we need to do it in it's own txn. */
        if (g.use_timestamps && g.no_ts_deletes && new_txn &&
          __wt_random(&td->data_rnd) % 72 == 0) {
            new_txn = false;
            for (j = 0; j < g.ntables; j++) {
                ret = worker_no_ts_delete(cursors[j], keyno);
                if (ret == WT_ROLLBACK || ret == WT_PREPARE_CONFLICT)
                    break;
                else if (ret != 0)
                    goto err;
            }

            if (ret == 0) {
                if ((ret = session->commit_transaction(session, NULL)) != 0) {
                    (void)log_print_err("real_worker:commit_no_ts_transaction", ret, 1);
                    goto err;
                }
            } else {
                if ((ret = session->rollback_transaction(session, NULL)) != 0) {
                    (void)log_print_err("real_worker:rollback_no_ts_transaction", ret, 1);
                    goto err;
                }
            }
            start_txn = true;
            continue;
        } else
            new_txn = false;

        for (j = 0; ret == 0 && j < g.ntables; j++)
            ret = worker_op(cursors[j], g.cookies[j].type, keyno, i);
        if (ret != 0 && ret != WT_ROLLBACK) {
            (void)log_print_err("worker op failed", ret, 1);
            goto err;
        } else if (ret == 0) {
            next_rnd = __wt_random(&td->data_rnd);
            if (next_rnd % 7 == 0) {
                if (g.use_timestamps) {
                    /*
                     * For a predictable run, the timestamps for worker's operations are managed by
                     * reserving them across the threads and the iterations, such that they don't
                     * overlap. For a regular run, the timestamp thread manages the advance of the
                     * global clock. The workers synchronize with the clock using a reader - writer
                     * lock, and decide the operation timestamp based on the global clock.
                     */
                    if (g.predictable_replay ||
                      (__wt_try_readlock((WT_SESSION_IMPL *)session, &g.clock_lock) == 0)) {
                        if (g.predictable_replay)
                            /* i + 1 because we don't want a thread to start with commit-ts of 1 */
                            base_ts = RESERVED_TIMESTAMPS_FOR_ITERATION(g.nworkers, td, i + 1);
                        else
                            base_ts = g.ts_stable + 1;
                        next_rnd = __wt_random(&td->data_rnd);
                        if (g.prepare && next_rnd % 2 == 0) {
                            prepared_id = __wt_atomic_addv64(&g.prepared_id, 1);
                            testutil_snprintf(buf, sizeof(buf),
                              "prepare_timestamp=%" PRIx64 ",prepared_id=%" PRIx64, base_ts,
                              prepared_id);
                            if ((ret = session->prepare_transaction(session, buf)) != 0) {
                                if (!g.predictable_replay)
                                    __wt_readunlock((WT_SESSION_IMPL *)session, &g.clock_lock);
                                (void)log_print_err("real_worker:prepare_transaction", ret, 1);
                                goto err;
                            }
                            testutil_snprintf(buf, sizeof(buf),
                              "durable_timestamp=%" PRIx64 ",commit_timestamp=%" PRIx64,
                              base_ts + 2, base_ts);
                            prepared = true;
                        } else
                            testutil_snprintf(
                              buf, sizeof(buf), "commit_timestamp=%" PRIx64, base_ts);

                        /* Commit majority of times. */
                        if (next_rnd % 49 != 0) {
                            if ((ret = session->commit_transaction(session, buf)) != 0) {
                                if (!g.predictable_replay)
                                    __wt_readunlock((WT_SESSION_IMPL *)session, &g.clock_lock);
                                (void)log_print_err("real_worker:commit_transaction", ret, 1);
                                goto err;
                            }
                            if (g.predictable_replay)
                                WT_RELEASE_WRITE_WITH_BARRIER(td->ts, base_ts);
                        } else {
                            if (prepared) {
                                testutil_snprintf(
                                  buf, sizeof(buf), "rollback_timestamp=%" PRIx64, base_ts + 2);
                                rollback_cfg = buf;
                            } else
                                rollback_cfg = NULL;
                            if ((ret = session->rollback_transaction(session, rollback_cfg)) != 0) {
                                if (!g.predictable_replay)
                                    __wt_readunlock((WT_SESSION_IMPL *)session, &g.clock_lock);
                                (void)log_print_err("real_worker:rollback_transaction", ret, 1);
                                goto err;
                            }
                        }
                        if (!g.predictable_replay)
                            __wt_readunlock((WT_SESSION_IMPL *)session, &g.clock_lock);
                        start_txn = true;
                        /* Occasionally reopen cursors after transaction finish. */
                        if (next_rnd % 13 == 0)
                            reopen_cursors = true;
                    }
                } else {
                    /* Commit majority of times. */
                    if (next_rnd % 49 != 0) {
                        if ((ret = session->commit_transaction(session, NULL)) != 0) {
                            (void)log_print_err("real_worker:commit_transaction", ret, 1);
                            goto err;
                        }
                    } else {
                        if ((ret = session->rollback_transaction(session, NULL)) != 0) {
                            (void)log_print_err("real_worker:rollback_transaction", ret, 1);
                            goto err;
                        }
                    }
                    start_txn = true;
                }
            } else if (next_rnd % 15 == 0)
                /* Occasionally reopen cursors during a running transaction. */
                reopen_cursors = true;
        } else {
            if ((ret = session->rollback_transaction(session, NULL)) != 0) {
                (void)log_print_err("real_worker:rollback_transaction", ret, 1);
                goto err;
            }
            start_txn = true;
        }
        if (reopen_cursors) {
            for (j = 0; j < g.ntables; j++) {
                testutil_check(cursors[j]->close(cursors[j]));
                if ((ret = session->open_cursor(
                       session, g.cookies[j].uri, NULL, NULL, &cursors[j])) != 0) {
                    (void)log_print_err("session.open_cursor", ret, 1);
                    goto err;
                }
            }
            reopen_cursors = false;
        }
    }

err:
    if (session != NULL && (t_ret = session->close(session, NULL)) != 0 && ret == 0) {
        ret = t_ret;
        (void)log_print_err("session.close", ret, 1);
    }
    free(cursors);

    return (ret);
}
