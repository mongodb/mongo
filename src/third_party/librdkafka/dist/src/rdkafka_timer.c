/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdkafka_int.h"
#include "rd.h"
#include "rdtime.h"
#include "rdrand.h"
#include "rdsysqueue.h"

#include "rdkafka_queue.h"

static RD_INLINE void rd_kafka_timers_lock(rd_kafka_timers_t *rkts) {
        mtx_lock(&rkts->rkts_lock);
}

static RD_INLINE void rd_kafka_timers_unlock(rd_kafka_timers_t *rkts) {
        mtx_unlock(&rkts->rkts_lock);
}


static RD_INLINE int rd_kafka_timer_started(const rd_kafka_timer_t *rtmr) {
        return rtmr->rtmr_interval ? 1 : 0;
}


static RD_INLINE int rd_kafka_timer_scheduled(const rd_kafka_timer_t *rtmr) {
        return rtmr->rtmr_next ? 1 : 0;
}


static int rd_kafka_timer_cmp(const void *_a, const void *_b) {
        const rd_kafka_timer_t *a = _a, *b = _b;
        return RD_CMP(a->rtmr_next, b->rtmr_next);
}

static void rd_kafka_timer_unschedule(rd_kafka_timers_t *rkts,
                                      rd_kafka_timer_t *rtmr) {
        TAILQ_REMOVE(&rkts->rkts_timers, rtmr, rtmr_link);
        rtmr->rtmr_next = 0;
}


/**
 * @brief Schedule the next firing of the timer at \p abs_time.
 *
 * @remark Will not update rtmr_interval, only rtmr_next.
 *
 * @locks_required timers_lock()
 */
static void rd_kafka_timer_schedule_next(rd_kafka_timers_t *rkts,
                                         rd_kafka_timer_t *rtmr,
                                         rd_ts_t abs_time) {
        rd_kafka_timer_t *first;

        rtmr->rtmr_next = abs_time;

        if (!(first = TAILQ_FIRST(&rkts->rkts_timers)) ||
            first->rtmr_next > rtmr->rtmr_next) {
                TAILQ_INSERT_HEAD(&rkts->rkts_timers, rtmr, rtmr_link);
                cnd_signal(&rkts->rkts_cond);
                if (rkts->rkts_wakeq)
                        rd_kafka_q_yield(rkts->rkts_wakeq);
        } else
                TAILQ_INSERT_SORTED(&rkts->rkts_timers, rtmr,
                                    rd_kafka_timer_t *, rtmr_link,
                                    rd_kafka_timer_cmp);
}


/**
 * @brief Schedule the next firing of the timer according to the timer's
 *        interval plus an optional \p extra_us.
 *
 * @locks_required timers_lock()
 */
static void rd_kafka_timer_schedule(rd_kafka_timers_t *rkts,
                                    rd_kafka_timer_t *rtmr,
                                    int extra_us) {

        /* Timer has been stopped */
        if (!rtmr->rtmr_interval)
                return;

        /* Timers framework is terminating */
        if (unlikely(!rkts->rkts_enabled))
                return;

        rd_kafka_timer_schedule_next(
            rkts, rtmr, rd_clock() + rtmr->rtmr_interval + extra_us);
}

/**
 * @brief Stop a timer that may be started.
 *        If called from inside a timer callback 'lock' must be 0, else 1.
 *
 * @returns 1 if the timer was started (before being stopped), else 0.
 */
int rd_kafka_timer_stop(rd_kafka_timers_t *rkts,
                        rd_kafka_timer_t *rtmr,
                        int lock) {
        if (lock)
                rd_kafka_timers_lock(rkts);

        if (!rd_kafka_timer_started(rtmr)) {
                if (lock)
                        rd_kafka_timers_unlock(rkts);
                return 0;
        }

        if (rd_kafka_timer_scheduled(rtmr))
                rd_kafka_timer_unschedule(rkts, rtmr);

        rtmr->rtmr_interval = 0;

        if (lock)
                rd_kafka_timers_unlock(rkts);

        return 1;
}


/**
 * @returns true if timer is started, else false.
 */
rd_bool_t rd_kafka_timer_is_started(rd_kafka_timers_t *rkts,
                                    const rd_kafka_timer_t *rtmr) {
        rd_bool_t ret;
        rd_kafka_timers_lock(rkts);
        ret = rtmr->rtmr_interval != 0;
        rd_kafka_timers_unlock(rkts);
        return ret;
}


/**
 * @brief Start the provided timer with the given interval.
 *
 * Upon expiration of the interval (us) the callback will be called in the
 * main rdkafka thread, after callback return the timer will be restarted.
 *
 * @param oneshot just fire the timer once.
 * @param restart if timer is already started, restart it.
 *
 * Use rd_kafka_timer_stop() to stop a timer.
 */
void rd_kafka_timer_start0(rd_kafka_timers_t *rkts,
                           rd_kafka_timer_t *rtmr,
                           rd_ts_t interval,
                           rd_bool_t oneshot,
                           rd_bool_t restart,
                           void (*callback)(rd_kafka_timers_t *rkts, void *arg),
                           void *arg) {
        rd_kafka_timers_lock(rkts);

        if (!restart && rd_kafka_timer_scheduled(rtmr)) {
                rd_kafka_timers_unlock(rkts);
                return;
        }

        rd_kafka_timer_stop(rkts, rtmr, 0 /*!lock*/);

        /* Make sure the timer interval is non-zero or the timer
         * won't be scheduled, which is not what the caller of .._start*()
         * would expect. */
        rtmr->rtmr_interval = interval == 0 ? 1 : interval;
        rtmr->rtmr_callback = callback;
        rtmr->rtmr_arg      = arg;
        rtmr->rtmr_oneshot  = oneshot;

        rd_kafka_timer_schedule(rkts, rtmr, 0);

        rd_kafka_timers_unlock(rkts);
}

/**
 * Delay the next timer invocation by '2 * rtmr->rtmr_interval'
 * @param minimum_backoff the minimum backoff to be applied
 * @param maximum_backoff the maximum backoff to be applied
 * @param max_jitter the jitter percentage to be applied to the backoff
 */
void rd_kafka_timer_exp_backoff(rd_kafka_timers_t *rkts,
                                rd_kafka_timer_t *rtmr,
                                rd_ts_t minimum_backoff,
                                rd_ts_t maximum_backoff,
                                int max_jitter) {
        int64_t jitter;
        rd_kafka_timers_lock(rkts);
        if (rd_kafka_timer_scheduled(rtmr)) {
                rd_kafka_timer_unschedule(rkts, rtmr);
        }
        rtmr->rtmr_interval *= 2;
        jitter =
            (rd_jitter(-max_jitter, max_jitter) * rtmr->rtmr_interval) / 100;
        if (rtmr->rtmr_interval + jitter < minimum_backoff) {
                rtmr->rtmr_interval = minimum_backoff;
                jitter              = 0;
        } else if ((maximum_backoff != -1) &&
                   (rtmr->rtmr_interval + jitter) > maximum_backoff) {
                rtmr->rtmr_interval = maximum_backoff;
                jitter              = 0;
        }
        rd_kafka_timer_schedule(rkts, rtmr, jitter);
        rd_kafka_timers_unlock(rkts);
}

/**
 * @brief Override the interval once for the next firing of the timer.
 *
 * @locks_required none
 * @locks_acquired timers_lock
 */
void rd_kafka_timer_override_once(rd_kafka_timers_t *rkts,
                                  rd_kafka_timer_t *rtmr,
                                  rd_ts_t interval) {
        rd_kafka_timers_lock(rkts);
        if (rd_kafka_timer_scheduled(rtmr))
                rd_kafka_timer_unschedule(rkts, rtmr);
        rd_kafka_timer_schedule_next(rkts, rtmr, rd_clock() + interval);
        rd_kafka_timers_unlock(rkts);
}


/**
 * @returns the delta time to the next time (>=0) this timer fires, or -1
 *          if timer is stopped.
 */
rd_ts_t rd_kafka_timer_next(rd_kafka_timers_t *rkts,
                            rd_kafka_timer_t *rtmr,
                            int do_lock) {
        rd_ts_t now   = rd_clock();
        rd_ts_t delta = -1;

        if (do_lock)
                rd_kafka_timers_lock(rkts);

        if (rd_kafka_timer_scheduled(rtmr)) {
                delta = rtmr->rtmr_next - now;
                if (delta < 0)
                        delta = 0;
        }

        if (do_lock)
                rd_kafka_timers_unlock(rkts);

        return delta;
}


/**
 * Interrupt rd_kafka_timers_run().
 * Used for termination.
 */
void rd_kafka_timers_interrupt(rd_kafka_timers_t *rkts) {
        rd_kafka_timers_lock(rkts);
        cnd_signal(&rkts->rkts_cond);
        rd_kafka_timers_unlock(rkts);
}


/**
 * Returns the delta time to the next timer to fire, capped by 'timeout_ms'.
 */
rd_ts_t
rd_kafka_timers_next(rd_kafka_timers_t *rkts, int timeout_us, int do_lock) {
        rd_ts_t now       = rd_clock();
        rd_ts_t sleeptime = 0;
        rd_kafka_timer_t *rtmr;

        if (do_lock)
                rd_kafka_timers_lock(rkts);

        if (likely((rtmr = TAILQ_FIRST(&rkts->rkts_timers)) != NULL)) {
                sleeptime = rtmr->rtmr_next - now;
                if (sleeptime < 0)
                        sleeptime = 0;
                else if (sleeptime > (rd_ts_t)timeout_us)
                        sleeptime = (rd_ts_t)timeout_us;
        } else
                sleeptime = (rd_ts_t)timeout_us;

        if (do_lock)
                rd_kafka_timers_unlock(rkts);

        return sleeptime;
}


/**
 * Dispatch timers.
 * Will block up to 'timeout' microseconds before returning.
 */
void rd_kafka_timers_run(rd_kafka_timers_t *rkts, int timeout_us) {
        rd_ts_t now = rd_clock();
        rd_ts_t end = now + timeout_us;

        rd_kafka_timers_lock(rkts);

        while (!rd_kafka_terminating(rkts->rkts_rk) && now <= end) {
                int64_t sleeptime;
                rd_kafka_timer_t *rtmr;

                if (timeout_us != RD_POLL_NOWAIT) {
                        sleeptime = rd_kafka_timers_next(rkts, timeout_us,
                                                         0 /*no-lock*/);

                        if (sleeptime > 0) {
                                cnd_timedwait_ms(&rkts->rkts_cond,
                                                 &rkts->rkts_lock,
                                                 (int)(sleeptime / 1000));
                        }
                }

                now = rd_clock();

                while ((rtmr = TAILQ_FIRST(&rkts->rkts_timers)) &&
                       rtmr->rtmr_next <= now) {
                        rd_bool_t oneshot;

                        rd_kafka_timer_unschedule(rkts, rtmr);

                        /* If timer must only be fired once,
                         * disable it now prior to callback.
                         *
                         * NOTE: Oneshot timers are never touched again after
                         * the callback has been called to avoid use-after-free.
                         */
                        if ((oneshot = rtmr->rtmr_oneshot))
                                rtmr->rtmr_interval = 0;

                        rd_kafka_timers_unlock(rkts);

                        rtmr->rtmr_callback(rkts, rtmr->rtmr_arg);

                        rd_kafka_timers_lock(rkts);

                        /* Restart timer, unless it has been stopped, or
                         * already reschedueld (start()ed) from callback. */
                        if (!oneshot && rd_kafka_timer_started(rtmr) &&
                            !rd_kafka_timer_scheduled(rtmr))
                                rd_kafka_timer_schedule(rkts, rtmr, 0);
                }

                if (timeout_us == RD_POLL_NOWAIT) {
                        /* Only iterate once, even if rd_clock doesn't change */
                        break;
                }
        }

        rd_kafka_timers_unlock(rkts);
}


void rd_kafka_timers_destroy(rd_kafka_timers_t *rkts) {
        rd_kafka_timer_t *rtmr;

        rd_kafka_timers_lock(rkts);
        rkts->rkts_enabled = 0;
        while ((rtmr = TAILQ_FIRST(&rkts->rkts_timers)))
                rd_kafka_timer_stop(rkts, rtmr, 0);
        rd_kafka_assert(rkts->rkts_rk, TAILQ_EMPTY(&rkts->rkts_timers));
        rd_kafka_timers_unlock(rkts);

        cnd_destroy(&rkts->rkts_cond);
        mtx_destroy(&rkts->rkts_lock);
}

void rd_kafka_timers_init(rd_kafka_timers_t *rkts,
                          rd_kafka_t *rk,
                          struct rd_kafka_q_s *wakeq) {
        memset(rkts, 0, sizeof(*rkts));
        rkts->rkts_rk = rk;
        TAILQ_INIT(&rkts->rkts_timers);
        mtx_init(&rkts->rkts_lock, mtx_plain);
        cnd_init(&rkts->rkts_cond);
        rkts->rkts_enabled = 1;
        rkts->rkts_wakeq   = wakeq;
}
