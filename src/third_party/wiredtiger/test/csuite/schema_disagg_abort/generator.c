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
 * generator_emit --
 *     Write one event to the node's self-pipe, blocking while it is full: the workers' consumption
 *     rate backpressures the generator through the pipe and the queues.
 */
static void
generator_emit(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev)
{
    testutil_assert(ev->type != EVENT_NONE);
    pipe_event_write(state->cfg->self_pipe_write_fd, ev);
    ++state->emitted;
}

/*
 * table_state_expects_checkpoint --
 *     Report whether a state waits for a checkpoint to cover the slot's value before it can move
 *     on.
 */
static bool
table_state_expects_checkpoint(TABLE_STATE cur)
{
    return (cur == TABLE_CREATE_PUBLISHED || cur == TABLE_DIRTY || cur == TABLE_DROP_PUBLISHED);
}

/*
 * table_state_after_checkpoint --
 *     The state a waiting slot moves to once a completed checkpoint has covered the value it waits
 *     on: a covered create or insert makes the table durable, a covered drop frees the slot.
 *     Anything else stays put.
 */
static TABLE_STATE
table_state_after_checkpoint(TABLE_STATE cur, uint64_t wait, uint64_t covered)
{
    if ((cur == TABLE_CREATE_PUBLISHED || cur == TABLE_DIRTY) && covered >= wait)
        return (TABLE_DURABLE);
    if (cur == TABLE_DROP_PUBLISHED && covered >= wait)
        return (TABLE_NONE);
    return (cur);
}

/*
 * generator_op --
 *     Advance one slot of the given worker thread through the table lifecycle, taking one of its
 *     state's valid moves at random. Reports whether an event was emitted; taking no move is valid,
 *     and lingering in the unpublished states widens the window checkpoints land in.
 *
 * Two gates keep every move safe to execute: an insert only after its table's create published, so
 *     its commit exceeds the create's epoch, and a drop only once a checkpoint covers the table,
 *     since an uncovered drop wedges its worker in EBUSY.
 */
static bool
generator_op(WORKLOAD_STATE *state, uint32_t t)
{
    WT_RAND_STATE *rnd = &state->gen_rnd[t];
    const uint32_t slot = __wt_random(rnd) % state->cfg->pool_size;
    TABLE_STATE *slot_state = &state->workers[t].table_state[slot];
    uint64_t *wait = &state->workers[t].table_wait_ts[slot];
    const uint64_t covered = __wt_atomic_load_uint64(&state->ckpt_covered_ts);

    *slot_state = table_state_after_checkpoint(*slot_state, *wait, covered);

    SCHEMA_EVENT ev = {0}; /* EVENT_NONE until a move is taken */
    switch (*slot_state) {
    case TABLE_NONE:
        /* The only move: a fresh table; its publish is a later event of its own. */
        ev.type = EVENT_CREATE;
        *slot_state = TABLE_CREATED;
        break;
    case TABLE_CREATED:
        switch (__wt_random(rnd) % 3) {
        case 0: /* publish the create */
            ev.type = EVENT_PUBLISH_CREATE;
            *slot_state = TABLE_CREATE_PUBLISHED;
            break;
        case 1: /* cancel it: an unpublished create dropped again leaves no trace */
            ev.type = EVENT_DROP;
            *slot_state = TABLE_NONE;
            break;
        default: /* linger, widening the op-publish window */
            break;
        }
        break;
    case TABLE_CREATE_PUBLISHED:
        /* Waiting for the create's coverage; meanwhile the table may take data. */
        if (__wt_random(rnd) % INSERT_ODDS == 0) {
            ev.type = EVENT_INSERT;
            *slot_state = TABLE_DIRTY;
        }
        break;
    case TABLE_DIRTY:
        /* Nothing to do: waiting for the data's coverage. */
        break;
    case TABLE_DURABLE:
        /* Covered, so droppable - or take (more) data first, and wait again. */
        if (__wt_random(rnd) % INSERT_ODDS == 0) {
            ev.type = EVENT_INSERT;
            *slot_state = TABLE_DIRTY;
        } else {
            ev.type = EVENT_DROP;
            *slot_state = TABLE_DROPPED;
        }
        break;
    case TABLE_DROPPED:
        /* Publish the drop, or linger in the window. */
        if (__wt_random(rnd) % 2 == 0) {
            ev.type = EVENT_PUBLISH_DROP;
            *slot_state = TABLE_DROP_PUBLISHED;
        }
        break;
    case TABLE_DROP_PUBLISHED:
        /* Wait the drop's coverage out, or recreate the slot over the pending remove. */
        if (__wt_random(rnd) % 2 == 0) {
            ev.type = EVENT_CREATE;
            *slot_state = TABLE_CREATED;
        }
        break;
    }
    if (ev.type == EVENT_NONE)
        return (false);

    /* A slot left waiting gets the value a checkpoint must cover. */
    ev.thread_id = t;
    testutil_snprintf(ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t, slot);
    if (ev.type == EVENT_INSERT) {
        ev.key_min = DATA_KEY_MIN;
        ev.key_max = DATA_KEY_MAX;
    }
    if (table_state_expects_checkpoint(*slot_state)) {
        /* Only a valued event may leave a slot waiting. */
        testutil_assertfmt(ev.type == EVENT_INSERT || ev.type == EVENT_PUBLISH_CREATE ||
            ev.type == EVENT_PUBLISH_DROP,
          "state %d waits on event type %d, which carries no value", *slot_state, ev.type);
        *wait = ev.event_ts = __wt_atomic_add_uint64(&state->current_ts, 1);
    }

    generator_emit(state, &ev);
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

    for (uint32_t t = 0; t < state->nth_workers && workload_active(state, STAGE_GENERATOR); t++)
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
 * generator_flush_publishes --
 *     Emit the pending publish for every slot in an unpublished state, before the hand-over event.
 *     A step-down clears WiredTiger's shared metadata queue and the URIs are namespaced by origin
 *     node, so a publish left unemitted here could never be emitted by anyone afterwards.
 */
static void
generator_flush_publishes(WORKLOAD_STATE *state)
{
    for (uint32_t t = 0; t < state->nth_workers; t++)
        for (uint32_t slot = 0; slot < state->cfg->pool_size; slot++) {
            TABLE_STATE *slot_state = &state->workers[t].table_state[slot];
            if (*slot_state != TABLE_CREATED && *slot_state != TABLE_DROPPED)
                continue;

            SCHEMA_EVENT ev = {0};
            ev.type = *slot_state == TABLE_CREATED ? EVENT_PUBLISH_CREATE : EVENT_PUBLISH_DROP;
            ev.thread_id = t;
            ev.event_ts = __wt_atomic_add_uint64(&state->current_ts, 1);
            testutil_snprintf(
              ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t, slot);

            *slot_state =
              *slot_state == TABLE_CREATED ? TABLE_CREATE_PUBLISHED : TABLE_DROP_PUBLISHED;
            state->workers[t].table_wait_ts[slot] = ev.event_ts;
            generator_emit(state, &ev);
        }
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

    while (workload_active(state, STAGE_GENERATOR)) {
        if (!generator_round(state, pacing.lead_max))
            __wt_sleep(0, WT_THOUSAND);

        if (generator_ckpt_due(&pacing)) {
            SCHEMA_EVENT ev = {0};
            ev.type = EVENT_CKPT;
            generator_emit(state, &ev);
        }

        if (generator_switch_requested(&pacing)) {
            generator_flush_publishes(state);
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
 * node_generator_join --
 *     Join the generator thread, if one is running. The stage it exits on is the caller's to set.
 */
void
node_generator_join(void)
{
    if (!generator_started)
        return;

    testutil_check(__wt_thread_join(NULL, &generator_thr));
    generator_started = false;
}
