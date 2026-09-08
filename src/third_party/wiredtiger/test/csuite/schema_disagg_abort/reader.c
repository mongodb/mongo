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
 * frontier_assert --
 *     Assert the frontier has not passed the final counter.
 */
static void
frontier_assert(WORKLOAD_STATE *state, uint64_t timestamp)
{
    const uint64_t frontier_ts = __wt_atomic_load_uint64(&state->frontier_ts);

    testutil_assertfmt(frontier_ts <= timestamp,
      "step-down: the frontier %" PRIu64 " passed the final counter %" PRIu64, frontier_ts,
      timestamp);
}

/*
 * reader_step_down --
 *     The step-down work once the timestamp is set.
 */
static void
reader_step_down(WORKLOAD_STATE *state, uint64_t ts)
{
    testutil_assert(__wt_atomic_load_uint64(&state->stepdown_ts) == 0);
    testutil_assert(__wt_atomic_load_bool(&state->stepdown_ckpt_due) == false);

    /* Signal the timestamp and checkpoint threads to pause. */
    __wt_atomic_store_uint64(&state->stepdown_ts, ts);
    while (__wt_atomic_load_bool(&state->ts_busy))
        __wt_sleep(0, WT_THOUSAND);

    /*
     * FIXME-WT-18314: Once the ticket is fixed, the `-e` mode with role switches becomes illegal.
     */
    set_ts(state->cfg, state->conn, TS_STEPDOWN, ts);
    workload_set_frontier(state, ts);

    /* Signal the checkpoint thread to resume and run the step-down checkpoint. */
    __wt_atomic_store_bool(&state->stepdown_ckpt_due, true);
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
    uint64_t final_ts;
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
            final_ts = __wt_atomic_load_uint64(&state->current_ts);
            frontier_assert(state, final_ts);
            reader_step_down(state, final_ts);
            break;
        case EVENT_SWITCH:
            /* The final event of the term's stream. */
            evq_drain_barrier(state);

            if (!state->generates)
                testutil_assertfmt(__wt_atomic_load_uint64(&state->current_ts) == ev.event_ts,
                  "hand-over: local final counter %" PRIu64 " != sender's final counter %" PRIu64,
                  __wt_atomic_load_uint64(&state->current_ts), ev.event_ts);
            __wt_atomic_store_bool(&state->handover_received, true);
            running = false;
            break;
        case EVENT_NONE:
        case EVENT_PENDING:
            /* Never emitted; the framing lost its way. */
            testutil_die(
              EINVAL, "Node %" PRIu32 ": invalid event read from the source pipe", cfg->node_id);
        }
    }

    return (WT_THREAD_RET_VALUE);
}
