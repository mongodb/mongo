/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The reader stage: the single consumer of the node's event source - the self-pipe when this phase
 * generates, a live peer's otherwise. Queues events for the workers, runs the step-down work at the
 * marker, and ends the phase on the hand-over.
 */

#include "schema_disagg_abort.h"

/*
 * workers_timestamps_assert --
 *     Assert that no worker completed an event above the given timestamp.
 */
static void
workers_timestamps_assert(WORKLOAD_STATE *state, uint64_t timestamp)
{
    for (uint32_t t = 0; t < state->nth_workers; t++) {
        const uint64_t completed = __wt_atomic_load_uint64(&state->workers[t].completed_ts);
        testutil_assertfmt(completed <= timestamp,
          "step-down: worker %" PRIu32 " completed %" PRIu64 " above the marker's %" PRIu64, t,
          completed, timestamp);
    }
}

/*
 * reader_step_down --
 *     The step-down work once the timestamp is set.
 */
static void
reader_step_down(WORKLOAD_STATE *state, uint64_t ts)
{
    WT_CONNECTION *conn = state->conn;

    /* Signal the timestamp and checkpoint threads to pause. */
    __wt_atomic_store_uint64(&state->stepdown_ts, ts);
    while (__wt_atomic_load_bool(&state->ts_busy))
        __wt_sleep(0, WT_THOUSAND);

    set_stepdown_ts(conn, ts);
    set_frontier(conn, ts);

    WT_SESSION *session;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->checkpoint(session, "use_timestamp=true"));
    testutil_check(session->close(session, NULL));

    /* Keep the step-down checkpoint's LSN; the next leader should adopt it. */
    WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS ckpt_args = {0};
    testutil_assert(ckpt_latest(state, &ckpt_args));
    free(ckpt_args.checkpoint_metadata.mem);

    __wt_atomic_store_uint64(&state->stepdown_ckpt_lsn, ckpt_args.checkpoint_lsn);
    println("Node %" PRIu32 ": step-down checkpoint at %" PRIu64 " (lsn %" PRIu64 ")",
      state->cfg->node_id, ts, ckpt_args.checkpoint_lsn);
}

/*
 * thread_reader_run --
 *     Drain the node's event source: the self-pipe when this phase generates, the peer's pipe
 *     otherwise. Schema and data events are queued for the worker threads; the hand-over event ends
 *     the phase. Pipe EOF can only happen on a peer-fed pipe: it marks the peer dead and turns this
 *     node into a lone follower.
 */
WT_THREAD_RET
thread_reader_run(void *arg)
{
    WORKLOAD_STATE *state = arg;
    TEST_CONFIG *cfg = state->cfg;
    const int src_fd = state->generates ? cfg->self_pipe_read_fd : cfg->pipe_read_fd;

    SCHEMA_EVENT ev;
    bool running = true;
    while (running && workload_active(state, STAGE_READER)) {
        if (!pipe_wait_readable(src_fd))
            continue;
        if (!pipe_event_read(src_fd, &ev)) {
            /* EOF: the peer died. Keep the role; the node continues as a lone follower. */
            testutil_assert(!state->leads); /* The self-pipe's writer lives in this process. */
            cfg->peer_alive = false;
            println("Node %" PRIu32 ": peer died; continuing as a lone follower", cfg->node_id);
            running = false;
            continue;
        }

        switch (ev.type) {
        case EVENT_CREATE:
        case EVENT_DROP:
        case EVENT_INSERT:
        case EVENT_PUBLISH_CREATE:
        case EVENT_PUBLISH_DROP:
            evq_enqueue(state, &ev);
            break;
        case EVENT_STEPDOWN:
            testutil_assert(state->leads && state->generates);
            /*
             * Drain the workers before stepping-down. Write operations cannot straddle the
             * step-down timestamp.
             */
            evq_drain_barrier(state);
            workers_timestamps_assert(state, ev.event_ts);
            reader_step_down(state, ev.event_ts);
            break;
        case EVENT_SWITCH:
            /* The final event of the term's stream: this node must step up. */
            evq_drain_barrier(state);
            /*
             * Relay-integrity check: the drained counter must equal the sender's final counter.
             * Every counter value the term allocated rides an event that precedes the switch in the
             * stream, so after the drain nothing may be missing.
             */
            testutil_assertfmt(__wt_atomic_load_uint64(&state->current_ts) == ev.event_ts,
              "hand-over: drained counter %" PRIu64 " != sender's final counter %" PRIu64,
              __wt_atomic_load_uint64(&state->current_ts), ev.event_ts);
            __wt_atomic_store_bool(&state->handover_received, true);
            running = false;
            break;
        case EVENT_NONE:
            /* Never emitted; a zeroed event means the framing lost its way. */
            testutil_die(
              EINVAL, "Node %" PRIu32 ": empty event read from the source pipe", cfg->node_id);
        }
    }

    return (WT_THREAD_RET_VALUE);
}
