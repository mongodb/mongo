/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The worker stage: N threads applying what the reader queued, exactly as the source stream fixed
 * each event, so both roles execute an identical history. Writes the verifier's record files, and
 * reports each completed operation as the thread's frontier mark.
 */

#include "schema_disagg_abort.h"

/* The table configuration every schema table is created with, on either role. */
#define SCHEMA_TABLE_CONFIG "key_format=S,value_format=S,type=layered,block_manager=disagg"

/* Thread argument: the shared workload state plus this worker's identity. */
typedef struct {
    WORKLOAD_STATE *state;
    uint32_t thread_index;
} THREAD_ARG;

/* Per-thread worker state for one phase. */
typedef struct {
    WT_SESSION *session;
    FILE *record_fp; /* records for what this node originated, or for what it applied */
} WORKER_CTX;

/*
 * record_event_line --
 *     Append one event to a record file; the one place that defines the record format for both the
 *     schema and the relay files.
 */
static void
record_event_line(FILE *fp, const SCHEMA_EVENT *ev)
{
    int ret = 0;

    switch (ev->type) {
    case EVENT_PUBLISH_CREATE:
    case EVENT_PUBLISH_DROP:
        /* A schema operation is recorded when its publish fixes the epoch, not when it ran. */
        ret = fprintf(fp, "%s %" PRIu64 " %s\n",
          ev->type == EVENT_PUBLISH_CREATE ? "CREATE" : "DROP", ev->event_ts, ev->uri);
        break;
    case EVENT_INSERT:
        ret = fprintf(fp, "INSERT %" PRIu64 " %" PRIu32 " %" PRIu32 " %s\n", ev->event_ts,
          ev->key_min, ev->key_max, ev->uri);
        break;
    case EVENT_NONE:
    case EVENT_CREATE:
    case EVENT_DROP:
    case EVENT_STEPDOWN:
    case EVENT_SWITCH:
        testutil_assertfmt(false, "Unexpected record event type: %d", ev->type);
    }
    if (ret < 0)
        testutil_die(EIO, "fprintf event record");
}

/*
 * worker_record_open --
 *     Open a worker's record file, named for the origin of what it logs: the operations this node
 *     produced itself go to its leader records, the peer's relayed events to its follower records.
 *     Append so a later phase preserves the earlier records for the post-crash verifier.
 */
static FILE *
worker_record_open(const WORKLOAD_STATE *state, uint32_t thread_index)
{
    char fname[128];
    testutil_snprintf(fname, sizeof(fname),
      state->generates ? LEADER_RECORDS_FILE : FOLLOWER_RECORDS_FILE, state->cfg->node_id,
      thread_index);

    FILE *fp;
    testutil_assert_errno((fp = fopen(fname, "a")) != NULL);
    /* Flush the record file per line so entries survive a SIGKILL crash. */
    __wt_stream_set_line_buffer(fp);
    return (fp);
}

/*
 * schema_op_stall_report --
 *     Report the frontier state a stalled schema operation is waiting on.
 */
static void
schema_op_stall_report(WORKLOAD_STATE *state)
{
    println("Node %" PRIu32 ": stable %" PRIu64 ", frontier %" PRIu64, state->cfg->node_id,
      query_ts(state->conn, TS_STABLE), __wt_atomic_load_uint64(&state->frontier_ts));
    for (uint32_t t = 0; t < state->nth_workers; t++)
        println("  worker %" PRIu32 ": %" PRIu64 " events queued", t, evq_depth(state, t));
}

/*
 * schema_op_execute --
 *     Execute one schema operation: create or drop the test's tables, on either role.
 */
static void
schema_op_execute(WORKLOAD_STATE *state, WT_SESSION *session, const SCHEMA_EVENT *ev)
{
    const bool is_create = ev->type == EVENT_CREATE;
    testutil_assert(ev->type == EVENT_CREATE || ev->type == EVENT_DROP);

    struct timespec start;
    __wt_epoch(NULL, &start);

    int ret;
    for (;;) {
        ret = is_create ? session->create(session, ev->uri, SCHEMA_TABLE_CONFIG) :
                          session->drop(session, ev->uri, "force=false,lock_wait=true");
        /*
         * When dropping tables with uncheckpointed data, EBUSY is expected. Checkpoint thread keeps
         * taking checkpoints and will eventually unblock the schema operation.
         */
        if (ret != EBUSY)
            break;

        struct timespec now;
        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, start) > MAX_OP_WAIT) {
            int err, sub_err;
            const char *err_msg;
            session->get_last_error(session, &err, &sub_err, &err_msg);
            schema_op_stall_report(state);
            testutil_die(ETIMEDOUT, "node%" PRIu32 " %s %s %s: EBUSY for %d seconds: %s",
              state->cfg->node_id, state->leads ? "leader" : "follower",
              is_create ? "CREATE" : "DROP", ev->uri, MAX_OP_WAIT, err_msg);
        }

        /*
         * Back off rather than spin: the checkpoint that clears this needs the locks a retry takes.
         */
        __wt_sleep(0, 10 * WT_THOUSAND);
    }

    /* The checkpoint pick-up may have already applied this drop, so a missing table is success. */
    if (!is_create && ret == ENOENT)
        ret = 0;

    testutil_assertfmt(ret == 0, "node%" PRIu32 " %s %s: %s", state->cfg->node_id,
      is_create ? "CREATE" : "DROP", ev->uri, wiredtiger_strerror(ret));
}

/*
 * schema_op_publish --
 *     Publish the schema operation at the given epoch so it becomes visible in shared metadata at
 *     the next checkpoint. Runs on both roles: a follower's applied operations queue up and drain
 *     when it eventually leads.
 */
static void
schema_op_publish(WT_SESSION *session, const char *uri, uint64_t epoch)
{
    char pub_cfg[64];
    testutil_snprintf(pub_cfg, sizeof(pub_cfg), "disaggregated=(schema_epoch=%" PRIx64 ")", epoch);
    testutil_check(session->publish(session, uri, pub_cfg));
}

/*
 * insert_data --
 *     Populate a table with rows keyed key_min..key_max at the given commit timestamp; each row is
 *     set to the commit timestamp, so the verifier can tell which generation of a reused table name
 *     wrote the data.
 */
static void
insert_data(
  WT_SESSION *session, const char *uri, uint64_t commit_ts, uint32_t key_min, uint32_t key_max)
{
    char val_buf[32];
    testutil_snprintf(val_buf, sizeof(val_buf), "%" PRIu64, commit_ts);
    testutil_check(session->begin_transaction(session, NULL));

    WT_CURSOR *cursor;
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (uint32_t r = key_min; r <= key_max; r++) {
        char key_buf[16];
        testutil_snprintf(key_buf, sizeof(key_buf), "%" PRIu32, r);
        cursor->set_key(cursor, key_buf);
        cursor->set_value(cursor, val_buf);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    char commit_cfg[64];
    testutil_snprintf(commit_cfg, sizeof(commit_cfg), "commit_timestamp=%" PRIx64, commit_ts);
    testutil_check(session->commit_transaction(session, commit_cfg));
}

/*
 * worker_complete --
 *     Mark one timestamp fully completed by a worker: adopt it and mark it in the completion
 *     window.
 */
static void
worker_complete(WORKLOAD_STATE *state, uint64_t value)
{
    workload_counter_advance(state, value);
    (void)__wt_atomic_add_uint64(&state->applied, 1);

    /* One writer per timestamp, so the mark needs no read-modify-write. */
    const uint64_t frontier_ts = __wt_atomic_load_uint64(&state->frontier_ts);
    testutil_assertfmt(value > frontier_ts && value - frontier_ts < FRONTIER_WINDOW,
      "completed timestamp %" PRIu64 " outside the window above the frontier %" PRIu64, value,
      frontier_ts);
    __wt_atomic_store_uint8(&state->completed_ts[value % FRONTIER_WINDOW], 1);
}

/*
 * apply_event --
 *     Apply one event on this node, identically for both roles and exactly as the source stream
 *     fixed it: execute it, record it, relay it to the peer when leading, and mark its value
 *     completed. A schema operation carries no timestamp - its epoch, record and completion belong
 *     to its later publish event.
 *
 * The order within an event is load-bearing (README invariant 1): relay before record, so a record
 *     on disk implies the peer holds the event; record before publish, so no checkpoint can make an
 *     unrecorded epoch durable; relay before the completion store, so the stable frontier only ever
 *     covers already-relayed events.
 */
static void
apply_event(WORKLOAD_STATE *state, WORKER_CTX *ctx, uint32_t thread_index, const SCHEMA_EVENT *ev)
{
    /* The role is fixed for the phase, step-down window included: a leader relays throughout. */
    const bool relay = state->leads;

    if (ctx->record_fp == NULL)
        ctx->record_fp = worker_record_open(state, thread_index);

    switch (ev->type) {
    case EVENT_INSERT:
        insert_data(ctx->session, ev->uri, ev->event_ts, ev->key_min, ev->key_max);
        if (relay)
            (void)pipe_relay_event(state->cfg, ev);
        record_event_line(ctx->record_fp, ev);
        worker_complete(state, ev->event_ts);
        break;
    case EVENT_CREATE:
    case EVENT_DROP:
        schema_op_execute(state, ctx->session, ev);
        if (relay)
            (void)pipe_relay_event(state->cfg, ev);
        /* No timestamp: count it applied, but the completion belongs to the publish. */
        (void)__wt_atomic_add_uint64(&state->applied, 1);
        break;
    case EVENT_PUBLISH_CREATE:
    case EVENT_PUBLISH_DROP:
        if (relay)
            (void)pipe_relay_event(state->cfg, ev);
        record_event_line(ctx->record_fp, ev);
        schema_op_publish(ctx->session, ev->uri, ev->event_ts);
        worker_complete(state, ev->event_ts);
        break;
    case EVENT_NONE:
    case EVENT_STEPDOWN:
    case EVENT_SWITCH:
        testutil_assertfmt(false, "Unexpected apply event type: %d", ev->type);
    }
}

/*
 * worker_apply_loop --
 *     A worker's phase, identical in both roles: execute whatever the reader queued while the phase
 *     runs, then drain the queue so a graceful stop loses nothing.
 */
static void
worker_apply_loop(WORKLOAD_STATE *state, WORKER_CTX *ctx, uint32_t thread_index)
{
    bool *busyp = &state->workers[thread_index].busy;

    while (workload_active(state, STAGE_WORKERS) || !evq_is_empty(state, thread_index)) {
        /* Publish busy before checking the queue so the drain barrier never races an apply. */
        __wt_atomic_store_bool(busyp, true);
        SCHEMA_EVENT ev;
        const bool popped = evq_dequeue(state, thread_index, &ev);
        if (popped)
            apply_event(state, ctx, thread_index, &ev);
        __wt_atomic_store_bool(busyp, false);
        if (!popped)
            __wt_sleep(0, WT_THOUSAND);
    }
}

/*
 * thread_worker_run --
 *     One worker thread: set up the per-phase context, run the processing loop, tear the context
 *     down.
 */
static WT_THREAD_RET
thread_worker_run(void *arg)
{
    const THREAD_ARG *ta = arg;
    WORKLOAD_STATE *state = ta->state;

    WORKER_CTX ctx;
    WT_CLEAR(ctx);
    testutil_check(state->conn->open_session(state->conn, NULL, NULL, &ctx.session));

    worker_apply_loop(state, &ctx, ta->thread_index);

    if (ctx.record_fp != NULL)
        testutil_check(fclose(ctx.record_fp));
    testutil_check(ctx.session->close(ctx.session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * node_workers_start --
 *     Start this phase's worker threads.
 */
void
node_workers_start(WORKLOAD_STATE *state)
{
    /* One argument per worker, alive for as long as its thread; the handles live in the state. */
    static THREAD_ARG worker_arg[MAX_TH];

    for (uint32_t i = 0; i < state->nth_workers; i++) {
        worker_arg[i].state = state;
        worker_arg[i].thread_index = i;
        testutil_check(
          __wt_thread_create(NULL, &state->workers[i].thr, thread_worker_run, &worker_arg[i]));
    }
}

/*
 * node_workers_stop --
 *     Stop the worker stage. The workers complete the stage by draining whatever the reader
 *     delivered before they exit.
 */
void
node_workers_stop(WORKLOAD_STATE *state)
{
    /* Previous stage must have stopped. */
    testutil_assert(node_stage_stopped(state, STAGE_WORKERS - 1));

    /* Stop the current stage. */
    __wt_atomic_store_uint32(&state->stop_stage, STAGE_WORKERS);
    for (uint32_t i = 0; i < state->nth_workers; i++)
        testutil_check(__wt_thread_join(NULL, &state->workers[i].thr));
}
