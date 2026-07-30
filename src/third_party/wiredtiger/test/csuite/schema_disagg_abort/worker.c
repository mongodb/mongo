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
    case EVENT_CREATE:
    case EVENT_DROP:
        ret = fprintf(fp, "%s %" PRIu64 " %s\n", ev->type == EVENT_CREATE ? "CREATE" : "DROP",
          ev->event_ts, ev->uri);
        break;
    case EVENT_INSERT:
        ret = fprintf(fp, "INSERT %" PRIu64 " %" PRIu32 " %" PRIu32 " %s\n", ev->event_ts,
          ev->key_min, ev->key_max, ev->uri);
        break;
    case EVENT_CKPT:
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
 * schema_op_execute --
 *     Execute one schema operation: the single call site for creating and dropping the test's
 *     tables, on either role. EBUSY is retried (the stream cannot be reordered, and when the source
 *     is the peer the operation already succeeded there), with a bound so a wedged operation fails
 *     the test instead of hanging it.
 */
static void
schema_op_execute(WT_SESSION *session, const SCHEMA_EVENT *ev)
{
    const bool is_create = ev->type == EVENT_CREATE;
    testutil_assert(ev->type == EVENT_CREATE || ev->type == EVENT_DROP);

    struct timespec start;
    __wt_epoch(NULL, &start);

    int ret;
    for (;;) {
        ret = is_create ? session->create(session, ev->uri, SCHEMA_TABLE_CONFIG) :
                          session->drop(session, ev->uri, "force=false,lock_wait=false");
        if (ret != EBUSY)
            break;

        struct timespec now;
        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, start) > MAX_STARTUP)
            testutil_die(ETIMEDOUT, "%s %s: EBUSY for %d seconds", is_create ? "CREATE" : "DROP",
              ev->uri, MAX_STARTUP);
        __wt_yield();
    }
    testutil_assertfmt(ret == 0, "%s %s (ts %" PRIu64 "): %s", is_create ? "CREATE" : "DROP",
      ev->uri, ev->event_ts, wiredtiger_strerror(ret));
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
 * schema_op_insert_data --
 *     Populate a table with rows keyed key_min..key_max at the given commit timestamp; each row is
 *     valued with the commit timestamp, so the verifier can tell which generation of a reused table
 *     name wrote the data.
 */
static void
schema_op_insert_data(
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
 *     Mark one allocator value fully completed by a worker: track it in the counter (a no-op for
 *     freshly allocated values) and publish it as the thread's completed frontier mark.
 */
static void
worker_complete(WORKLOAD_STATE *state, uint32_t thread_index, uint64_t value)
{
    workload_counter_advance(state, value);
    (void)__wt_atomic_add_uint64(&state->applied, 1);
    __wt_atomic_store_uint64(&state->workers[thread_index].completed_ts, value);
}

/*
 * apply_event --
 *     Apply one event on this node, identically for both roles and exactly as the source stream
 *     fixed it - same operation, same epoch, same commit timestamp: execute the schema operation or
 *     the insert, record the event, publish a schema operation, relay it to the peer when leading,
 *     and mark it completed.
 *
 * The ordering is load-bearing. A schema operation is recorded before it is published, so the
 *     record reaches the file before a checkpoint can make the epoch durable (a record without a
 *     durable epoch is ignored by the verifier, the reverse would be a hole). The relay precedes
 *     the completion store, which is what lets the stable frontier advance past this operation,
 *     what lets a checkpoint cover it, and what lets the checkpoint thread send that checkpoint's
 *     pipe event: the peer holds every event at or below a checkpoint's stable frontier by the time
 *     it sees that checkpoint's event.
 */
static void
apply_event(WORKLOAD_STATE *state, WORKER_CTX *ctx, uint32_t thread_index, const SCHEMA_EVENT *ev)
{
    const bool relay = state->leads;

    if (ctx->record_fp == NULL)
        ctx->record_fp = worker_record_open(state, thread_index);

    switch (ev->type) {
    case EVENT_INSERT:
        schema_op_insert_data(ctx->session, ev->uri, ev->event_ts, ev->key_min, ev->key_max);
        record_event_line(ctx->record_fp, ev);
        if (relay)
            (void)node_event_send(state->cfg, ev);
        worker_complete(state, thread_index, ev->event_ts);
        break;
    case EVENT_CREATE:
    case EVENT_DROP:
        schema_op_execute(ctx->session, ev);
        record_event_line(ctx->record_fp, ev);
        schema_op_publish(ctx->session, ev->uri, ev->event_ts);
        if (relay)
            (void)node_event_send(state->cfg, ev);
        worker_complete(state, thread_index, ev->event_ts);
        break;
    case EVENT_CKPT:
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

    while (workload_running(state) || !workload_queue_empty(state, thread_index)) {
        /* Publish busy before checking the queue so the drain barrier never races an apply. */
        __wt_atomic_store_bool(busyp, true);
        SCHEMA_EVENT ev;
        const bool popped = workload_dequeue(state, thread_index, &ev);
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

/* Thread handles have process lifetime; phases join and restart them but never free them. */
static wt_thread_t worker_thr[MAX_TH];
static THREAD_ARG worker_arg[MAX_TH];

/*
 * node_workers_start --
 *     Start this phase's worker threads.
 */
void
node_workers_start(WORKLOAD_STATE *state)
{
    for (uint32_t i = 0; i < state->nth_workers; i++) {
        worker_arg[i].state = state;
        worker_arg[i].thread_index = i;
        testutil_check(__wt_thread_create(NULL, &worker_thr[i], thread_worker_run, &worker_arg[i]));
    }
}

/*
 * node_workers_join --
 *     Join the worker threads. The engine has already directed the phase to quiesce, which the
 *     workers answer by draining whatever the reader delivered before exiting.
 */
void
node_workers_join(WORKLOAD_STATE *state)
{
    for (uint32_t i = 0; i < state->nth_workers; i++)
        testutil_check(__wt_thread_join(NULL, &worker_thr[i]));
}
