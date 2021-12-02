/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Inspired by "Spinlocks and Read-Write Locks" by Dr. Steven Fuerst:
 *	http://locklessinc.com/articles/locks/
 *
 * Dr. Fuerst further credits:
 *	There exists a form of the ticket lock that is designed for read-write
 * locks. An example written in assembly was posted to the Linux kernel mailing
 * list in 2002 by David Howells from RedHat. This was a highly optimized
 * version of a read-write ticket lock developed at IBM in the early 90's by
 * Joseph Seigh. Note that a similar (but not identical) algorithm was published
 * by John Mellor-Crummey and Michael Scott in their landmark paper "Scalable
 * Reader-Writer Synchronization for Shared-Memory Multiprocessors".
 *
 * The following is an explanation of our interpretation and implementation.
 * First, the underlying lock structure.
 *
 * volatile union {
 *	uint64_t v;				// Full 64-bit value
 *	struct {
 *		uint8_t current;		// Current ticket
 *		uint8_t next;			// Next available ticket
 *		uint8_t reader;			// Read queue ticket
 *		uint8_t readers_queued;		// Count of queued readers
 *		uint32_t readers_active;	// Count of active readers
 *	} s;
 * } u;
 *
 * First, imagine a store's 'take a number' ticket algorithm. A customer takes
 * a unique ticket number and customers are served in ticket order. In the data
 * structure, 'next' is the ticket that will be allocated next, and 'current'
 * is the ticket being served.
 *
 * Next, consider exclusive (write) locks.  To lock, 'take a number' and wait
 * until that number is being served; more specifically, atomically increment
 * 'next', and then wait until 'current' equals that allocated ticket.
 *
 * Shared (read) locks are similar, except that readers can share a ticket
 * (both with each other and with a single writer).  Readers with a given
 * ticket execute before the writer with that ticket.  In other words, writers
 * wait for both their ticket to become current and for all readers to exit
 * the lock.
 *
 * If there are no active writers (indicated by 'current' == 'next'), readers
 * can immediately enter the lock by atomically incrementing 'readers_active'.
 * When there are writers active, readers form a new queue by first setting
 * 'reader' to 'next' (i.e. readers are scheduled after any queued writers,
 * avoiding starvation), then atomically incrementing 'readers_queued'.
 *
 * We limit how many readers can queue: we don't allow more readers to queue
 * than there are active writers (calculated as `next - current`): otherwise,
 * in write-heavy workloads, readers can keep queuing up in front of writers
 * and throughput is unstable.  The remaining read requests wait without any
 * ordering.
 *
 * The 'next' field is a 1-byte value so the available ticket number wraps
 * after 256 requests. If a thread's write lock request would cause the 'next'
 * field to catch up with 'current', instead it waits to avoid the same ticket
 * being allocated to multiple threads.
 */

#include "wt_internal.h"

/*
 * __wt_rwlock_init --
 *     Initialize a read/write lock.
 */
int
__wt_rwlock_init(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    l->u.v = 0;
    l->stat_read_count_off = l->stat_write_count_off = -1;
    l->stat_app_usecs_off = l->stat_int_usecs_off = -1;

    WT_RET(__wt_cond_alloc(session, "rwlock wait", &l->cond_readers));
    WT_RET(__wt_cond_alloc(session, "rwlock wait", &l->cond_writers));
    return (0);
}

/*
 * __wt_rwlock_destroy --
 *     Destroy a read/write lock.
 */
void
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    l->u.v = 0;

    __wt_cond_destroy(session, &l->cond_readers);
    __wt_cond_destroy(session, &l->cond_writers);
}

/*
 * __wt_try_readlock --
 *     Try to get a shared lock, fail immediately if unavailable.
 */
int
__wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;
    int64_t **stats;

    WT_STAT_CONN_INCR(session, rwlock_read);
    if (l->stat_read_count_off != -1 && WT_STAT_ENABLED(session)) {
        stats = (int64_t **)S2C(session)->stats;
        stats[session->stat_bucket][l->stat_read_count_off]++;
    }

    old.u.v = l->u.v;

    /* This read lock can only be granted if there are no active writers. */
    if (old.u.s.current != old.u.s.next)
        return (__wt_set_return(session, EBUSY));

    /*
     * The replacement lock value is a result of adding an active reader. Check for overflow: if the
     * maximum number of readers are already active, no new readers can enter the lock.
     */
    new.u.v = old.u.v;
    if (++new.u.s.readers_active == 0)
        return (__wt_set_return(session, EBUSY));

    /* We rely on this atomic operation to provide a barrier. */
    return (__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v) ? 0 : EBUSY);
}

/*
 * __read_blocked --
 *     Check whether the current read lock request should keep waiting.
 */
static bool
__read_blocked(WT_SESSION_IMPL *session)
{
    return (session->current_rwticket != session->current_rwlock->u.s.current);
}

/*
 * __wt_readlock --
 *     Get a shared lock.
 */
void
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;
    uint64_t time_diff, time_start, time_stop;
    int64_t *session_stats, **stats;
    int16_t writers_active;
    uint8_t ticket;
    int pause_cnt;

    WT_STAT_CONN_INCR(session, rwlock_read);

    WT_DIAGNOSTIC_YIELD;

    for (;;) {
        /*
         * Fast path: if there is no active writer, join the current group.
         */
        for (old.u.v = l->u.v; old.u.s.current == old.u.s.next; old.u.v = l->u.v) {
            new.u.v = old.u.v;
            /*
             * Check for overflow: if the maximum number of readers are already active, no new
             * readers can enter the lock.
             */
            if (++new.u.s.readers_active == 0)
                goto stall;
            if (__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v))
                return;
            WT_PAUSE();
        }

        /*
         * There is an active writer: join the next group.
         *
         * Limit how many readers can queue: don't allow more readers
         * to queue than there are active writers (calculated as
         * `next - current`): otherwise, in write-heavy workloads,
         * readers can keep queuing up in front of writers and
         * throughput is unstable.
         *
         * If the maximum allowed number of readers are already queued or there is a
         * potential overflow, wait until we can get a valid ticket.
         */
        writers_active = old.u.s.next - old.u.s.current;
        if (old.u.s.readers_queued == UINT8_MAX || old.u.s.readers_queued > writers_active) {
stall:
            __wt_cond_wait(session, l->cond_readers, 10 * WT_THOUSAND, NULL);
            continue;
        }

        /*
         * If we are the first reader to queue, set the next read group. Note: don't re-read from
         * the lock or we could race with a writer unlocking.
         */
        new.u.v = old.u.v;
        if (new.u.s.readers_queued++ == 0)
            new.u.s.reader = new.u.s.next;
        ticket = new.u.s.reader;
        WT_ASSERT(session, new.u.s.readers_queued != 0);
        if (__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v))
            break;
    }

    /* Wait for our group to start. */
    time_start = l->stat_read_count_off != -1 && WT_STAT_ENABLED(session) ? __wt_clock(session) : 0;
    for (pause_cnt = 0; ticket != l->u.s.current; pause_cnt++) {
        if (pause_cnt < 1000)
            WT_PAUSE();
        else if (pause_cnt < 1200)
            __wt_yield();
        else {
            session->current_rwlock = l;
            session->current_rwticket = ticket;
            __wt_cond_wait(session, l->cond_readers, 10 * WT_THOUSAND, __read_blocked);
        }
    }
    if (time_start != 0) {
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);

        stats = (int64_t **)S2C(session)->stats;
        stats[session->stat_bucket][l->stat_read_count_off]++;
        session_stats = (int64_t *)&(session->stats);
        if (F_ISSET(session, WT_SESSION_INTERNAL))
            stats[session->stat_bucket][l->stat_int_usecs_off] += (int64_t)time_diff;
        else {
            stats[session->stat_bucket][l->stat_app_usecs_off] += (int64_t)time_diff;
        }
        session_stats[l->stat_session_usecs_off] += (int64_t)time_diff;
    }

    /*
     * Applications depend on a barrier here so that operations holding the lock see consistent
     * data. The atomic operation above isn't sufficient here because we don't own the lock until
     * our ticket comes up and whatever data we are protecting may have changed in the meantime.
     */
    WT_READ_BARRIER();

    /* Sanity check that we (still) have the lock. */
    WT_ASSERT(session, ticket == l->u.s.current && l->u.s.readers_active > 0);
}

/*
 * __wt_readunlock --
 *     Release a shared lock.
 */
void
__wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;

    do {
        old.u.v = l->u.v;
        WT_ASSERT(session, old.u.s.readers_active > 0);

        /*
         * Decrement the active reader count (other readers are doing the same, make sure we don't
         * race).
         */
        new.u.v = old.u.v;
        --new.u.s.readers_active;
    } while (!__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v));

    if (new.u.s.readers_active == 0 && new.u.s.current != new.u.s.next)
        __wt_cond_signal(session, l->cond_writers);
}

/*
 * __wt_try_writelock --
 *     Try to get an exclusive lock, fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;
    int64_t **stats;

    WT_STAT_CONN_INCR(session, rwlock_write);
    if (l->stat_write_count_off != -1 && WT_STAT_ENABLED(session)) {
        stats = (int64_t **)S2C(session)->stats;
        stats[session->stat_bucket][l->stat_write_count_off]++;
    }

    /*
     * This write lock can only be granted if no readers or writers blocked on the lock, that is, if
     * this thread's ticket would be the next ticket granted. Check if this can possibly succeed
     * (and confirm the lock is in the correct state to grant this write lock).
     */
    old.u.v = l->u.v;
    if (old.u.s.current != old.u.s.next || old.u.s.readers_active != 0)
        return (__wt_set_return(session, EBUSY));

    /*
     * We've checked above that there is no writer active (since
     * `current == next`), so there should be no readers queued.
     */
    WT_ASSERT(session, old.u.s.readers_queued == 0);

    /*
     * The replacement lock value is a result of allocating a new ticket.
     *
     * We rely on this atomic operation to provide a barrier.
     */
    new.u.v = old.u.v;
    new.u.s.next++;
    return (__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v) ? 0 : EBUSY);
}

/*
 * __write_blocked --
 *     Check whether the current write lock request should keep waiting.
 */
static bool
__write_blocked(WT_SESSION_IMPL *session)
{
    WT_RWLOCK *l;

    l = session->current_rwlock;
    return (session->current_rwticket != l->u.s.current || l->u.s.readers_active != 0);
}

/*
 * __wt_writelock --
 *     Wait to get an exclusive lock.
 */
void
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;
    uint64_t time_diff, time_start, time_stop;
    int64_t *session_stats, **stats;
    uint8_t ticket;
    int pause_cnt;

    WT_STAT_CONN_INCR(session, rwlock_write);

    for (;;) {
        old.u.v = l->u.v;

        /* Allocate a ticket. */
        new.u.v = old.u.v;
        ticket = new.u.s.next++;

        /*
         * Check for overflow: if the next ticket is allowed to catch up with the current batch, two
         * writers could be granted the lock simultaneously.
         */
        if (new.u.s.current == new.u.s.next) {
            __wt_cond_wait(session, l->cond_writers, 10 * WT_THOUSAND, NULL);
            continue;
        }
        if (__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v))
            break;
    }

    /*
     * Wait for our group to start and any readers to drain.
     *
     * We take care here to do an atomic read of the full 64-bit lock value. Otherwise, reads are
     * not guaranteed to be ordered and we could see no readers active from a different batch and
     * decide that we have the lock.
     */
    time_start =
      l->stat_write_count_off != -1 && WT_STAT_ENABLED(session) ? __wt_clock(session) : 0;
    for (pause_cnt = 0, old.u.v = l->u.v; ticket != old.u.s.current || old.u.s.readers_active != 0;
         pause_cnt++, old.u.v = l->u.v) {
        if (pause_cnt < 1000)
            WT_PAUSE();
        else if (pause_cnt < 1200)
            __wt_yield();
        else {
            session->current_rwlock = l;
            session->current_rwticket = ticket;
            __wt_cond_wait(session, l->cond_writers, 10 * WT_THOUSAND, __write_blocked);
        }
    }
    if (time_start != 0) {
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);

        stats = (int64_t **)S2C(session)->stats;
        stats[session->stat_bucket][l->stat_write_count_off]++;
        session_stats = (int64_t *)&(session->stats);
        if (F_ISSET(session, WT_SESSION_INTERNAL))
            stats[session->stat_bucket][l->stat_int_usecs_off] += (int64_t)time_diff;
        else
            stats[session->stat_bucket][l->stat_app_usecs_off] += (int64_t)time_diff;
        session_stats[l->stat_session_usecs_off] += (int64_t)time_diff;
    }

    /*
     * Applications depend on a barrier here so that operations holding the lock see consistent
     * data. The atomic operation above isn't sufficient here because we don't own the lock until
     * our ticket comes up and whatever data we are protecting may have changed in the meantime.
     */
    WT_READ_BARRIER();

    /* Sanity check that we (still) have the lock. */
    WT_ASSERT(session, ticket == l->u.s.current && l->u.s.readers_active == 0);
}

/*
 * __wt_writeunlock --
 *     Release an exclusive lock.
 */
void
__wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK new, old;

    do {
        old.u.v = l->u.v;

        /*
         * We're holding the lock exclusive, there shouldn't be any active readers.
         */
        WT_ASSERT(session, old.u.s.readers_active == 0);

        /*
         * Allow the next batch to start.
         *
         * If there are readers in the next group, swap queued readers to active: this could race
         * with new readlock requests, so we have to spin.
         */
        new.u.v = old.u.v;
        if (++new.u.s.current == new.u.s.reader) {
            new.u.s.readers_active = new.u.s.readers_queued;
            new.u.s.readers_queued = 0;
        }
    } while (!__wt_atomic_casv64(&l->u.v, old.u.v, new.u.v));

    if (new.u.s.readers_active != 0)
        __wt_cond_signal(session, l->cond_readers);
    else if (new.u.s.current != new.u.s.next)
        __wt_cond_signal(session, l->cond_writers);

    WT_DIAGNOSTIC_YIELD;
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_rwlock_islocked --
 *     Return if a read/write lock is currently locked for reading or writing.
 */
bool
__wt_rwlock_islocked(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
    WT_RWLOCK old;

    WT_UNUSED(session);

    old.u.v = l->u.v;
    return (old.u.s.current != old.u.s.next || old.u.s.readers_active != 0);
}
#endif
