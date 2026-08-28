/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The generator stage: the node's command stream - workload, the step-down event, and the switch
 * event that ends a term - written into the self-pipe. Started only for a phase that produces its
 * own stream: a leader always does, and so does a follower with no peer.
 */

#include "schema_disagg_abort.h"

/* The generator's state machine. */
typedef enum {
    GEN_NORMAL,          /* the term's workload */
    GEN_FLUSH_PUBLISHES, /* one-shot: emit the publishes a term must not end holding */
    GEN_BEGIN_STEPDOWN,  /* one-shot: emit the step-down timestamp */
    GEN_STEPDOWN,        /* the step-down: limited workload */
    GEN_SWITCH,          /* one-shot: emit the switch event that ends the stream */
    GEN_STOP             /* terminal: the phase stopped, or the stream ended */
} GENERATOR_PHASE;

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
 * generator_op --
 *     Advance one slot of the given worker thread through the table lifecycle, taking one of its
 *     state's valid moves at random. Reports whether an event was emitted; taking no move is valid,
 *     and lingering widens the window a checkpoint can land in.
 *
 * One gate keeps every move safe to execute: an insert only after its table's create completes, so
 *     its commit exceeds the create's publish epoch or legacy operation timestamp. A table with
 *     data that no checkpoint covers yet is left droppable on purpose - the drop retries in EBUSY
 *     until the checkpoint thread's next checkpoint clears it.
 */
static bool
generator_op(WORKLOAD_STATE *state, uint32_t t, GENERATOR_PHASE phase)
{
    const bool stepping_down = phase == GEN_STEPDOWN;
    WT_RAND_STATE *rnd = &state->gen_rnd[t];
    const uint32_t slot = __wt_random(rnd) % state->cfg->pool_size;
    TABLE_STATE *slot_state = &state->workers[t].table[slot].state;
    /* Set while stepping down; such a slot cannot be dropped until the term ends. */
    bool *stepdown_insert = &state->workers[t].table[slot].stepdown_insert;

    SCHEMA_EVENT ev = {0}; /* EVENT_NONE until a move is taken */
    switch (*slot_state) {
    case TABLE_NONE:
        /* A legacy create is complete immediately; epoch mode publishes it in a later event. */
        ev.type = EVENT_CREATE;
        *slot_state = state->cfg->epoch_less ? TABLE_PUBLISHED : TABLE_CREATED;
        if (state->cfg->unique_tables)
            ++state->workers[t].table[slot].gen;
        break;
    case TABLE_CREATED:
        testutil_assert(!state->cfg->epoch_less);
        switch (__wt_random(rnd) % 3) {
        case 0: /* publish the create */
            ev.type = EVENT_PUBLISH_CREATE;
            *slot_state = TABLE_PUBLISHED;
            break;
        case 1: /* cancel it: an unpublished create dropped again leaves no trace */
            ev.type = EVENT_DROP;
            *slot_state = TABLE_NONE;
            break;
        default: /* linger, widening the op-publish window */
            break;
        }
        break;
    case TABLE_PUBLISHED:
        /* Take (more) data, drop the table, or linger. */
        if (__wt_random(rnd) % GEN_INSERT_ODDS == 0) {
            ev.type = EVENT_INSERT;
            if (stepping_down)
                *stepdown_insert = true;
        } else if (__wt_random(rnd) % GEN_DROP_ODDS == 0 && (!stepping_down || !*stepdown_insert)) {
            /* Such a drop would wait on a checkpoint the step-down cannot take. */
            ev.type = EVENT_DROP;
            *slot_state = state->cfg->epoch_less ? TABLE_NONE : TABLE_DROPPED;
        }
        break;
    case TABLE_DROPPED:
        testutil_assert(!state->cfg->epoch_less);
        /* Publish the drop, which frees the slot, or linger in the window. */
        if (__wt_random(rnd) % 2 == 0) {
            ev.type = EVENT_PUBLISH_DROP;
            *slot_state = TABLE_NONE;
        }
        break;
    }
    if (ev.type == EVENT_NONE)
        return (false);

    ev.thread_id = t;
    testutil_snprintf(ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t, slot,
      state->workers[t].table[slot].gen);
    if (ev.type == EVENT_INSERT) {
        ev.key_min = DATA_KEY_MIN;
        ev.key_max = DATA_KEY_MAX;
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
generator_round(WORKLOAD_STATE *state, uint64_t lead_max, GENERATOR_PHASE phase)
{
    if (state->emitted - __wt_atomic_load_uint64(&state->applied) > lead_max)
        return (false);

    bool emitted = false;

    for (uint32_t t = 0; t < state->worker_count && workload_active(state, STAGE_GENERATOR); t++)
        if (generator_op(state, t, phase))
            emitted = true;
    return (emitted);
}

/* A leading generator's pacing state. */
typedef struct {
    uint64_t lead_max; /* events that may be in flight */
    struct timespec last_poll;

    uint64_t stepdown_emitted;      /* events emitted since the step-down started */
    struct timespec stepdown_start; /* when the step-down event is emitted */
} GENERATOR_PACING;

/*
 * generator_pacing_init --
 *     Initialize the pacing state at the start of a leading phase.
 */
static void
generator_pacing_init(GENERATOR_PACING *pacing, const TEST_CONFIG *cfg)
{
    WT_CLEAR(*pacing);
    __wt_epoch(NULL, &pacing->last_poll);
    /*
     * How much lead the generator may have over the workers: enough to keep every worker fed, and
     * enough that a role switch, which drains what is queued first, completes in reasonable time.
     * Bounding it also keeps a graceful stop prompt, since the stop drains the same queues.
     */
    pacing->lead_max = WT_MAX((uint64_t)cfg->thread_count * GEN_LEAD_PER_THREAD,
      (uint64_t)cfg->switch_interval * GEN_APPLY_RATE_FLOOR);
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
 *     Emit the pending publish for every slot in an unpublished state, before the role transition.
 *     FIXME-WT-18272 FIXME-WT-18284: Remove this function once these tickets are resolved.
 *     Corresponding generator state GEN_FLUSH_PUBLISHES won't be needed anymore, as well.
 */
static void
generator_flush_publishes(WORKLOAD_STATE *state)
{
    if (state->cfg->epoch_less)
        return;

    for (uint32_t t = 0; t < state->worker_count; t++)
        for (uint32_t slot = 0; slot < state->cfg->pool_size; slot++) {
            TABLE_STATE *slot_state = &state->workers[t].table[slot].state;
            if (*slot_state != TABLE_CREATED && *slot_state != TABLE_DROPPED)
                continue;

            SCHEMA_EVENT ev = {0};
            ev.type = *slot_state == TABLE_CREATED ? EVENT_PUBLISH_CREATE : EVENT_PUBLISH_DROP;
            ev.thread_id = t;
            testutil_snprintf(ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t,
              slot, state->workers[t].table[slot].gen);

            *slot_state = *slot_state == TABLE_CREATED ? TABLE_PUBLISHED : TABLE_NONE;
            generator_emit(state, &ev);
        }
}

/*
 * generator_stepdown_ended --
 *     Whether the step-down may complete: the peer adopted the step-down checkpoint or died, or a
 *     lone node emitted its share of events.
 */
static bool
generator_stepdown_ended(WORKLOAD_STATE *state, GENERATOR_PACING *pacing)
{
    /* Zero until the reader's step-down work completes. */
    const uint64_t ckpt_lsn = __wt_atomic_load_uint64(&state->stepdown_ckpt_lsn);
    const bool lone = node_is_lone(state->cfg);

    /* A dead peer cannot adopt the checkpoint. */
    if (ckpt_lsn != 0 && !lone && !state->cfg->peer_alive)
        return (true);

    struct timespec now;
    __wt_epoch(NULL, &now);
    if (WT_TIMEDIFF_SEC(now, pacing->last_poll) < 1)
        return (false); /* Too early, come back later. */
    pacing->last_poll = now;

    const uint64_t stepdown_events = state->emitted - pacing->stepdown_emitted;
    /* Lone node exhausted step-down events or peer adopted the checkpoint. */
    const bool ended = ckpt_lsn != 0 &&
      (lone ? stepdown_events >= GEN_STEPDOWN_MIN_EVENTS : adopted_lsn_read() >= ckpt_lsn);

    if (ended)
        return (true);

    /* Report which part of the step-down stalled. */
    if (WT_TIMEDIFF_SEC(now, pacing->stepdown_start) > MAX_OP_WAIT) {
        if (ckpt_lsn == 0)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": the step-down checkpoint did not complete in %d seconds",
              state->cfg->node_id, MAX_OP_WAIT);
        else if (lone)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": the step-down emitted %" PRIu64 " of %d events in %d seconds",
              state->cfg->node_id, stepdown_events, GEN_STEPDOWN_MIN_EVENTS, MAX_OP_WAIT);
        else
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": peer did not adopt the step-down checkpoint (lsn %" PRIu64
              ") in %d seconds",
              state->cfg->node_id, ckpt_lsn, MAX_OP_WAIT);
    }

    return (false);
}

/*
 * generator_transition_emit --
 *     Emit a transition event (stepdown or switch).
 */
static void
generator_transition_emit(WORKLOAD_STATE *state, EVENT_TYPE type)
{
    SCHEMA_EVENT ev = {0};
    ev.type = type;
    generator_emit(state, &ev);
}

/*
 * thread_generator_run --
 *     The generator thread procedure.
 */
WT_THREAD_RET
thread_generator_run(void *arg)
{
    WORKLOAD_STATE *state = arg;

    GENERATOR_PACING pacing;
    generator_pacing_init(&pacing, state->cfg);

    GENERATOR_PHASE phase = GEN_NORMAL;

    while (phase != GEN_STOP && workload_active(state, STAGE_GENERATOR)) {
        /*
         * Transition-only states count as progress so the loop does not sleep between their
         * actions.
         */
        bool progressed = true;

        switch (phase) {
        case GEN_NORMAL:
            progressed = generator_round(state, pacing.lead_max, GEN_NORMAL);
            phase = generator_switch_requested(&pacing) ? GEN_FLUSH_PUBLISHES : GEN_NORMAL;
            break;
        case GEN_FLUSH_PUBLISHES:
            generator_flush_publishes(state);
            phase = state->leads ? GEN_BEGIN_STEPDOWN : GEN_SWITCH;
            break;
        case GEN_BEGIN_STEPDOWN:
            /*
             * Start the step-down work, then generate a limited workload until the step-down ends.
             */
            generator_transition_emit(state, EVENT_STEPDOWN);
            __wt_epoch(NULL, &pacing.stepdown_start);
            pacing.stepdown_emitted = state->emitted;
            phase = GEN_STEPDOWN;
            break;
        case GEN_STEPDOWN:
            /* A dead peer cannot carry the operations, so stop adding them. */
            progressed = (node_is_lone(state->cfg) || state->cfg->peer_alive) &&
              generator_round(state, pacing.lead_max, GEN_STEPDOWN);
            if (generator_stepdown_ended(state, &pacing)) {
                println("Node %" PRIu32 ": step-down emitted %" PRIu64 " events",
                  state->cfg->node_id, state->emitted - pacing.stepdown_emitted);
                phase = GEN_SWITCH;
            }
            break;
        case GEN_SWITCH:
            /* The stream's last event. */
            generator_transition_emit(state, EVENT_SWITCH);
            phase = GEN_STOP;
            break;
        case GEN_STOP:
            break;
        }

        if (!progressed)
            __wt_sleep(0, 10 * WT_THOUSAND); /* 10 ms */
    }

    return (WT_THREAD_RET_VALUE);
}
