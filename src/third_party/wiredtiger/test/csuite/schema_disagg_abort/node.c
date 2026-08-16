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
 * One pipeline for both leader and follower roles: a generator produces the node's command stream
 * into a self-pipe, a reader demuxes the source pipe - the self-pipe, or a live peer's - to N
 * workers that apply the events, a timestamp thread advances the frontier, and a checkpoint thread
 * checkpoints on a cadence of its own.
 */

#include "schema_disagg_abort.h"

#include <signal.h>

/*
 * workload_state_create --
 *     Return the node's workload state, zeroed and bound to the configuration. The state has
 *     process lifetime; the control loop and the role transitions keep its connection current.
 */
static WORKLOAD_STATE *
workload_state_create(TEST_CONFIG *cfg)
{
    static WORKLOAD_STATE state;
    WT_CLEAR(state);
    state.cfg = cfg;
    return (&state);
}

/*
 * workload_seed_counter --
 *     Seed the node's timestamps from the previous leader's final timestamp, so a node stepping up
 *     continues the sequence.
 */
static void
workload_seed_counter(WORKLOAD_STATE *state, uint64_t ts)
{
    testutil_assert(state->current_ts <= ts);
    state->current_ts = ts;
}

/*
 * workload_counter_advance --
 *     Advance the node's timestamp to at least the one applied, so a follower tracks every
 *     timestamp it applied.
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
 * workload_active --
 *     The condition a phase loop runs on: true until the shutdown has reached the caller's stage.
 */
bool
workload_active(WORKLOAD_STATE *state, uint32_t stage)
{
    return (__wt_atomic_load_uint32(&state->stop_stage) < stage);
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
static void
node_open(WORKLOAD_STATE *state, const char *disagg_mode)
{
    char node_home[32];
    testutil_snprintf(node_home, sizeof(node_home), NODE_HOME_FMT, state->cfg->node_id);

    state->cfg->opts->disagg.mode = disagg_mode;
    testutil_wiredtiger_open(
      state->cfg->opts, node_home, ENV_CONFIG_DEF, NULL, &state->conn, false, false);

    /* The page log outlives every role the node takes; the connection owns it either way. */
    testutil_check(state->conn->get_page_log(state->conn, "palite", &state->page_log));
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
 * node_stage_stopped --
 *     Whether every thread of a stage has been joined. STAGE_WORKERS is nth_workers threads wide,
 *     the other stages are one thread each, and STAGE_NONE is no thread at all.
 */
bool
node_stage_stopped(WORKLOAD_STATE *state, uint32_t stage)
{
    if (stage != STAGE_WORKERS)
        return (!state->aux_thr[stage].created);

    for (uint32_t i = 0; i < state->nth_workers; i++)
        if (state->workers[i].thr.created)
            return (false);
    return (true);
}

/*
 * node_aux_start --
 *     Start an auxiliary thread for a given stage.
 */
static void
node_aux_start(WORKLOAD_STATE *state, uint32_t stage, WT_THREAD_RET (*func)(void *))
{
    testutil_check(__wt_thread_create(NULL, &state->aux_thr[stage], func, state));
}

/*
 * node_aux_stop --
 *     Stop an auxiliary thread for a given stage.
 */
static void
node_aux_stop(WORKLOAD_STATE *state, uint32_t stage)
{
    testutil_assert(0 < stage && stage < AUX_THR_COUNT);

    /* Previous stage must have stopped. */
    testutil_assert(node_stage_stopped(state, stage - 1));

    /* Stop the current stage. */
    __wt_atomic_store_uint32(&state->stop_stage, stage);
    testutil_check(__wt_thread_join(NULL, &state->aux_thr[stage]));
}

/*
 * workload_start --
 *     Start one phase's threads, the same set in either role: N event-processing workers, the
 *     timestamp thread, the checkpoint thread, the reader (when the phase has an event source), and
 *     the generator. Only the event source differs by role: a leader phase consumes its own
 *     generated stream, a follower phase consumes the peer's.
 */
static void
workload_start(WORKLOAD_STATE *state, bool as_leader)
{
    TEST_CONFIG *cfg = state->cfg;
    testutil_assert(cfg->nth <= MAX_TH);

    state->nth_workers = cfg->nth;
    state->leads = as_leader;
    /* A leader feeds itself; so does a follower with no peer. Snapshot it: peer_alive can flip. */
    state->generates = as_leader || !cfg->peer_alive;
    state->stop_stage = STAGE_NONE;
    state->handover_received = false;
    state->emitted = state->applied = 0;
    state->stepdown_ts = state->stepdown_ckpt_lsn = 0;

    /* Reset workers' state. Note: tables' state survives role transitioning. */
    for (uint32_t i = 0; i < cfg->nth; i++) {
        state->workers[i].completed_ts = 0;
        state->workers[i].busy = false;
        state->workers[i].evq.head = state->workers[i].evq.tail = 0;
        memset(state->workers[i].stepdown_insert, 0, sizeof(state->workers[i].stepdown_insert));
    }

    /* Re-seed the phase's worker and auxiliary streams. */
    for (uint32_t i = 0; i <= cfg->nth; i++)
        testutil_random_from_random(
          &state->gen_rnd[i], i < cfg->nth ? &cfg->opts->data_rnd : &cfg->opts->extra_rnd);

    node_aux_start(state, STAGE_TS, thread_ts_run);
    node_workers_start(state);
    node_aux_start(state, STAGE_CKPT, thread_ckpt_run);
    node_aux_start(state, STAGE_READER, thread_reader_run);

    /* Start the generator last, once the machinery consuming its stream is up. */
    if (state->generates)
        node_aux_start(state, STAGE_GENERATOR, thread_generator_run);
    fflush(stdout);
}

/*
 * workload_stop --
 *     Quiesce and join the phase's threads, walking the shutdown stages in order. The checkpoint
 *     and timestamp threads outlive the workers for a reason: a draining worker blocked on a drop
 *     is waiting for exactly what those two do, a frontier that advances and a checkpoint over it.
 */
static void
workload_stop(WORKLOAD_STATE *state)
{
    node_aux_stop(state, STAGE_GENERATOR);
    node_aux_stop(state, STAGE_READER);
    node_workers_stop(state);
    node_aux_stop(state, STAGE_CKPT);
    node_aux_stop(state, STAGE_TS);
}

/*
 * node_step_down --
 *     Leader to follower.
 */
static void
node_step_down(WORKLOAD_STATE *state, uint64_t final_ts)
{
    WT_CONNECTION *conn = state->conn;

    /* Step-down transition must be done by now. */
    testutil_assert(__wt_atomic_load_uint64(&state->stepdown_ts) != 0);
    testutil_check(conn->reconfigure(conn, "disaggregated=(role=follower)"));

    SCHEMA_EVENT ev = {0};
    ev.type = EVENT_SWITCH;
    ev.event_ts = final_ts;
    /* Peer death is the only reason a hand-over may go undelivered; the write itself detects it. */
    if (!pipe_relay_event(state->cfg, &ev)) {
        testutil_assert(!state->cfg->peer_alive);
        println("Node %" PRIu32 ": no peer to hand over to; continuing alone", state->cfg->node_id);
    }

    /* Reset adopted checkpoint and transition tracking. */
    state->adopted_ckpt_lsn = 0;
    __wt_atomic_store_uint64(&state->stepdown_ts, 0);
    __wt_atomic_store_uint64(&state->stepdown_ckpt_lsn, 0);
}

/*
 * node_step_up --
 *     Follower to leader.
 */
static void
node_step_up(WORKLOAD_STATE *state, uint64_t final_ts)
{
    ckpt_adopt_latest(state);

    testutil_check(state->conn->reconfigure(state->conn, "disaggregated=(role=leader)"));
    workload_seed_counter(state, final_ts);

    /* Restore the timestamps on the new leader's connection. */
    if (final_ts != 0)
        set_frontier(state->conn, final_ts);
}

/*
 * node_role --
 *     Return the role based on whether the node leads.
 */
const NODE_ROLE *
node_role(bool leads)
{
    static const NODE_ROLE node_role_leader = {"leader", NULL, true, leader_checkpoint};
    static const NODE_ROLE node_role_follower = {
      "follower", "debug=(skip_checkpoint=true)", false, follower_checkpoint};

    return (leads ? &node_role_leader : &node_role_follower);
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
             * The timestamp the ending term finished on. The workload is quiesced and drained, so
             * this node holds every timestamp the term allocated or adopted - on a peered hand-over
             * the reader asserted it equals the sender's final timestamp.
             */
            const uint64_t final_ts = state->current_ts;

            if (role->leads)
                node_step_down(state, final_ts);
            else
                node_step_up(state, final_ts);
            role = node_role(!role->leads);
            println("Node %" PRIu32 ": now %s", cfg->node_id, role->name);
            /* The swap-completing transition: entering leadership, or a lone node's only one. */
            node_transition_done(cfg, state, role->leads || !cfg->peer_alive);
        }
    } while (trigger != TRIGGER_STOP);

    /* The parent directed a graceful stop; the last phase is already quiesced. */
    testutil_check(state->page_log->terminate(state->page_log, NULL));
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

    const NODE_ROLE *role = node_role(cfg->start_leader);
    node_open(state, role->name);
    /*
     * Enter the epoch world before the workload can publish anything, on either role: a follower
     * publishes the operations it applies too. Timestamps start at the same point, so the first
     * event's epoch is above the stable one.
     */
    workload_seed_counter(state, SCHEMA_EPOCH_BOOTSTRAP);
    set_frontier(state->conn, SCHEMA_EPOCH_BOOTSTRAP);
    println("Node %" PRIu32 ": starting as %s", cfg->node_id, role->name);

    return (node_run(cfg, state, role));
}
