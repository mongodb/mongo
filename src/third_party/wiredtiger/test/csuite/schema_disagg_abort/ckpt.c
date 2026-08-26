/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The checkpoint stage: the node's checkpoint duty, paced independently of the workload - produced
 * while leading, adopted from the page log while following.
 */

#include "schema_disagg_abort.h"

/*
 * ckpt_latest --
 *     Read the latest complete checkpoint. False when none found; the caller frees the metadata
 *     buffer.
 */
bool
ckpt_latest(WORKLOAD_STATE *state, WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS *args)
{
    WT_CONNECTION *conn = state->conn;
    WT_PAGE_LOG *page_log = state->page_log;

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    const int ret = page_log->pl_get_complete_checkpoint(page_log, session, args);
    testutil_check_error_ok(ret, WT_NOTFOUND);
    testutil_check(session->close(session, NULL));

    return (ret != WT_NOTFOUND);
}
/*
 * ckpt_lsn_stat --
 *     Return one of the connection's checkpoint metadata LSN statistics.
 */
static uint64_t
ckpt_lsn_stat(WT_SESSION *session, int stat_key)
{
    WT_CURSOR *stat_cursor;
    testutil_check(session->open_cursor(session, "statistics:", NULL, NULL, &stat_cursor));
    stat_cursor->set_key(stat_cursor, stat_key);
    testutil_check(stat_cursor->search(stat_cursor));

    int64_t value;
    const char *desc, *pvalue;
    testutil_check(stat_cursor->get_value(stat_cursor, &desc, &pvalue, &value));
    testutil_check(stat_cursor->close(stat_cursor));

    return ((uint64_t)value);
}

/*
 * ckpt_pick_up --
 *     Pick up and adopt the latest complete checkpoint onto this connection. Returns true once a
 *     new checkpoint is adopted, false if the page log has no new checkpoint.
 */
static bool
ckpt_pick_up(WORKLOAD_STATE *state, WT_SESSION *session)
{
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};

    if (!ckpt_latest(state, &ckpt_args))
        return (false);

    if (ckpt_args.checkpoint_lsn == state->adopted_ckpt_lsn) {
        free(ckpt_args.checkpoint_metadata.mem);
        return (false);
    }

    /* Never adopt a checkpoint the node has not caught up with. */
    if (__wt_atomic_load_uint64(&state->current_ts) < ckpt_args.checkpoint_timestamp) {
        free(ckpt_args.checkpoint_metadata.mem);
        return (false);
    }

    struct timespec start;
    __wt_epoch(NULL, &start);

    WT_CONNECTION *conn = session->connection;
    char meta_config[4096];
    testutil_snprintf(meta_config, sizeof(meta_config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)ckpt_args.checkpoint_metadata.size, (const char *)ckpt_args.checkpoint_metadata.data);
    testutil_check(conn->reconfigure(conn, meta_config));
    free(ckpt_args.checkpoint_metadata.mem);

    const uint64_t delivered = ckpt_lsn_stat(session, WT_STAT_CONN_DISAGG_CHECKPOINT_DELIVERED_LSN);
    for (;;) {
        const uint64_t adopted = ckpt_lsn_stat(session, WT_STAT_CONN_DISAGG_CHECKPOINT_META_LSN);
        if (adopted >= delivered)
            break;

        struct timespec now;
        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, start) > MAX_OP_WAIT)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": checkpoint metadata LSN %" PRIu64
              " not adopted in %d seconds, stalled at %" PRIu64,
              state->cfg->node_id, delivered, MAX_OP_WAIT, adopted);
        __wt_sleep(0, 10 * WT_THOUSAND);
    }

    state->adopted_ckpt_lsn = ckpt_args.checkpoint_lsn;
    return (true);
}

/*
 * ckpt_adopt_latest --
 *     Adopt the latest checkpoint before stepping up.
 */
void
ckpt_adopt_latest(WORKLOAD_STATE *state)
{
    WT_SESSION *session;
    testutil_check(state->conn->open_session(state->conn, NULL, NULL, &session));
    const bool adopted = ckpt_pick_up(state, session);
    testutil_check(session->close(session, NULL));
    if (adopted)
        println(
          "Node %" PRIu32 ": adopted the latest checkpoint before step-up", state->cfg->node_id);
}

/*
 * thread_ckpt_run --
 *     Take one checkpoint per random interval for as long as the phase runs. The interval runs from
 *     the previous checkpoint's completion, so slow checkpoints do not chain back to back.
 */
WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    WT_SESSION *session;
    testutil_check(state->conn->open_session(state->conn, NULL, NULL, &session));

    /* The role's checkpoint bookkeeping for this phase; only a follower adopts checkpoints. */
    CKPT_CTX ckpt = {0};
    __wt_epoch(NULL, &ckpt.phase_start);

    /* The cadence draws from the stream one past the generator's per-worker streams. */
    WT_RAND_STATE *rnd = &state->gen_rnd[state->worker_count];
    struct timespec last = ckpt.phase_start;
    uint64_t wait = 1 + __wt_random(rnd) % MAX_CKPT_INVL;

    while (workload_active(state, STAGE_CKPT)) {
        struct timespec now;
        __wt_epoch(NULL, &now);
        if ((uint64_t)WT_TIMEDIFF_SEC(now, last) < wait) {
            __wt_sleep(0, 100 * WT_THOUSAND);
            continue;
        }

        node_role(state->leads)->checkpoint(state, session, &ckpt);

        __wt_epoch(NULL, &last);
        wait = 1 + __wt_random(rnd) % MAX_CKPT_INVL;
    }

    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * frontier_advance --
 *     Advance the frontier over the timestamps the workers completed: the frontier with no
 *     unfinished schema operation or commit at or below it. Only the timestamp thread may call it.
 */
static uint64_t
frontier_advance(WORKLOAD_STATE *state)
{
    uint64_t frontier_ts = __wt_atomic_load_uint64(&state->frontier_ts);

    while (__wt_atomic_load_uint8(&state->completed_ts[(frontier_ts + 1) % FRONTIER_WINDOW]) != 0) {
        /* Consume the mark for this timestamp. */
        __wt_atomic_store_uint8(&state->completed_ts[(frontier_ts + 1) % FRONTIER_WINDOW], 0);
        ++frontier_ts;
    }
    __wt_atomic_store_uint64(&state->frontier_ts, frontier_ts);

    return (frontier_ts);
}

/*
 * thread_ts_run --
 *     Advance the oldest and stable timestamps, plus the stable schema epoch, when in use.
 */
WT_THREAD_RET
thread_ts_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    while (workload_active(state, STAGE_TS)) {
        /* Frontier of the fully complete operations across all workers. */
        const uint64_t frontier_ts = frontier_advance(state);

        /*
         * Setting timestamps is a critical section: the stable frontier must not advance while a
         * step-down is in progress.
         */
        __wt_atomic_store_bool(&state->ts_busy, true);
        if (__wt_atomic_load_uint64(&state->stepdown_ts) == 0) {
            /*
             * The single frontier serves both schema and data operations: everything at or below it
             * is completed.
             */
            const uint64_t stable_ts = query_ts(state->conn, TS_STABLE);
            if (frontier_ts >= stable_ts)
                set_ts(state->cfg, state->conn, TS_FRONTIER, frontier_ts);
        }
        __wt_atomic_store_bool(&state->ts_busy, false);

        __wt_sleep(0, 100 * WT_THOUSAND);
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * leader_checkpoint --
 *     Produce one checkpoint. The first one reports leader readiness. Nothing is checkpointed while
 *     no stable timestamp exists yet.
 */
void
leader_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    /* Skip the checkpoint if the step-down is in progress. */
    if (__wt_atomic_load_uint64(&state->stepdown_ts) != 0)
        return;

    /* Skip the checkpoint if there is no stable timestamp yet. */
    const uint64_t stable_ts = query_ts(state->conn, TS_STABLE);
    if (stable_ts == 0) {
        struct timespec now;
        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, ckpt->phase_start) > MAX_OP_WAIT)
            testutil_die(ETIMEDOUT, "stable timestamp not set after %d seconds", MAX_OP_WAIT);
        return;
    }

    /* The timestamp thread owns the frontier; just checkpoint. */
    testutil_check(session->checkpoint(session, "use_timestamp=true"));

    println(
      "Node %" PRIu32 ": checkpoint %" PRIu32 " complete", state->cfg->node_id, ++ckpt->produced);

    /* A stable frontier implies every worker completed an operation this phase. */
    if (ckpt->produced == 1u)
        testutil_sentinel(NULL, LEADER_READY_FILE);
}

/*
 * follower_checkpoint --
 *     Adopt the latest checkpoint from the leader. The workers keep running through it.
 */
void
follower_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    WT_UNUSED(ckpt);

    /* Adopted LSN is 0 until a checkpoint is picked up, so read it before the pick-up. */
    const bool first_ckpt = state->adopted_ckpt_lsn == 0;

    if (!ckpt_pick_up(state, session))
        return;

    /* Each adoption is reported for a stepping-down peer. */
    adopted_lsn_publish(state->cfg->node_id, state->adopted_ckpt_lsn);

    /* The first picked up checkpoint: follower is ready. */
    if (first_ckpt)
        testutil_sentinel(NULL, FOLLOWER_READY_FILE);
}
