/*
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Compute the time in nanoseconds that must be reserved to represent a number of bytes in a
 * subsystem with a particular capacity per second.
 */
#define WT_RESERVATION_NS(bytes, capacity) (((bytes)*WT_BILLION) / (capacity))

/*
 * The fraction of a second's worth of capacity that will be stolen at a time. The number of bytes
 * this represents may be different for different subsystems, since each subsystem has its own
 * capacity per second.
 */
#define WT_STEAL_FRACTION(x) ((x) / 16)

/*
 * __capacity_config --
 *     Set I/O capacity configuration.
 */
static int
__capacity_config(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CAPACITY *cap;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint64_t total;

    conn = S2C(session);

    WT_RET(__wt_config_gets(session, cfg, "io_capacity.total", &cval));
    if (cval.val != 0 && cval.val < WT_THROTTLE_MIN)
        WT_RET_MSG(session, EINVAL, "total I/O capacity value %" PRId64 " below minimum %d",
          cval.val, WT_THROTTLE_MIN);

    cap = &conn->capacity;
    cap->total = total = (uint64_t)cval.val;
    if (cval.val != 0) {
        /*
         * We've been given a total capacity, set the capacity of all the subsystems.
         */
        cap->ckpt = WT_CAPACITY_SYS(total, WT_CAP_CKPT);
        cap->evict = WT_CAPACITY_SYS(total, WT_CAP_EVICT);
        cap->log = WT_CAPACITY_SYS(total, WT_CAP_LOG);
        cap->read = WT_CAPACITY_SYS(total, WT_CAP_READ);

        /*
         * Set the threshold to the percent of our capacity to periodically asynchronously flush
         * what we've written.
         */
        cap->threshold = ((cap->ckpt + cap->evict + cap->log) / 100) * WT_CAPACITY_PCT;
        if (cap->threshold < WT_CAPACITY_MIN_THRESHOLD)
            cap->threshold = WT_CAPACITY_MIN_THRESHOLD;
        WT_STAT_CONN_SET(session, capacity_threshold, cap->threshold);
    } else
        WT_STAT_CONN_SET(session, capacity_threshold, 0);

    return (0);
}

/*
 * __capacity_server_run_chk --
 *     Check to decide if the capacity server should continue running.
 */
static bool
__capacity_server_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CAPACITY));
}

/*
 * __capacity_server --
 *     The capacity server thread.
 */
static WT_THREAD_RET
__capacity_server(void *arg)
{
    WT_CAPACITY *cap;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t start, stop, time_ms;

    session = arg;
    conn = S2C(session);
    cap = &conn->capacity;
    for (;;) {
        /*
         * Wait until signalled but check once per second in case the signal was missed.
         */
        __wt_cond_wait(session, conn->capacity_cond, WT_MILLION, __capacity_server_run_chk);

        /* Check if we're quitting or being reconfigured. */
        if (!__capacity_server_run_chk(session))
            break;

        cap->signalled = false;
        if (cap->written < cap->threshold)
            continue;

        start = __wt_clock(session);
        WT_ERR(__wt_fsync_background(session));
        stop = __wt_clock(session);
        time_ms = WT_CLOCKDIFF_MS(stop, start);
        WT_STAT_CONN_SET(session, fsync_all_time, time_ms);
        cap->written = 0;
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "capacity server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __capacity_server_start --
 *     Start the capacity server thread.
 */
static int
__capacity_server_start(WT_CONNECTION_IMPL *conn)
{
    WT_SESSION_IMPL *session;

    FLD_SET(conn->server_flags, WT_CONN_SERVER_CAPACITY);

    /*
     * The capacity server gets its own session.
     */
    WT_RET(
      __wt_open_internal_session(conn, "capacity-server", false, 0, 0, &conn->capacity_session));
    session = conn->capacity_session;

    WT_RET(__wt_cond_alloc(session, "capacity server", &conn->capacity_cond));

    /*
     * Start the thread.
     */
    WT_RET(__wt_thread_create(session, &conn->capacity_tid, __capacity_server, session));
    conn->capacity_tid_set = true;

    return (0);
}

/*
 * __wt_capacity_server_create --
 *     Configure and start the capacity server.
 */
int
__wt_capacity_server_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /*
     * Stop any server that is already running. This means that each time reconfigure is called
     * we'll bounce the server even if there are no configuration changes. This makes our life
     * easier as the underlying configuration routine doesn't have to worry about freeing objects in
     * the connection structure (it's guaranteed to always start with a blank slate), and we don't
     * have to worry about races where a running server is reading configuration information that
     * we're updating, and it's not expected that reconfiguration will happen a lot.
     */
    if (conn->capacity_session != NULL)
        WT_RET(__wt_capacity_server_destroy(session));
    WT_RET(__capacity_config(session, cfg));

    /*
     * If it is a read only connection or if background fsync is not supported, then there is
     * nothing to do.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY) || !__wt_fsync_background_chk(session))
        return (0);

    if (conn->capacity.total != 0)
        WT_RET(__capacity_server_start(conn));

    return (0);
}

/*
 * __wt_capacity_server_destroy --
 *     Destroy the capacity server thread.
 */
int
__wt_capacity_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CAPACITY);
    if (conn->capacity_tid_set) {
        __wt_cond_signal(session, conn->capacity_cond);
        WT_TRET(__wt_thread_join(session, &conn->capacity_tid));
        conn->capacity_tid_set = false;
    }
    __wt_cond_destroy(session, &conn->capacity_cond);

    /* Close the server thread's session. */
    if (conn->capacity_session != NULL)
        WT_TRET(__wt_session_close_internal(conn->capacity_session));

    /*
     * Ensure capacity settings are cleared - so that reconfigure doesn't get confused.
     */
    conn->capacity_session = NULL;
    conn->capacity_tid_set = false;
    conn->capacity_cond = NULL;

    return (ret);
}

/*
 * __capacity_signal --
 *     Signal the capacity thread if sufficient data has been written.
 */
static void
__capacity_signal(WT_SESSION_IMPL *session)
{
    WT_CAPACITY *cap;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    cap = &conn->capacity;
    if (cap->written >= cap->threshold && !cap->signalled) {
        __wt_cond_signal(session, conn->capacity_cond);
        cap->signalled = true;
    }
}

/*
 * __capacity_reserve --
 *     Make a reservation for the given number of bytes against the capacity of the subsystem.
 */
static void
__capacity_reserve(
  uint64_t *reservation, uint64_t bytes, uint64_t capacity, uint64_t now_ns, uint64_t *result)
{
    uint64_t res_len, res_value;

    if (capacity != 0) {
        res_len = WT_RESERVATION_NS(bytes, capacity);
        res_value = __wt_atomic_add64(reservation, res_len);
        if (now_ns > res_value && now_ns - res_value > WT_BILLION)
            /*
             * If the reservation clock is out of date, bring it to within a second of a current
             * time.
             */
            *reservation = (now_ns - WT_BILLION) + res_len;
    } else
        res_value = now_ns;

    *result = res_value;
}

/*
 * __wt_capacity_throttle --
 *     Reserve a time to perform a write operation for the subsystem, and wait until that time. The
 *     concept is that each write to a subsystem reserves a time slot to do its write, and
 *     atomically adjusts the reservation marker to point past the reserved slot. The size of the
 *     adjustment (i.e. the length of time represented by the slot in nanoseconds) is chosen to be
 *     proportional to the number of bytes to be written, and the proportion is a simple calculation
 *     so that we can fit reservations for exactly the configured capacity in a second. Reservation
 *     times are in nanoseconds since the epoch.
 */
void
__wt_capacity_throttle(WT_SESSION_IMPL *session, uint64_t bytes, WT_THROTTLE_TYPE type)
{
    struct timespec now;
    WT_CAPACITY *cap;
    WT_CONNECTION_IMPL *conn;
    uint64_t best_res, capacity, new_res, now_ns, sleep_us, res_total_value;
    uint64_t res_value, steal_capacity, stolen_bytes, this_res;
    uint64_t *reservation, *steal;
    uint64_t total_capacity;

    conn = S2C(session);
    cap = &conn->capacity;
    /* NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores) */
    capacity = steal_capacity = 0;
    reservation = steal = NULL;
    switch (type) {
    case WT_THROTTLE_CKPT:
        capacity = cap->ckpt;
        reservation = &cap->reservation_ckpt;
        WT_STAT_CONN_INCRV(session, capacity_bytes_ckpt, bytes);
        WT_STAT_CONN_INCRV(session, capacity_bytes_written, bytes);
        break;
    case WT_THROTTLE_EVICT:
        capacity = cap->evict;
        reservation = &cap->reservation_evict;
        WT_STAT_CONN_INCRV(session, capacity_bytes_evict, bytes);
        WT_STAT_CONN_INCRV(session, capacity_bytes_written, bytes);
        break;
    case WT_THROTTLE_LOG:
        capacity = cap->log;
        reservation = &cap->reservation_log;
        WT_STAT_CONN_INCRV(session, capacity_bytes_log, bytes);
        WT_STAT_CONN_INCRV(session, capacity_bytes_written, bytes);
        break;
    case WT_THROTTLE_READ:
        capacity = cap->read;
        reservation = &cap->reservation_read;
        WT_STAT_CONN_INCRV(session, capacity_bytes_read, bytes);
        break;
    }
    total_capacity = cap->total;

    /*
     * Right now no subsystem can be individually turned off, but it is certainly a possibility to
     * consider one subsystem may be turned off at some point in the future. If this subsystem is
     * not throttled there's nothing to do.
     */
    if (cap->total == 0 || capacity == 0 || F_ISSET(conn, WT_CONN_RECOVERING))
        return;

    /*
     * There may in fact be some reads done under the umbrella of log I/O, but they are mostly done
     * under recovery. And if we are recovering, we don't reach this code.
     */
    if (type != WT_THROTTLE_READ) {
        (void)__wt_atomic_addv64(&cap->written, bytes);
        __capacity_signal(session);
    }

    /* If we get sizes larger than this, later calculations may overflow. */
    WT_ASSERT(session, bytes < 16 * (uint64_t)WT_GIGABYTE);
    WT_ASSERT(session, capacity != 0);

    /* Get the current time in nanoseconds since the epoch. */
    __wt_epoch(session, &now);
    now_ns = (uint64_t)now.tv_sec * WT_BILLION + (uint64_t)now.tv_nsec;

again:
    /* Take a reservation for the subsystem, and for the total */
    __capacity_reserve(reservation, bytes, capacity, now_ns, &res_value);
    __capacity_reserve(&cap->reservation_total, bytes, total_capacity, now_ns, &res_total_value);

    /*
     * If we ended up with a future reservation, and we aren't constricted by the total capacity,
     * then we may be able to reallocate some unused reservation time from another subsystem.
     */
    if (res_value > now_ns && res_total_value < now_ns && steal == NULL && total_capacity != 0) {
        best_res = now_ns - WT_BILLION / 2;
        if (type != WT_THROTTLE_CKPT && (this_res = cap->reservation_ckpt) < best_res) {
            steal = &cap->reservation_ckpt;
            steal_capacity = cap->ckpt;
            best_res = this_res;
        }
        if (type != WT_THROTTLE_EVICT && (this_res = cap->reservation_evict) < best_res) {
            steal = &cap->reservation_evict;
            steal_capacity = cap->evict;
            best_res = this_res;
        }
        if (type != WT_THROTTLE_LOG && (this_res = cap->reservation_log) < best_res) {
            steal = &cap->reservation_log;
            steal_capacity = cap->log;
            best_res = this_res;
        }
        if (type != WT_THROTTLE_READ && (this_res = cap->reservation_read) < best_res) {
            steal = &cap->reservation_read;
            steal_capacity = cap->read;
            best_res = this_res;
        }

        if (steal != NULL) {
            /*
             * We have a subsystem that has enough spare capacity to steal. We'll take a small slice
             * (a fraction of a second worth) and add it to our own subsystem.
             */
            if (best_res < now_ns - WT_BILLION && now_ns > WT_BILLION)
                new_res = now_ns - WT_BILLION;
            else
                new_res = best_res;
            WT_ASSERT(session, steal_capacity != 0);
            new_res += WT_STEAL_FRACTION(WT_BILLION) + WT_RESERVATION_NS(bytes, steal_capacity);
            if (!__wt_atomic_casv64(steal, best_res, new_res)) {
                /*
                 * Give up our reservations and try again. We won't try to steal the next time.
                 */
                (void)__wt_atomic_sub64(reservation, WT_RESERVATION_NS(bytes, capacity));
                (void)__wt_atomic_sub64(
                  &cap->reservation_total, WT_RESERVATION_NS(bytes, total_capacity));
                goto again;
            }

            /*
             * We've stolen a fraction of a second of capacity. Figure out how many bytes that is,
             * before adding that many bytes to the acquiring subsystem's capacity.
             */
            stolen_bytes = WT_STEAL_FRACTION(steal_capacity);
            res_value = __wt_atomic_sub64(reservation, WT_RESERVATION_NS(stolen_bytes, capacity));
        }
    }
    if (res_value < res_total_value)
        res_value = res_total_value;

    if (res_value > now_ns) {
        sleep_us = (res_value - now_ns) / WT_THOUSAND;
        if (res_value == res_total_value)
            WT_STAT_CONN_INCRV(session, capacity_time_total, sleep_us);
        else
            switch (type) {
            case WT_THROTTLE_CKPT:
                WT_STAT_CONN_INCRV(session, capacity_time_ckpt, sleep_us);
                break;
            case WT_THROTTLE_EVICT:
                WT_STAT_CONN_INCRV(session, capacity_time_evict, sleep_us);
                break;
            case WT_THROTTLE_LOG:
                WT_STAT_CONN_INCRV(session, capacity_time_log, sleep_us);
                break;
            case WT_THROTTLE_READ:
                WT_STAT_CONN_INCRV(session, capacity_time_read, sleep_us);
                break;
            }
        if (sleep_us > WT_CAPACITY_SLEEP_CUTOFF_US)
            /* Sleep handles large usec values. */
            __wt_sleep(0, sleep_us);
    }
}
