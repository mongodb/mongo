/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The checkpoint stage: the node's checkpoint duty, paced independently of the workload - produced
 * while leading, adopted one at a time while following.
 */

#include "schema_disagg_abort.h"

/*
 * ckpt_get --
 *     Read one complete checkpoint: the one at the given LSN or the next one above it, or the
 *     latest when the LSN is zero. False when none found; the caller frees the metadata buffer.
 */
bool
ckpt_get(WORKLOAD_STATE *state, WT_SESSION *session, WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS *args)
{
    WT_PAGE_LOG *page_log = state->page_log;

    const int ret = page_log->pl_get_complete_checkpoint(page_log, session, args);
    testutil_check_error_ok(ret, WT_NOTFOUND);

    return (ret != WT_NOTFOUND);
}
/*
 * ckpt_stat --
 *     Return one of the connection's statistics.
 */
static uint64_t
ckpt_stat(WT_SESSION *session, int stat_key)
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
 *     Pick up one checkpoint's metadata onto this connection and wait for the pick-up to land.
 */
static void
ckpt_pick_up(WORKLOAD_STATE *state, WT_SESSION *session, const char *meta, size_t meta_size)
{
    struct timespec start;
    __wt_epoch(NULL, &start);

    WT_CONNECTION *conn = session->connection;
    char meta_config[4096];
    testutil_snprintf(meta_config, sizeof(meta_config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)meta_size, meta);
    testutil_check(conn->reconfigure(conn, meta_config));

    const uint64_t delivered = ckpt_stat(session, WT_STAT_CONN_DISAGG_CHECKPOINT_DELIVERED_LSN);
    for (;;) {
        const uint64_t adopted = ckpt_stat(session, WT_STAT_CONN_DISAGG_CHECKPOINT_META_LSN);
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

    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};
    if (!ckpt_get(state, session, &ckpt_args)) {
        testutil_check(session->close(session, NULL));
        return; /* no checkpoint to adopt */
    }

    const bool adopt = ckpt_args.checkpoint_lsn != state->adopted_ckpt_lsn &&
      __wt_atomic_load_uint64(&state->frontier_ts) >= ckpt_args.checkpoint_timestamp;
    if (adopt) {
        ckpt_pick_up(state, session, (const char *)ckpt_args.checkpoint_metadata.data,
          ckpt_args.checkpoint_metadata.size);

        state->adopted_ckpt_lsn = ckpt_args.checkpoint_lsn;
        println(
          "Node %" PRIu32 ": adopted the latest checkpoint before step-up", state->cfg->node_id);
    }

    testutil_check(session->close(session, NULL));
    free(ckpt_args.checkpoint_metadata.mem);
}

/*
 * thread_ckpt_run --
 *     Run the role's checkpoint duty for as long as the phase runs.
 */
WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    WT_SESSION *session;
    testutil_check(state->conn->open_session(state->conn, NULL, NULL, &session));

    CKPT_CTX ckpt;
    WT_CLEAR(ckpt);
    __wt_epoch(NULL, &ckpt.phase_start);
    ckpt.last = ckpt.phase_start;
    ckpt.rnd = &state->ext_rnd;
    ckpt.wait = 1 + __wt_random(ckpt.rnd) % MAX_CKPT_INVL;

    while (workload_active(state, STAGE_CKPT)) {
        node_role(state->leads)->checkpoint(state, session, &ckpt);
        __wt_sleep(0, 100 * WT_THOUSAND);
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
                workload_set_frontier(state, frontier_ts);
        }
        __wt_atomic_store_bool(&state->ts_busy, false);

        __wt_sleep(0, 100 * WT_THOUSAND);
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * ckpt_take --
 *     Take one checkpoint at the given stable timestamp and report it, returning its LSN.
 */
static uint64_t
ckpt_take(
  WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt, uint64_t stable_ts, const char *kind)
{
    /* The timestamp thread owns the frontier; just checkpoint. */
    testutil_check(session->checkpoint(session, "use_timestamp=true"));

    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};
    testutil_assert(ckpt_get(state, session, &ckpt_args));
    free(ckpt_args.checkpoint_metadata.mem);
    testutil_assert(ckpt_args.checkpoint_lsn != 0);

    println("Node %" PRIu32 ": %scheckpoint %" PRIu32 " at %" PRIu64 " (lsn %" PRIu64 ")",
      state->cfg->node_id, kind, ++ckpt->produced, stable_ts, ckpt_args.checkpoint_lsn);

    return (ckpt_args.checkpoint_lsn);
}

/*
 * ckpt_take_periodic --
 *     Produce one checkpoint on the phase's own cadence.
 */
static void
ckpt_take_periodic(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    struct timespec now;
    __wt_epoch(NULL, &now);
    if ((uint64_t)WT_TIMEDIFF_SEC(now, ckpt->last) < ckpt->wait)
        return;

    const uint64_t stable_ts = query_ts(state->conn, TS_STABLE);
    if (stable_ts == 0) {
        if (WT_TIMEDIFF_SEC(now, ckpt->phase_start) > MAX_OP_WAIT)
            testutil_die(ETIMEDOUT, "stable timestamp not set after %d seconds", MAX_OP_WAIT);
        return;
    }

    (void)ckpt_take(state, session, ckpt, stable_ts, "");

    /* A stable frontier implies every worker completed an operation by now. */
    if (ckpt->produced == 1u)
        testutil_sentinel(NULL, LEADER_READY_FILE);

    /* The interval runs from the completion, so slow checkpoints do not chain back to back. */
    __wt_epoch(NULL, &ckpt->last);
    ckpt->wait = 1 + __wt_random(ckpt->rnd) % MAX_CKPT_INVL;
}

/*
 * ckpt_take_stepdown --
 *     Produce the single step-down checkpoint. The next leader waits on its LSN.
 */
static void
ckpt_take_stepdown(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    if (!__wt_atomic_load_bool(&state->stepdown_ckpt_due) ||
      __wt_atomic_load_uint64(&state->stepdown_ckpt_lsn) != 0)
        return;

    /* The reader pinned stable at the step-down timestamp before releasing this thread. */
    const uint64_t stepdown_ts = __wt_atomic_load_uint64(&state->stepdown_ts);
    const uint64_t lsn = ckpt_take(state, session, ckpt, stepdown_ts, "step-down ");

    /* Zero would read as "not taken yet" to the generator waiting on it. */
    __wt_atomic_store_uint64(&state->stepdown_ckpt_lsn, lsn);
}

/*
 * leader_checkpoint --
 *     The leader's checkpoint duty: a step-down replaces the cadence with one final checkpoint, and
 *     nothing else is checkpointed until the transition completes.
 */
void
leader_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    if (__wt_atomic_load_uint64(&state->stepdown_ts) != 0)
        ckpt_take_stepdown(state, session, ckpt);
    else
        ckpt_take_periodic(state, session, ckpt);
}

/*
 * follower_checkpoint --
 *     Pick up the next checkpoint once this node has caught up with it; a worker blocked on a drop
 *     is waiting for it.
 */
void
follower_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    WT_UNUSED(ckpt);

    /* The next checkpoint this node has not picked up. */
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};
    ckpt_args.lsn = state->adopted_ckpt_lsn + 1;
    if (!ckpt_get(state, session, &ckpt_args))
        return;

    /* Never pick up a checkpoint the node has not caught up with; leave it for a later tick. */
    const uint64_t frontier_ts = __wt_atomic_load_uint64(&state->frontier_ts);
    if (frontier_ts >= ckpt_args.checkpoint_timestamp) {
        /* Adopted LSN is 0 until a checkpoint is picked up, so read it before the pick-up. */
        const bool first_ckpt = state->adopted_ckpt_lsn == 0;

        ckpt_pick_up(state, session, (const char *)ckpt_args.checkpoint_metadata.data,
          ckpt_args.checkpoint_metadata.size);
        state->adopted_ckpt_lsn = ckpt_args.checkpoint_lsn;

        println("Node %" PRIu32 ": pick-up at %" PRIu64 " (lsn %" PRIu64 "); frontier %" PRIu64,
          state->cfg->node_id, ckpt_args.checkpoint_timestamp, ckpt_args.checkpoint_lsn,
          frontier_ts);

        /* Each pick-up is reported for a stepping-down peer. */
        adopted_lsn_publish(state->cfg->node_id, state->adopted_ckpt_lsn);

        /* The first picked up checkpoint: follower is ready. */
        if (first_ckpt)
            testutil_sentinel(NULL, FOLLOWER_READY_FILE);
    }
    free(ckpt_args.checkpoint_metadata.mem);
}
