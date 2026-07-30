/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The generator stage: the node's command stream - workload, checkpoint events, and the switch
 * event that ends a term - written into the self-pipe. Started only for a phase that produces its
 * own stream: a leader always does, and so does a follower with no peer.
 *
 * All switch triggering lives here, never in the control loop: the generator consumes the parent's
 * switch request and turns it into the stream's final event.
 */

#include "schema_disagg_abort.h"

/*
 * generator_running --
 *     The generator's loop condition: true until the engine directs it to exit.
 */
static bool
generator_running(WORKLOAD_STATE *state)
{
    return (!__wt_atomic_load_bool(&state->generator_stop) && workload_running(state));
}

/*
 * generator_emit --
 *     Write one event to the node's self-pipe, blocking while it is full: the workers' consumption
 *     rate backpressures the generator through the pipe and the queues.
 */
static void
generator_emit(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev)
{
    pipe_event_write(state->cfg->self_pipe_write_fd, ev);
    ++state->emitted;
}

/*
 * generator_op --
 *     Generate one schema operation for the given worker thread: pick a slot, flip it between
 *     create and drop, allocate the epoch, and give one create in INSERT_ODDS an insert at a fresh
 *     commit timestamp, which comes from the same allocator as the epoch and so is above the
 *     table's create epoch by construction. Reports whether an event was emitted.
 *
 * Only a slot holding data is gated: a dirty table cannot be dropped until a completed checkpoint
 *     covers its insert (the drop would wedge in EBUSY), so that pick is simply skipped, the
 *     generation-time analogue of trying another slot. Clean tables churn ungated. The slot model
 *     flips at generation time; the worker's bounded EBUSY retry guarantees the executed state
 *     converges to it.
 */
static bool
generator_op(WORKLOAD_STATE *state, uint32_t t)
{
    WT_RAND_STATE *rnd = &state->gen_rnd[t];

    const uint32_t slot = __wt_random(rnd) % state->cfg->pool_size;
    const bool is_create = !state->table_exists[t][slot];
    /* A clean table (commit timestamp 0) is droppable at once; a dirty one waits for coverage. */
    if (!is_create && state->table_commit_ts[t][slot] != 0 &&
      state->table_commit_ts[t][slot] > __wt_atomic_load_uint64(&state->ckpt_covered_ts))
        return (false);
    state->table_exists[t][slot] = is_create;

    SCHEMA_EVENT ev = {0};
    ev.type = is_create ? EVENT_CREATE : EVENT_DROP;
    ev.thread_id = t;
    ev.event_ts = __wt_atomic_add_uint64(&state->current_ts, 1);
    testutil_snprintf(ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t, slot);
    generator_emit(state, &ev);

    if (is_create) {
        /* Most creates leave the table clean, so the churn is not gated on checkpoints. */
        state->table_commit_ts[t][slot] = 0;
        if (__wt_random(rnd) % INSERT_ODDS == 0) {
            ev.type = EVENT_INSERT;
            ev.event_ts = __wt_atomic_add_uint64(&state->current_ts, 1);
            ev.key_min = DATA_KEY_MIN;
            ev.key_max = DATA_KEY_MAX;
            state->table_commit_ts[t][slot] = ev.event_ts;
            generator_emit(state, &ev);
        }
    }
    return (true);
}

/*
 * generator_round --
 *     Feed every worker thread one generated operation, round-robin. Reports whether anything was
 *     emitted: nothing is while the lead over the workers is spent, or when every pick was an
 *     uncovered drop, and the caller waits instead of spinning on the slot model.
 */
static bool
generator_round(WORKLOAD_STATE *state, uint64_t lead_max)
{
    if (state->emitted - __wt_atomic_load_uint64(&state->applied) > lead_max)
        return (false);

    bool emitted = false;

    for (uint32_t t = 0; t < state->nth_workers && generator_running(state); t++)
        if (generator_op(state, t))
            emitted = true;
    return (emitted);
}

/*
 * A leading generator's pacing state: the checkpoint cadence, the sentinel poll throttle, and the
 * lead it may build over its workers.
 */
typedef struct {
    WT_RAND_STATE *rnd; /* the generator's own rnd stream, used for the checkpoint intervals */
    struct timespec last_ckpt;
    struct timespec last_poll;
    uint64_t ckpt_wait; /* seconds until the next checkpoint event is due */
    uint64_t lead_max;  /* events that may be in flight; UINT64_MAX when nothing bounds it */
} GENERATOR_PACING;

/*
 * generator_pacing_init --
 *     Initialize the pacing state at the start of a leading phase.
 */
static void
generator_pacing_init(GENERATOR_PACING *pacing, const TEST_CONFIG *cfg, WT_RAND_STATE *rnd)
{
    pacing->rnd = rnd;
    __wt_epoch(NULL, &pacing->last_ckpt);
    pacing->last_poll = pacing->last_ckpt;
    pacing->ckpt_wait = __wt_random(rnd) % MAX_CKPT_INVL;
    /* Bound the lead so a hand-over drains inside one switch period; no switches, no bound. */
    pacing->lead_max = cfg->switch_interval == 0 ?
      UINT64_MAX :
      WT_MAX(cfg->switch_interval * GEN_APPLY_RATE_FLOOR, GEN_LEAD_MIN);
}

/*
 * generator_ckpt_due --
 *     Pace the stream's checkpoint events: true when the current random interval has elapsed,
 *     starting the next one.
 */
static bool
generator_ckpt_due(GENERATOR_PACING *pacing)
{
    struct timespec now;
    __wt_epoch(NULL, &now);
    if ((uint64_t)WT_TIMEDIFF_SEC(now, pacing->last_ckpt) < pacing->ckpt_wait)
        return (false);
    pacing->last_ckpt = now;
    pacing->ckpt_wait = __wt_random(pacing->rnd) % MAX_CKPT_INVL;
    return (true);
}

/*
 * generator_switch_requested --
 *     Watch for the parent's switch request, polling the sentinel at most once a second (the
 *     cadence the control loop's own waits use).
 */
static bool
generator_switch_requested(GENERATOR_PACING *pacing)
{
    struct timespec now;
    __wt_epoch(NULL, &now);
    if (WT_TIMEDIFF_SEC(now, pacing->last_poll) < 1)
        return (false);
    pacing->last_poll = now;
    return (node_switch_request_consume());
}

/*
 * thread_generator_run --
 *     The generator thread: feeds workload rounds into the self-pipe, a checkpoint event whenever
 *     one is due, and, once the parent requests a switch, the hand-over event that ends the stream
 *     and the phase.
 */
static WT_THREAD_RET
thread_generator_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    /* The pacing stream is the one past the workers' streams. */
    GENERATOR_PACING pacing;
    generator_pacing_init(&pacing, state->cfg, &state->gen_rnd[state->nth_workers]);

    while (generator_running(state)) {
        if (!generator_round(state, pacing.lead_max))
            __wt_sleep(0, WT_THOUSAND);

        if (generator_ckpt_due(&pacing)) {
            SCHEMA_EVENT ev = {0};
            ev.type = EVENT_CKPT;
            generator_emit(state, &ev);
        }

        if (generator_switch_requested(&pacing)) {
            /* The stream's last event, carrying the counter the next leader continues from. */
            SCHEMA_EVENT ev = {0};
            ev.type = EVENT_SWITCH;
            ev.event_ts = __wt_atomic_load_uint64(&state->current_ts);
            generator_emit(state, &ev);
            break;
        }
    }
    return (WT_THREAD_RET_VALUE);
}

/* The generator thread's handle; its state lives in the workload state. */
static wt_thread_t generator_thr;
static bool generator_started = false;

/*
 * node_generator_start --
 *     Start the generator thread for a phase that produces its own stream. Started last of the
 *     phase's threads, once the machinery consuming the stream is up.
 */
void
node_generator_start(WORKLOAD_STATE *state)
{
    testutil_assert(!generator_started);
    testutil_check(__wt_thread_create(NULL, &generator_thr, thread_generator_run, state));
    generator_started = true;
}

/*
 * node_generator_stop --
 *     Stop and join the generator thread, if one is running. First of the phase's threads to go,
 *     while the reader still drains the self-pipe it may be blocked on.
 */
void
node_generator_stop(WORKLOAD_STATE *state)
{
    if (!generator_started)
        return;
    __wt_atomic_store_bool(&state->generator_stop, true);
    testutil_check(__wt_thread_join(NULL, &generator_thr));
    generator_started = false;
}
