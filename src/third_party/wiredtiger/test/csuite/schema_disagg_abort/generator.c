/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The generator stage: the node's command stream - workload, the step-down marker, and the switch
 * event that ends a term - written into the self-pipe. Started only for a phase that produces its
 * own stream: a leader always does, and so does a follower with no peer.
 */

#include "schema_disagg_abort.h"

/* The generator's state machine. */
typedef enum {
    GEN_NORMAL,          /* the term's workload */
    GEN_FLUSH_PUBLISHES, /* one-shot: emit the publishes a term must not end holding */
    GEN_BEGIN_STEPDOWN,  /* one-shot: emit the step-down timestamp */
    GEN_STEPDOWN,        /* the step-down window: limited workload */
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
 * One gate keeps every move safe to execute: an insert only after its table's create published, so
 *     its commit exceeds the create's epoch. A published table with data that no checkpoint covers
 *     yet is left droppable on purpose - the drop retries in EBUSY until the checkpoint thread's
 *     next checkpoint clears it.
 */
static bool
generator_op(WORKLOAD_STATE *state, uint32_t t, GENERATOR_PHASE phase)
{
    const bool stepping_down = phase == GEN_STEPDOWN;
    WT_RAND_STATE *rnd = &state->gen_rnd[t];
    const uint32_t slot = __wt_random(rnd) % state->cfg->pool_size;
    TABLE_STATE *slot_state = &state->workers[t].table_state[slot];
    /* Set while stepping down; such a slot cannot be dropped until the term ends. */
    bool *stepdown_insert = &state->workers[t].stepdown_insert[slot];

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
        if (__wt_random(rnd) % INSERT_ODDS == 0) {
            ev.type = EVENT_INSERT;
            if (stepping_down)
                *stepdown_insert = true;
        } else if (__wt_random(rnd) % DROP_ODDS == 0 && (!stepping_down || !*stepdown_insert)) {
            /* Such a drop would wait on a checkpoint the step-down cannot take. */
            ev.type = EVENT_DROP;
            *slot_state = TABLE_DROPPED;
        }
        break;
    case TABLE_DROPPED:
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
    testutil_snprintf(ev.uri, sizeof(ev.uri), SCHEMA_TABLE_FMT, state->cfg->node_id, t, slot);
    if (ev.type == EVENT_INSERT) {
        ev.key_min = DATA_KEY_MIN;
        ev.key_max = DATA_KEY_MAX;
    }
    /* This event needs a timestamp. */
    if (ev.type == EVENT_INSERT || ev.type == EVENT_PUBLISH_CREATE || ev.type == EVENT_PUBLISH_DROP)
        ev.event_ts = __wt_atomic_add_uint64(&state->current_ts, 1);

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

    for (uint32_t t = 0; t < state->nth_workers && workload_active(state, STAGE_GENERATOR); t++)
        if (generator_op(state, t, phase))
            emitted = true;
    return (emitted);
}

/*
 * A leading generator's pacing state: the sentinel poll throttle, and the lead it may build over
 * its workers.
 */
typedef struct {
    struct timespec last_poll;
    uint64_t lead_max; /* events that may be in flight; UINT64_MAX when nothing bounds it */
} GENERATOR_PACING;

/*
 * generator_pacing_init --
 *     Initialize the pacing state at the start of a leading phase.
 */
static void
generator_pacing_init(GENERATOR_PACING *pacing, const TEST_CONFIG *cfg)
{
    __wt_epoch(NULL, &pacing->last_poll);
    /* Bound the lead so a hand-over drains inside one switch period; no switches, no bound. */
    pacing->lead_max = cfg->switch_interval == 0 ?
      UINT64_MAX :
      WT_MAX(cfg->switch_interval * GEN_APPLY_RATE_FLOOR, GEN_LEAD_MIN);
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

            *slot_state = *slot_state == TABLE_CREATED ? TABLE_PUBLISHED : TABLE_NONE;
            generator_emit(state, &ev);
        }
}

/*
 * generator_stepdown_ended --
 *     Whether the step-down may complete: the peer adopted the step-down checkpoint, or died.
 */
static bool
generator_stepdown_ended(
  WORKLOAD_STATE *state, GENERATOR_PACING *pacing, const struct timespec *start)
{
    /* Zero until the reader's step-down work completes. */
    const uint64_t ckpt_lsn = __wt_atomic_load_uint64(&state->stepdown_ckpt_lsn);

    /* The peer died mid-window: nothing left to wait for, and no next leader to carry it. */
    if (ckpt_lsn != 0 && !state->cfg->peer_alive)
        return (true);

    struct timespec now;
    __wt_epoch(NULL, &now);
    if (WT_TIMEDIFF_SEC(now, pacing->last_poll) < 1)
        return (false);
    pacing->last_poll = now;

    if (ckpt_lsn != 0 && adopted_lsn_read() >= ckpt_lsn)
        return (true);

    /* Which side stalled: our own step-down work, or the peer's adoption of it. */
    if (WT_TIMEDIFF_SEC(now, *start) > MAX_OP_WAIT) {
        if (ckpt_lsn == 0)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": the step-down checkpoint did not complete in %d seconds",
              state->cfg->node_id, MAX_OP_WAIT);
        testutil_die(ETIMEDOUT,
          "Node %" PRIu32 ": peer did not adopt the step-down checkpoint (lsn %" PRIu64
          ") in %d seconds",
          state->cfg->node_id, ckpt_lsn, MAX_OP_WAIT);
    }
    return (false);
}

/*
 * generator_transition_emit --
 *     Emit a transition event (stepdown or switch) stamped with the current counter.
 */
static void
generator_transition_emit(WORKLOAD_STATE *state, EVENT_TYPE type)
{
    SCHEMA_EVENT ev = {0};
    ev.type = type;
    ev.event_ts = __wt_atomic_load_uint64(&state->current_ts);
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

    struct timespec start = {0};
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
             * Tell the reader to start the step-down work. If the peer exists, then generate a
             * limited workload until the peer adopts the step-down checkpoint.
             */
            generator_transition_emit(state, EVENT_STEPDOWN);
            __wt_epoch(NULL, &start);
            phase = state->cfg->peer_alive ? GEN_STEPDOWN : GEN_SWITCH;
            break;
        case GEN_STEPDOWN:
            /* Stop adding operations the moment the peer is gone. */
            progressed =
              state->cfg->peer_alive && generator_round(state, pacing.lead_max, GEN_STEPDOWN);
            phase = generator_stepdown_ended(state, &pacing, &start) ? GEN_SWITCH : GEN_STEPDOWN;
            break;
        case GEN_SWITCH:
            /* The stream's last event, carrying the counter the next leader continues from. */
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
