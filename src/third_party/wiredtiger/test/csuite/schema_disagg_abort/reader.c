/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The reader stage: the single consumer of the node's event source - the self-pipe when this phase
 * generates, a live peer's otherwise. Queues events for the workers and ends the phase on the
 * hand-over. Checkpoints are not in the stream; the checkpoint thread owns them.
 */

#include "schema_disagg_abort.h"

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
