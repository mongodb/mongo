/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The checkpoint stage: the node's checkpoint duty, paced independently of the workload - produced
 * while leading, adopted from the page log while following. Off the event stream so a checkpoint
 * can land between a schema operation and its publish, and can still be taken while a worker is
 * blocked waiting for one; on its own thread so a slow checkpoint cannot freeze the frontier.
 */

#include "schema_disagg_abort.h"

/*
 * ckpt_pick_up --
 *     Pick up the latest complete checkpoint onto this connection. Returns true if a checkpoint was
 *     adopted, false if the page log has no new checkpoint.
 */
static bool
ckpt_pick_up(WORKLOAD_STATE *state, WT_SESSION *session, WT_PAGE_LOG *page_log)
{
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};

    const int ret = page_log->pl_get_complete_checkpoint(page_log, session, &ckpt_args);
    if (ret == WT_NOTFOUND)
        return (false);
    testutil_check(ret);

    if (ckpt_args.checkpoint_lsn == state->adopted_ckpt_lsn) {
        free(ckpt_args.checkpoint_metadata.mem);
        return (false);
    }

    WT_CONNECTION *conn = session->connection;
    char meta_config[4096];
    testutil_snprintf(meta_config, sizeof(meta_config), "disaggregated=(checkpoint_meta=\"%.*s\")",
      (int)ckpt_args.checkpoint_metadata.size, (const char *)ckpt_args.checkpoint_metadata.data);
    testutil_check(conn->reconfigure(conn, meta_config));
    free(ckpt_args.checkpoint_metadata.mem);
    state->adopted_ckpt_lsn = ckpt_args.checkpoint_lsn;
    return (true);
}

/*
 * ckpt_adopt_latest --
 *     Adopt the latest complete checkpoint.
 */
void
ckpt_adopt_latest(WORKLOAD_STATE *state)
{
    WT_CONNECTION *conn = state->conn;

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    WT_PAGE_LOG *page_log;
    testutil_check(conn->get_page_log(conn, "palite", &page_log));

    (void)ckpt_pick_up(state, session, page_log);

    testutil_check(page_log->terminate(page_log, NULL));
    testutil_check(session->close(session, NULL));

    println("Node %" PRIu32 ": adopted the latest checkpoint before step-up", state->cfg->node_id);
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
    if (!state->leads)
        testutil_check(state->conn->get_page_log(state->conn, "palite", &ckpt.page_log));

    /* The cadence draws from the stream one past the generator's per-worker streams. */
    WT_RAND_STATE *rnd = &state->gen_rnd[state->nth_workers];
    struct timespec last = ckpt.phase_start;
    uint64_t wait = __wt_random(rnd) % MAX_CKPT_INVL;

    while (workload_active(state, STAGE_CKPT)) {
        struct timespec now;
        __wt_epoch(NULL, &now);
        if ((uint64_t)WT_TIMEDIFF_SEC(now, last) < wait) {
            __wt_sleep(0, 100 * WT_THOUSAND);
            continue;
        }

        node_role(state->leads)->checkpoint(state, session, &ckpt);

        __wt_epoch(NULL, &last);
        wait = __wt_random(rnd) % MAX_CKPT_INVL;
    }

    if (ckpt.page_log != NULL)
        testutil_check(ckpt.page_log->terminate(ckpt.page_log, NULL));
    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * workers_min --
 *     Return the minimum completed timestamp across all worker threads: the frontier with no
 *     unfinished publish or commit at or below it. Returns 0 if any worker has not yet completed an
 *     operation this phase.
 */
static uint64_t
workers_min(WORKLOAD_STATE *state)
{
    uint64_t min_val = UINT64_MAX;
    for (uint32_t i = 0; i < state->nth_workers; i++) {
        const uint64_t val = __wt_atomic_load_uint64(&state->workers[i].completed_ts);
        if (val == 0)
            return (0);
        if (val < min_val)
            min_val = val;
    }
    return (min_val);
}

/*
 * thread_ts_run --
 *     Advances the oldest and stable timestamps and the stable schema epoch to the workers'
 *     completed frontier, keeping stable data on published tables only.
 */
WT_THREAD_RET
thread_ts_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    while (workload_active(state, STAGE_TS)) {
        /*
         * The single frontier serves both schema and data operations: everything at or below it is
         * published and committed.
         */
        const uint64_t frontier = workers_min(state);
        if (frontier != 0) {
            const uint64_t cur_stable = query_ts(state->conn, "stable_timestamp");
            if (frontier >= cur_stable)
                set_frontier(state->conn, frontier);
        }

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
    /* The stable value this checkpoint is bound to cover; it can only advance mid-checkpoint. */
    const uint64_t covered = query_ts(state->conn, "stable_timestamp");
    if (covered == 0) {
        struct timespec now;
        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, ckpt->phase_start) > MAX_OP_WAIT)
            testutil_die(ETIMEDOUT, "stable timestamp not set after %d seconds", MAX_OP_WAIT);
        return;
    }

    /* The timestamp thread owns the stable epoch and timestamps; just checkpoint. */
    testutil_check(session->checkpoint(session, "use_timestamp=true"));

    println("Node %" PRIu32 ": checkpoint %d complete", state->cfg->node_id, ++ckpt->produced);

    /* A stable frontier implies every worker published, so this checkpoint has a schema op. */
    if (ckpt->produced == 1)
        testutil_sentinel(NULL, LEADER_READY_FILE);
}

/*
 * follower_checkpoint --
 *     Adopt the latest checkpoint the page log holds. The workers keep running through it. The
 *     first adoption reports follower readiness.
 */
void
follower_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt)
{
    if (ckpt_pick_up(state, session, ckpt->page_log) && !ckpt->picked_up) {
        testutil_sentinel(NULL, FOLLOWER_READY_FILE);
        ckpt->picked_up = true;
    }
}
