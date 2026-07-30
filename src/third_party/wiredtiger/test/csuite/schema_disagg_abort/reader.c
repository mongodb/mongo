/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The reader stage: the single consumer of the node's event source - the self-pipe when this phase
 * generates, a live peer's otherwise. Queues events for the workers, dispatches checkpoint events
 * to the role, and ends the phase on the hand-over. Its checkpoint bookkeeping lives on this
 * thread's stack, so the engine holds no per-role state.
 */

#include "schema_disagg_abort.h"

/*
 * node_current_role --
 *     Return the role this phase runs. The engine tracks the role as a single bool, so the role
 *     instance is derived from it rather than stored: one source of truth, no invariant to keep.
 */
static const NODE_ROLE *
node_current_role(const WORKLOAD_STATE *state)
{
    return (state->leads ? &node_role_leader : &node_role_follower);
}

/*
 * thread_reader_run --
 *     Drain the node's event source: the self-pipe when this phase generates, the peer's pipe
 *     otherwise. Schema and data events are queued for the worker threads; a checkpoint event is
 *     produced (leading) or picked up after a drain barrier (following); the hand-over event ends
 *     the phase. Pipe EOF can only happen on a peer-fed pipe: it marks the peer dead and turns this
 *     node into a lone follower.
 */
static WT_THREAD_RET
thread_reader_run(void *arg)
{
    WORKLOAD_STATE *state = arg;
    TEST_CONFIG *cfg = state->cfg;
    const int src_fd = state->generates ? cfg->self_pipe_read_fd : cfg->pipe_read_fd;

    WT_SESSION *session;
    testutil_check(state->conn->open_session(state->conn, NULL, NULL, &session));

    /* The role's checkpoint bookkeeping for this phase; only a follower picks checkpoints up. */
    CKPT_CTX ckpt = {0};
    __wt_epoch(NULL, &ckpt.phase_start);
    if (!state->leads)
        testutil_check(state->conn->get_page_log(state->conn, "palite", &ckpt.page_log));

    SCHEMA_EVENT ev;
    bool running = true;
    while (running && !__wt_atomic_load_bool(&state->reader_stop)) {
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
            workload_enqueue(state, &ev);
            break;
        case EVENT_CKPT:
            node_current_role(state)->checkpoint(state, session, &ckpt, &ev);
            break;
        case EVENT_SWITCH:
            /* The final event of the term's stream: this node must step up. */
            workload_drain_barrier(state);
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
        }
    }

    if (ckpt.page_log != NULL)
        testutil_check(ckpt.page_log->terminate(ckpt.page_log, NULL));
    testutil_check(session->close(session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/* The reader thread's handle; its context and results live in the workload state. */
static wt_thread_t reader_thr;
static bool reader_started = false;

/*
 * node_reader_start --
 *     Start the reader thread for a phase with an event source: any leader phase (the self-pipe),
 *     or a follower phase with a live peer. The per-phase hand-over and stop fields were reset by
 *     workload_start.
 */
void
node_reader_start(WORKLOAD_STATE *state)
{
    testutil_assert(!reader_started);
    testutil_check(__wt_thread_create(NULL, &reader_thr, thread_reader_run, state));
    reader_started = true;
}

/*
 * node_reader_stop --
 *     Stop and join the reader thread, if one is running.
 */
void
node_reader_stop(WORKLOAD_STATE *state)
{
    if (!reader_started)
        return;
    __wt_atomic_store_bool(&state->reader_stop, true);
    testutil_check(__wt_thread_join(NULL, &reader_thr));
    reader_started = false;
}
