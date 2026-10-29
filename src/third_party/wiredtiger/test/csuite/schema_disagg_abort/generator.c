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
    GEN_NORMAL,         /* the term's workload */
    GEN_BEGIN_STEPDOWN, /* one-shot: the step-down event; reserves timestamps for publishes */
    GEN_STEPDOWN,       /* the step-down: limited workload */
    GEN_HANDOVER,       /* one-shot: publish what is pending, then emit the switch event */
    GEN_STOP            /* terminal: the phase stopped, or the stream ended */
} GENERATOR_PHASE;

/* The generator of one phase. */
typedef struct {
    WORKLOAD_STATE *state;
    GENERATOR_PHASE phase;

    uint64_t emitted;          /* events emitted this phase */
    uint64_t lead_max;         /* events that may be in flight */
    struct timespec last_poll; /* the last poll of the parent's sentinel or the peer's adoption */

    uint64_t stepdown_emitted;      /* the step-down event's stream position, else 0 */
    struct timespec stepdown_start; /* when the step-down event was emitted */
} GENERATOR;

/*
 * generator_emit --
 *     Write one table's event to the self-pipe, blocking while it is full: the workers' consumption
 *     rate backpressures the generator through the pipe and the queues.
 */
static void
generator_emit(GENERATOR *gen, uint32_t t, uint32_t slot, SCHEMA_EVENT *ev)
{
    testutil_assert(ev->type != EVENT_NONE);
    ev->thread_id = t;
    ev->slot = slot;
    testutil_snprintf(
      ev->uri, sizeof(ev->uri), SCHEMA_TABLE_FMT, gen->state->cfg->node_id, t, slot);
    pipe_event_write(gen->state->cfg->self_pipe_write_fd, ev);
    gen->state->workers[t].table[slot].emitted_at = ++gen->emitted;
}

/*
 * generator_ts_source --
 *     Returns the source of the publish timestamp: reserved range or next available timestamp.
 */
static TIMESTAMP_SOURCE
generator_ts_source(const GENERATOR *gen, const TABLE *table)
{
    return (table->emitted_at < gen->stepdown_emitted ? TIMESTAMP_RESERVED : TIMESTAMP_NEXT);
}

/*
 * generator_table_pending --
 *     Whether the table has an operation awaiting its publish.
 */
static bool
generator_table_pending(const TABLE *table)
{
    return (table->state == TABLE_CREATED || table->state == TABLE_DROPPED);
}

/*
 * generator_publish --
 *     Publish a table's pending operation.
 */
static TABLE_STATE
generator_publish(GENERATOR *gen, const TABLE *table, SCHEMA_EVENT *ev)
{
    TABLE_STATE next_state = TABLE_NONE;

    switch (table->state) {
    case TABLE_CREATED:
        ev->type = EVENT_PUBLISH_CREATE;
        next_state = TABLE_PUBLISHED;
        break;
    case TABLE_DROPPED:
        ev->type = EVENT_PUBLISH_DROP;
        next_state = TABLE_REMOVED;
        break;
    case TABLE_NONE:
    case TABLE_PUBLISHED:
    case TABLE_REMOVED:
        testutil_die(
          EINVAL, "Publish of a table with no pending operation, state %d", table->state);
    }
    ev->ts_source = generator_ts_source(gen, table);
    return (next_state);
}

/*
 * generator_table_droppable --
 *     Whether this table can be dropped now.
 */
static bool
generator_table_droppable(GENERATOR *gen, TABLE *table)
{
    WORKLOAD_STATE *state = gen->state;

    if (table->uncovered_insert)
        return (false);

    /*
     * A drop can wait on the step-down checkpoint, which waits for the operations that predate the
     * step-down to be published: a publish queued behind the drop would never apply.
     */
    if (gen->phase == GEN_STEPDOWN && __wt_atomic_load_uint64(&state->stepdown_ckpt_lsn) == 0)
        return (false);

    /* Legacy mode has no epochs to cover, and a lone node or a dead peer has nobody to protect. */
    if (state->cfg->epoch_less || node_is_lone(state->cfg) || !node_peer_alive(state->cfg))
        return (true);

    const uint64_t create_epoch = __wt_atomic_load_uint64(&table->create_epoch);
    return (create_epoch != WT_SCHEMA_EPOCH_NONE &&
      __wt_atomic_load_uint64(&state->adopted_ckpt_epoch) >= create_epoch);
}

/*
 * generator_visit_none --
 *     Create a table. A legacy create is complete immediately; epoch mode publishes it in a later
 *     event.
 */
static TABLE_STATE
generator_visit_none(GENERATOR *gen, SCHEMA_EVENT *ev)
{
    ev->type = EVENT_CREATE;
    return (gen->state->cfg->epoch_less ? TABLE_PUBLISHED : TABLE_CREATED);
}

/*
 * generator_visit_created --
 *     Publish the create, cancel it with a drop, or linger, widening the op-publish window.
 */
static TABLE_STATE
generator_visit_created(GENERATOR *gen, WT_RAND_STATE *rnd, const TABLE *table, SCHEMA_EVENT *ev)
{
    testutil_assert(!gen->state->cfg->epoch_less);

    TABLE_STATE next_state = TABLE_CREATED;

    /*
     * A create whose publish holds a reserved timestamp cannot be canceled: the timestamp must be
     * used. Choices: 0 - publish, 1 - linger, and otherwise 2 - cancel with a drop.
     */
    const uint32_t choices = generator_ts_source(gen, table) == TIMESTAMP_RESERVED ? 2 : 3;

    switch (__wt_random(rnd) % choices) {
    case 0:
        next_state = generator_publish(gen, table, ev);
        break;
    case 1:
        /* Linger: do nothing, just stay in the created state. */
        break;
    case 2:
        ev->type = EVENT_DROP;
        next_state = TABLE_NONE;
        break;
    default:
        break;
    }
    return (next_state);
}

/*
 * generator_visit_published --
 *     Take (more) data, drop the table, or linger.
 */
static TABLE_STATE
generator_visit_published(GENERATOR *gen, WT_RAND_STATE *rnd, TABLE *table, SCHEMA_EVENT *ev)
{
    TABLE_STATE next_state = TABLE_PUBLISHED;

    if (__wt_random(rnd) % GEN_INSERT_ODDS == 0) {
        ev->type = EVENT_INSERT;
        ev->key_min = DATA_KEY_MIN;
        ev->key_max = DATA_KEY_MAX;
        /* No checkpoint of this phase can cover it, so the table stops being droppable. */
        if (gen->phase == GEN_STEPDOWN || !gen->state->leads)
            table->uncovered_insert = true;
    } else if (__wt_random(rnd) % GEN_DROP_ODDS == 0 && generator_table_droppable(gen, table)) {
        ev->type = EVENT_DROP;
        /* Consume the create's epoch: a recreated table publishes its own. */
        __wt_atomic_store_uint64(&table->create_epoch, WT_SCHEMA_EPOCH_NONE);
        next_state = gen->state->cfg->epoch_less ? TABLE_NONE : TABLE_DROPPED;
    }
    return (next_state);
}

/*
 * generator_visit_dropped --
 *     Publish the drop, or linger in the window.
 */
static TABLE_STATE
generator_visit_dropped(GENERATOR *gen, WT_RAND_STATE *rnd, const TABLE *table, SCHEMA_EVENT *ev)
{
    TABLE_STATE next_state = TABLE_DROPPED;

    testutil_assert(!gen->state->cfg->epoch_less);
    if (__wt_random(rnd) % 2 == 0)
        next_state = generator_publish(gen, table, ev);
    return (next_state);
}

/*
 * generator_visit_removed --
 *     Free the table once the stable epoch passes the published drop, so a recreate cannot precede
 *     it; zero is not applied yet.
 */
static TABLE_STATE
generator_visit_removed(GENERATOR *gen, TABLE *table)
{
    TABLE_STATE next_state = TABLE_REMOVED;
    const uint64_t published_drop = __wt_atomic_load_uint64(&table->drop_epoch);

    if (published_drop != WT_SCHEMA_EPOCH_NONE &&
      __wt_atomic_load_uint64(&gen->state->stable_epoch) >= published_drop) {
        __wt_atomic_store_uint64(&table->drop_epoch, WT_SCHEMA_EPOCH_NONE);
        next_state = TABLE_NONE;
    }
    return (next_state);
}

/*
 * generator_op --
 *     Visit one table of the given worker thread and take one of its state's valid moves at random.
 *     Reports whether an event was emitted; taking no move is valid, and lingering widens the
 *     window a checkpoint can land in.
 */
static bool
generator_op(GENERATOR *gen, uint32_t t)
{
    WT_RAND_STATE *rnd = &gen->state->workers[t].rnd;
    const uint32_t slot = __wt_random(rnd) % gen->state->cfg->pool_size;
    TABLE *table = &gen->state->workers[t].table[slot];

    SCHEMA_EVENT ev = {0}; /* EVENT_NONE until a move is taken */
    switch (table->state) {
    case TABLE_NONE:
        table->state = generator_visit_none(gen, &ev);
        break;
    case TABLE_CREATED:
        table->state = generator_visit_created(gen, rnd, table, &ev);
        break;
    case TABLE_PUBLISHED:
        table->state = generator_visit_published(gen, rnd, table, &ev);
        break;
    case TABLE_DROPPED:
        table->state = generator_visit_dropped(gen, rnd, table, &ev);
        break;
    case TABLE_REMOVED:
        table->state = generator_visit_removed(gen, table);
        break;
    }
    if (ev.type == EVENT_NONE)
        return (false);

    generator_emit(gen, t, slot, &ev);
    return (true);
}

/*
 * generator_peer_lost --
 *     Whether the node lost the peer that would carry its step-down's operations.
 */
static bool
generator_peer_lost(const GENERATOR *gen)
{
    return (!node_is_lone(gen->state->cfg) && !node_peer_alive(gen->state->cfg));
}

/*
 * generator_workload_allowed --
 *     Whether the generator may emit operations now.
 */
static bool
generator_workload_allowed(const GENERATOR *gen)
{
    /* The lead over the workers is spent. */
    if (gen->emitted - __wt_atomic_load_uint64(&gen->state->applied) > gen->lead_max)
        return (false);

    bool allowed = false;
    switch (gen->phase) {
    case GEN_NORMAL:
        allowed = true;
        break;
    case GEN_STEPDOWN:
        /* A lost peer cannot carry the step-down's operations. */
        allowed = !generator_peer_lost(gen);
        break;
    case GEN_BEGIN_STEPDOWN:
    case GEN_HANDOVER:
        /* These phases emit only their transition event. */
        break;
    case GEN_STOP:
        /* The generator loop ends before a round in the stopped phase. */
        testutil_assertfmt(false, "Unexpected generator phase: %d", gen->phase);
    }
    return (allowed);
}

/*
 * generator_round --
 *     Feed every worker thread one generated operation, round-robin, when the generator may emit.
 *     When nothing is emitted, wait instead of spinning: the phase has no workload, the lead over
 *     the workers is spent, or every visit took no move.
 */
static void
generator_round(GENERATOR *gen)
{
    bool emitted = false;

    if (generator_workload_allowed(gen))
        for (uint32_t t = 0; t < gen->state->worker_count; t++)
            emitted = generator_op(gen, t) || emitted;
    if (!emitted)
        __wt_sleep(0, 10 * WT_THOUSAND); /* 10 ms */
}

/*
 * generator_init --
 *     Initialize the generator at the start of a phase.
 */
static void
generator_init(GENERATOR *gen, WORKLOAD_STATE *state)
{
    WT_CLEAR(*gen);
    gen->state = state;
    gen->phase = GEN_NORMAL;
    __wt_epoch(NULL, &gen->last_poll);
    /*
     * How much lead the generator may have over the workers: enough to keep every worker fed, and
     * enough that a role switch, which drains what is queued first, completes in reasonable time.
     * Bounding it also keeps a graceful stop prompt, since the stop drains the same queues.
     */
    gen->lead_max = WT_MAX((uint64_t)state->cfg->thread_count * GEN_LEAD_PER_THREAD,
      (uint64_t)state->cfg->switch_interval * GEN_APPLY_RATE_FLOOR);
}

/*
 * generator_switch_requested --
 *     Watch for the parent's switch request, polling the sentinel at most once a second (the
 *     cadence the control loop's own waits use).
 */
static bool
generator_switch_requested(GENERATOR *gen)
{
    struct timespec now;
    __wt_epoch(NULL, &now);
    if (WT_TIMEDIFF_SEC(now, gen->last_poll) < 1)
        return (false);
    gen->last_poll = now;
    return (node_switch_request_consume());
}

/*
 * generator_publish_pending --
 *     Publish every pending operation emitted before the given stream position.
 */
static void
generator_publish_pending(GENERATOR *gen, uint64_t position)
{
    for (uint32_t t = 0; t < gen->state->worker_count; t++)
        for (uint32_t slot = 0; slot < gen->state->cfg->pool_size; slot++) {
            TABLE *table = &gen->state->workers[t].table[slot];
            if (!generator_table_pending(table) || table->emitted_at >= position)
                continue;

            SCHEMA_EVENT ev = {0};
            table->state = generator_publish(gen, table, &ev);
            generator_emit(gen, t, slot, &ev);
        }
}

/*
 * generator_count_pending --
 *     Count the tables with a pending publish.
 */
static uint32_t
generator_count_pending(GENERATOR *gen)
{
    uint32_t count = 0;
    for (uint32_t t = 0; t < gen->state->worker_count; t++)
        for (uint32_t slot = 0; slot < gen->state->cfg->pool_size; slot++)
            if (generator_table_pending(&gen->state->workers[t].table[slot]))
                ++count;
    return (count);
}

/*
 * generator_stepdown_ended --
 *     Whether the step-down may complete: the peer adopted the step-down checkpoint or died, or a
 *     lone node emitted its share of events.
 */
static bool
generator_stepdown_ended(GENERATOR *gen)
{
    WORKLOAD_STATE *state = gen->state;

    /* Zero until the step-down checkpoint is taken. */
    const uint64_t ckpt_lsn = __wt_atomic_load_uint64(&state->stepdown_ckpt_lsn);
    const bool lone = node_is_lone(state->cfg);

    /* A dead peer cannot adopt the checkpoint. */
    if (ckpt_lsn != 0 && !lone && !node_peer_alive(state->cfg))
        return (true);

    struct timespec now;
    __wt_epoch(NULL, &now);
    if (WT_TIMEDIFF_SEC(now, gen->last_poll) < 1)
        return (false); /* Too early, come back later. */
    gen->last_poll = now;

    const uint64_t stepdown_events = gen->emitted - gen->stepdown_emitted;
    /* Lone node exhausted step-down events or peer adopted the checkpoint. */
    const bool ended = ckpt_lsn != 0 &&
      (lone ? stepdown_events >= GEN_STEPDOWN_EVENTS : adopted_ckpt_read(NULL) >= ckpt_lsn);

    if (ended)
        return (true);

    /* Report which part of the step-down stalled. */
    if (WT_TIMEDIFF_SEC(now, gen->stepdown_start) > MAX_STEPDOWN_WAIT) {
        if (ckpt_lsn == 0)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32
              ": the step-down checkpoint did not complete in %d seconds "
              "(frontier %" PRIu64 ", reserved %" PRIu64 ", step-down %" PRIu64 ")",
              state->cfg->node_id, MAX_STEPDOWN_WAIT, __wt_atomic_load_uint64(&state->frontier_ts),
              __wt_atomic_load_uint64(&state->reserved_ts),
              __wt_atomic_load_uint64(&state->stepdown_ts));
        else if (lone)
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": the step-down emitted %" PRIu64 " of %d events in %d seconds",
              state->cfg->node_id, stepdown_events, GEN_STEPDOWN_EVENTS, MAX_STEPDOWN_WAIT);
        else
            testutil_die(ETIMEDOUT,
              "Node %" PRIu32 ": peer did not adopt the step-down checkpoint (lsn %" PRIu64
              ") in %d seconds",
              state->cfg->node_id, ckpt_lsn, MAX_STEPDOWN_WAIT);
    }

    return (false);
}

/*
 * generator_transition_emit --
 *     Emit a transition event: the step-down, with the number of timestamps to reserve for the
 *     pending publishes, or the switch.
 */
static void
generator_transition_emit(GENERATOR *gen, EVENT_TYPE type, uint32_t reserve_count)
{
    SCHEMA_EVENT ev = {0};
    ev.type = type;
    ev.reserve_count = reserve_count;
    pipe_event_write(gen->state->cfg->self_pipe_write_fd, &ev);
    ++gen->emitted;
}

/*
 * generator_normal --
 *     The term's workload runs until the parent requests a switch.
 */
static GENERATOR_PHASE
generator_normal(GENERATOR *gen)
{
    GENERATOR_PHASE next_phase = GEN_NORMAL;

    /* Only a leader steps down; a lone follower goes straight to the hand-over. */
    if (generator_switch_requested(gen))
        next_phase = gen->state->leads ? GEN_BEGIN_STEPDOWN : GEN_HANDOVER;
    return (next_phase);
}

/*
 * generator_begin_stepdown --
 *     Emit the step-down event. Every publish pending now takes a timestamp reserved at or below
 *     the step-down timestamp.
 */
static GENERATOR_PHASE
generator_begin_stepdown(GENERATOR *gen)
{
    generator_transition_emit(gen, EVENT_STEPDOWN, generator_count_pending(gen));
    gen->stepdown_emitted = gen->emitted;
    __wt_epoch(NULL, &gen->stepdown_start);
    return (GEN_STEPDOWN);
}

/*
 * generator_stepdown --
 *     The step-down's limited workload runs until the step-down ends. The step-down checkpoint
 *     waits for the operations that predate the step-down to be published, so publish them once the
 *     step-down has emitted its share of events or the peer is gone.
 */
static GENERATOR_PHASE
generator_stepdown(GENERATOR *gen)
{
    GENERATOR_PHASE next_phase = GEN_STEPDOWN;

    if (generator_peer_lost(gen) || gen->emitted - gen->stepdown_emitted >= GEN_STEPDOWN_EVENTS)
        generator_publish_pending(gen, gen->stepdown_emitted);

    if (generator_stepdown_ended(gen)) {
        struct timespec now;
        __wt_epoch(NULL, &now);
        println("Node %" PRIu32 ": step-down emitted %" PRIu64 " events in %" PRIu64
                " ms (budget %d s)",
          gen->state->cfg->node_id, gen->emitted - gen->stepdown_emitted,
          (uint64_t)WT_TIMEDIFF_MS(now, gen->stepdown_start), MAX_STEPDOWN_WAIT);
        next_phase = GEN_HANDOVER;
    }
    return (next_phase);
}

/*
 * generator_handover --
 *     Publish everything still pending, then emit the switch event that ends the stream: nothing
 *     the term originated may stay unpublished past it.
 */
static GENERATOR_PHASE
generator_handover(GENERATOR *gen)
{
    generator_publish_pending(gen, UINT64_MAX);
    generator_transition_emit(gen, EVENT_SWITCH, 0);
    return (GEN_STOP);
}

/*
 * thread_generator_run --
 *     The generator thread procedure.
 */
WT_THREAD_RET
thread_generator_run(void *arg)
{
    GENERATOR gen;
    generator_init(&gen, arg);

    while (gen.phase != GEN_STOP && workload_active(gen.state, STAGE_GENERATOR)) {
        generator_round(&gen);

        switch (gen.phase) {
        case GEN_NORMAL:
            gen.phase = generator_normal(&gen);
            break;
        case GEN_BEGIN_STEPDOWN:
            gen.phase = generator_begin_stepdown(&gen);
            break;
        case GEN_STEPDOWN:
            gen.phase = generator_stepdown(&gen);
            break;
        case GEN_HANDOVER:
            gen.phase = generator_handover(&gen);
            break;
        case GEN_STOP:
            break;
        }
    }

    return (WT_THREAD_RET_VALUE);
}
