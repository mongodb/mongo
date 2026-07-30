/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The generic node: the phase loop, the WiredTiger connection, the workload engine's state and
 * per-phase lifecycle, the worker event queues, and the timestamp thread.
 *
 * One pipeline serves both roles, coordinating without locks: a generator produces the node's
 * command stream into a self-pipe, a reader demuxes the source pipe - the self-pipe, or a live
 * peer's - to N workers that apply the events, and a timestamp thread advances the frontier. Each
 * stage lives in its own file behind a start/stop pair; the role specifics live in leader.c and
 * follower.c behind the NODE_ROLE operations.
 */

#include "schema_disagg_abort.h"

#include <signal.h>

/*
 * workload_state_create --
 *     Return the node's workload state, zeroed and bound to the configuration. The state has
 *     process lifetime; the control loop and the role transitions keep its connection current.
 */
WORKLOAD_STATE *
workload_state_create(TEST_CONFIG *cfg)
{
    static WORKLOAD_STATE state;
    WT_CLEAR(state);
    state.cfg = cfg;
    return (&state);
}

/*
 * workload_seed_counter --
 *     Seed the monotonic allocator from the previous leader's final counter, so a node stepping up
 *     continues the global epoch/timestamp sequence.
 */
void
workload_seed_counter(WORKLOAD_STATE *state, uint64_t ts)
{
    testutil_assert(state->current_ts <= ts);
    state->current_ts = ts;
}

/*
 * workload_counter_advance --
 *     Advance the monotonic allocator to at least the given applied value, so a follower's counter
 *     tracks everything it applied.
 */
void
workload_counter_advance(WORKLOAD_STATE *state, uint64_t v)
{
    uint64_t cur;
    do {
        cur = __wt_atomic_load_uint64(&state->current_ts);
    } while (cur < v && !__wt_atomic_cas_uint64(&state->current_ts, cur, v));
}

/*
 * workload_running --
 *     The condition every phase loop runs on: true until the phase is directed to quiesce.
 */
bool
workload_running(WORKLOAD_STATE *state)
{
    return (!__wt_atomic_load_bool(&state->stop_phase));
}

/*
 * disagg_opts_init --
 *     Point the test options at the shared PALite page log: the single source of truth for the
 *     disaggregated configuration, used by the nodes and by the parent's recovery opens.
 */
void
disagg_opts_init(const TEST_CONFIG *cfg)
{
    cfg->opts->disagg.is_enabled = true;
    cfg->opts->disagg.page_log = "palite";
    cfg->opts->disagg.page_log_home = cfg->page_log_home;
    cfg->opts->disagg.drain_threads = 1;
}

/*
 * node_open --
 *     Open this node's WiredTiger connection in the given disaggregated mode.
 */
void
node_open(TEST_CONFIG *cfg, const char *disagg_mode, WT_CONNECTION **connp)
{
    char node_home[32];
    testutil_snprintf(node_home, sizeof(node_home), NODE_HOME_FMT, cfg->node_id);

    cfg->opts->disagg.mode = disagg_mode;
    testutil_wiredtiger_open(cfg->opts, node_home, ENV_CONFIG_DEF, NULL, connp, false, false);
}

/*
 * node_stop_requested --
 *     Check for the parent's graceful-stop sentinel. Not consumed: every node must see it.
 */
static bool
node_stop_requested(void)
{
    return (testutil_exists(NULL, STOP_FILE));
}

/*
 * node_switch_request_consume --
 *     Check for the parent's switch-request sentinel and consume it, so one request triggers
 *     exactly one switch. Only the acting node (the leader, or a lone node) may call this.
 */
bool
node_switch_request_consume(void)
{
    if (!testutil_exists(NULL, SWITCH_REQUEST_FILE))
        return (false);
    testutil_assert_errno(remove(SWITCH_REQUEST_FILE) == 0);
    return (true);
}

/* What ends a phase of either role. */
typedef enum { TRIGGER_STOP, TRIGGER_SWITCH } NODE_TRIGGER;

/*
 * node_trigger_wait --
 *     The payload wait of a phase, identical in both roles: sleep until something ends the phase.
 *     The stop sentinel ends any phase; a hand-over report ends it with a switch.
 *
 * Whoever owns the phase's stream consumes the switch request. A phase left with no stream - a
 *     follower whose peer died has neither generator nor reader - consumes it here instead.
 */
static NODE_TRIGGER
node_trigger_wait(WORKLOAD_STATE *state)
{
    while (!node_stop_requested()) {
        if (__wt_atomic_load_bool(&state->handover_received))
            return (TRIGGER_SWITCH);

        const bool abandoned_follower = !state->generates && !state->cfg->peer_alive;
        if (abandoned_follower && node_switch_request_consume())
            return (TRIGGER_SWITCH);

        /*
         * Nothing will ever end this phase once the parent is gone: it owns both sentinels. A lone
         * follower would otherwise idle for as long as the machine is up.
         */
        if (getppid() == 1)
            testutil_die(ECHILD, "Node %" PRIu32 ": parent exited", state->cfg->node_id);
        __wt_sleep(1, 0);
    }
    return (TRIGGER_STOP);
}

/*
 * node_transition_done --
 *     Account for a completed role transition; the transition that completes the swap reports it to
 *     the parent through the numbered sentinel.
 */
static void
node_transition_done(const TEST_CONFIG *cfg, WORKLOAD_STATE *state, bool completes_swap)
{
    ++state->switch_gen;
    if (!completes_swap)
        return;

    char name[64];
    testutil_snprintf(name, sizeof(name), SWITCH_DONE_FMT, state->switch_gen);
    testutil_sentinel(NULL, name);
    println("Node %" PRIu32 ": switch %" PRIu32 " complete", cfg->node_id, state->switch_gen);
}

/*
 * evq_push --
 *     Try to append one event to a worker's ring; false when full.
 */
static bool
evq_push(EVENT_QUEUE *q, const SCHEMA_EVENT *ev)
{
    const uint64_t tail = q->tail; /* single producer */
    if (tail - __wt_atomic_load_uint64(&q->head) >= EVQ_SIZE)
        return (false);
    q->ev[tail % EVQ_SIZE] = *ev;
    __wt_atomic_store_uint64(&q->tail, tail + 1);
    return (true);
}

/*
 * evq_pop --
 *     Try to take one event off a worker's ring; false when empty.
 */
static bool
evq_pop(EVENT_QUEUE *q, SCHEMA_EVENT *ev)
{
    const uint64_t head = q->head; /* single consumer */
    if (head == __wt_atomic_load_uint64(&q->tail))
        return (false);
    *ev = q->ev[head % EVQ_SIZE];
    __wt_atomic_store_uint64(&q->head, head + 1);
    return (true);
}

/*
 * evq_empty --
 *     Report whether a worker's ring is empty.
 */
static bool
evq_empty(EVENT_QUEUE *q)
{
    return (__wt_atomic_load_uint64(&q->head) == __wt_atomic_load_uint64(&q->tail));
}

/*
 * workload_enqueue --
 *     Queue one received schema event for its worker thread, blocking while the ring is full: the
 *     stalled reader stops draining the pipe, which backpressures the leader. Gives up when the
 *     phase is stopping.
 */
void
workload_enqueue(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev)
{
    testutil_assert(ev->thread_id < state->nth_workers);

    EVENT_QUEUE *q = &state->workers[ev->thread_id].evq;
    while (!evq_push(q, ev) && workload_running(state))
        __wt_sleep(0, WT_THOUSAND);
}

/*
 * workload_dequeue --
 *     Take the next event queued for one worker; false when nothing is queued for it.
 */
bool
workload_dequeue(WORKLOAD_STATE *state, uint32_t thread_index, SCHEMA_EVENT *ev)
{
    return (evq_pop(&state->workers[thread_index].evq, ev));
}

/*
 * workload_queue_empty --
 *     Report whether one worker's queue is empty.
 */
bool
workload_queue_empty(WORKLOAD_STATE *state, uint32_t thread_index)
{
    return (evq_empty(&state->workers[thread_index].evq));
}

/*
 * workload_drain_barrier --
 *     Wait until every worker has applied everything queued so far. The follower's reader runs this
 *     before a checkpoint pickup and before a hand-over: everything at or below the checkpoint's
 *     stable frontier must be applied locally before its metadata is adopted, so later publishes
 *     and commits stay above the adopted stable values.
 */
void
workload_drain_barrier(WORKLOAD_STATE *state)
{
    for (uint32_t t = 0; t < state->nth_workers; t++)
        while (
          (!evq_empty(&state->workers[t].evq) || __wt_atomic_load_bool(&state->workers[t].busy)) &&
          workload_running(state))
            __wt_sleep(0, WT_THOUSAND);
}

/*
 * workers_min --
 *     Return the minimum completed value across all worker threads: the frontier with no unfinished
 *     publish or commit at or below it. Returns 0 if any worker has not yet completed an operation
 *     this phase.
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
 *     completed frontier, keeping stable data on published tables only. Runs in both roles; on a
 *     follower, checkpoint pickups may adopt stable values ahead of the local frontier, so the
 *     thread never moves the stable timestamp backwards.
 *
 * It also republishes the connection's durable schema epoch, the gate the generator drops dirty
 *     tables behind. Taking it from the connection rather than from the checkpoint call site keeps
 *     one owner for the value and keeps it right across role transitions: a follower's pickups
 *     advance it too.
 */
static WT_THREAD_RET
thread_ts_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    while (workload_running(state)) {
        /*
         * The single frontier serves both axes: everything at or below it is published and
         * committed, and any commit below it lands in a table created (and published) at a lower
         * value still.
         */
        const uint64_t frontier = workers_min(state);
        if (frontier != 0) {
            const uint64_t cur_stable = query_ts(state->conn, "stable_timestamp");
            if (frontier >= cur_stable)
                set_frontier(state->conn, frontier);
        }

        const uint64_t durable_epoch = query_ts(state->conn, "last_disaggregated_schema_epoch");
        __wt_atomic_store_uint64(&state->ckpt_covered_ts, durable_epoch);

        __wt_sleep(0, 100 * WT_THOUSAND);
    }
    return (WT_THREAD_RET_VALUE);
}

/* The timestamp thread's handle; phases join and restart it but never free it. */
static wt_thread_t ts_thr;

/*
 * workload_start --
 *     Start one phase's threads, the same set in either role: N event-processing workers, the
 *     timestamp thread, the reader (when the phase has an event source), and the generator. Only
 *     the event source differs by role: a leader phase consumes its own generated stream, a
 *     follower phase consumes the peer's.
 */
void
workload_start(WORKLOAD_STATE *state, bool as_leader)
{
    TEST_CONFIG *cfg = state->cfg;
    testutil_assert(cfg->nth <= MAX_TH);

    state->nth_workers = cfg->nth;
    state->leads = as_leader;
    /* A leader feeds itself; so does a follower with no peer. Snapshot it: peer_alive can flip. */
    state->generates = as_leader || !cfg->peer_alive;
    state->stop_phase = false;
    state->reader_stop = false;
    state->generator_stop = false;
    state->handover_received = false;
    /* Start gated: the timestamp thread republishes the connection's durable epoch immediately. */
    state->ckpt_covered_ts = 0;
    state->emitted = state->applied = 0;

    for (uint32_t i = 0; i < cfg->nth; i++) {
        /*
         * The stable frontier must wait for this phase's workers, not trust the previous phase's.
         */
        state->workers[i].completed_ts = 0;
        state->workers[i].busy = false;
        state->workers[i].evq.head = state->workers[i].evq.tail = 0;
    }

    /*
     * Reseed the generator's streams: the worker streams first, then its own pacing stream. Every
     * phase draws, whether it generates or not, so the streams stay in step across role switches.
     */
    for (uint32_t i = 0; i <= cfg->nth; i++)
        testutil_random_from_random(
          &state->gen_rnd[i], i < cfg->nth ? &cfg->opts->data_rnd : &cfg->opts->extra_rnd);

    testutil_check(__wt_thread_create(NULL, &ts_thr, thread_ts_run, state));
    node_workers_start(state);

    /* Every phase has a source: this node's own generator, or a live peer's relay. */
    node_reader_start(state);

    /* Start the generator last, once the machinery consuming its stream is up. */
    if (state->generates)
        node_generator_start(state);
    fflush(stdout);
}

/*
 * workload_stop --
 *     Quiesce and join all of the phase's threads, in dependency order. The generator goes first if
 *     the phase had one, while the reader still drains the self-pipe it may be blocked on; the
 *     reader next, while the workers are still consuming; then the workers drain what the reader
 *     delivered before exiting.
 */
void
workload_stop(WORKLOAD_STATE *state)
{
    node_generator_stop(state);
    node_reader_stop(state);

    __wt_atomic_store_bool(&state->stop_phase, true);
    node_workers_join(state);
    testutil_check(__wt_thread_join(NULL, &ts_thr));
}

/*
 * node_switch_role --
 *     Return the opposite role instance: follower if the current role is leader, and vice versa.
 */
static const NODE_ROLE *
node_switch_role(const NODE_ROLE *role)
{
    return (role->leads ? &node_role_follower : &node_role_leader);
}

/*
 * node_run --
 *     The node's control loop, one state machine for both roles. Each iteration runs one phase:
 *     start the workload in the current role, wait for the trigger that ends the phase, quiesce,
 *     then switch roles through the role's leave/enter operations. Returns the process exit status
 *     once the parent directs a graceful stop; a SIGKILL can end the process at any point instead.
 */
static int
node_run(TEST_CONFIG *cfg, WORKLOAD_STATE *state, const NODE_ROLE *role)
{
    NODE_TRIGGER trigger;

    do {
        workload_start(state, role->leads);
        trigger = node_trigger_wait(state);
        workload_stop(state);

        if (trigger == TRIGGER_SWITCH) {
            /*
             * The counter the ending term finished on. The workload is quiesced and drained, so the
             * node's own counter holds every value the term allocated or adopted - on a peered
             * hand-over the reader asserted it equals the sender's final counter.
             */
            const uint64_t final_counter = state->current_ts;

            role->leave(state, final_counter);
            role = node_switch_role(role);
            role->enter(state, final_counter);
            println("Node %" PRIu32 ": now %s", cfg->node_id, role->name);
            /* The swap-completing transition: entering leadership, or a lone node's only one. */
            node_transition_done(cfg, state, role->leads || !cfg->peer_alive);
        }
    } while (trigger != TRIGGER_STOP);

    /* The parent directed a graceful stop; the last phase is already quiesced. */
    testutil_check(state->conn->close(state->conn, role->close_config));
    println("Node %" PRIu32 ": stopped gracefully as %s", cfg->node_id, role->name);
    return (EXIT_SUCCESS);
}

/*
 * node_main --
 *     Node role entry point: set the node up in its parent-assigned starting role, hand control to
 *     the state machine, and report its exit status.
 */
int
node_main(TEST_CONFIG *cfg)
{
    /* A dead peer must not kill this node with a pipe signal. */
    if (cfg->pipe_write_fd >= 0)
        (void)signal(SIGPIPE, SIG_IGN);

    /* The node's own event source; both ends live for the process, across every role switch. */
    int self_pipe[2];
    testutil_assert_errno(pipe(self_pipe) == 0);
    cfg->self_pipe_read_fd = self_pipe[0];
    cfg->self_pipe_write_fd = self_pipe[1];

    if (chdir(cfg->home) != 0)
        testutil_die(errno, "Node %" PRIu32 " chdir: %s", cfg->node_id, cfg->home);

    disagg_opts_init(cfg);
    cfg->peer_alive = cfg->pipe_read_fd >= 0;

    WORKLOAD_STATE *state = workload_state_create(cfg);

    const NODE_ROLE *role = cfg->start_leader ? &node_role_leader : &node_role_follower;
    node_open(cfg, role->name, &state->conn);
    /*
     * Enter the epoch world before the workload can publish anything, on either role: a follower
     * publishes the operations it applies too. The allocator starts at the same value, so the first
     * event's epoch is above the stable one.
     */
    workload_seed_counter(state, SCHEMA_EPOCH_BOOTSTRAP);
    set_frontier(state->conn, SCHEMA_EPOCH_BOOTSTRAP);
    println("Node %" PRIu32 ": starting as %s", cfg->node_id, role->name);

    return (node_run(cfg, state, role));
}
