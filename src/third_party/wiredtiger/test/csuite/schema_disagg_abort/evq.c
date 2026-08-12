/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The per-worker event queues: one single-producer/single-consumer ring per worker thread. The
 * reader is the sole producer, its worker the sole consumer, so the rings need no locks - only the
 * head and tail are atomic. A full ring blocks the reader, which stops draining the source pipe and
 * so backpressures whoever is producing the stream.
 */

#include "schema_disagg_abort.h"

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
 * evq_enqueue --
 *     Queue one received schema event for its worker thread, blocking while the ring is full: the
 *     stalled reader stops draining the pipe, which backpressures the leader. Gives up when the
 *     phase is stopping.
 */
void
evq_enqueue(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev)
{
    testutil_assert(ev->thread_id < state->nth_workers);

    EVENT_QUEUE *q = &state->workers[ev->thread_id].evq;
    while (!evq_push(q, ev) && workload_active(state, STAGE_WORKERS))
        __wt_sleep(0, WT_THOUSAND);
}

/*
 * evq_dequeue --
 *     Take the next event queued for one worker; false when nothing is queued for it.
 */
bool
evq_dequeue(WORKLOAD_STATE *state, uint32_t thread_index, SCHEMA_EVENT *ev)
{
    return (evq_pop(&state->workers[thread_index].evq, ev));
}

/*
 * evq_is_empty --
 *     Report whether one worker's queue is empty.
 */
bool
evq_is_empty(WORKLOAD_STATE *state, uint32_t thread_index)
{
    return (evq_empty(&state->workers[thread_index].evq));
}

/*
 * evq_drain_barrier --
 *     Wait until every worker has applied everything queued so far. Only the reader may call it: it
 *     is the sole producer for the queues, so nothing new can arrive while it waits here. It runs
 *     this before a hand-over, so the counter it asserts against the sender's covers every event of
 *     the term.
 */
void
evq_drain_barrier(WORKLOAD_STATE *state)
{
    for (uint32_t t = 0; t < state->nth_workers; t++)
        while (
          (!evq_empty(&state->workers[t].evq) || __wt_atomic_load_bool(&state->workers[t].busy)) &&
          workload_active(state, STAGE_WORKERS))
            __wt_sleep(0, WT_THOUSAND);
}
