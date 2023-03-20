/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2013, Magnus Edenhill
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

#ifndef _RDKAFKA_TIMER_H_
#define _RDKAFKA_TIMER_H_

#include "rd.h"

struct rd_kafka_q_s; /**< Forward decl */

/* A timer engine. */
typedef struct rd_kafka_timers_s {

        TAILQ_HEAD(, rd_kafka_timer_s) rkts_timers;

        struct rd_kafka_s *rkts_rk;

        mtx_t rkts_lock;
        cnd_t rkts_cond;

        /** Optional wake-up (q_yield()) to wake up when a new timer
         *  is scheduled that will fire prior to any existing timers.
         *  This is used to wake up blocking IO or queue polls that run
         *  in the same loop as timers_run(). */
        struct rd_kafka_q_s *rkts_wakeq;

        int rkts_enabled;
} rd_kafka_timers_t;


typedef struct rd_kafka_timer_s {
        TAILQ_ENTRY(rd_kafka_timer_s) rtmr_link;

        rd_ts_t rtmr_next;
        rd_ts_t rtmr_interval;  /* interval in microseconds */
        rd_bool_t rtmr_oneshot; /**< Only fire once. */

        void (*rtmr_callback)(rd_kafka_timers_t *rkts, void *arg);
        void *rtmr_arg;
} rd_kafka_timer_t;



int rd_kafka_timer_stop(rd_kafka_timers_t *rkts,
                        rd_kafka_timer_t *rtmr,
                        int lock);
void rd_kafka_timer_start0(rd_kafka_timers_t *rkts,
                           rd_kafka_timer_t *rtmr,
                           rd_ts_t interval,
                           rd_bool_t oneshot,
                           rd_bool_t restart,
                           void (*callback)(rd_kafka_timers_t *rkts, void *arg),
                           void *arg);
#define rd_kafka_timer_start(rkts, rtmr, interval, callback, arg)              \
        rd_kafka_timer_start0(rkts, rtmr, interval, rd_false, rd_true,         \
                              callback, arg)
#define rd_kafka_timer_start_oneshot(rkts, rtmr, restart, interval, callback,  \
                                     arg)                                      \
        rd_kafka_timer_start0(rkts, rtmr, interval, rd_true, restart,          \
                              callback, arg)

void rd_kafka_timer_exp_backoff(rd_kafka_timers_t *rkts,
                                rd_kafka_timer_t *rtmr);
rd_ts_t rd_kafka_timer_next(rd_kafka_timers_t *rkts,
                            rd_kafka_timer_t *rtmr,
                            int do_lock);

void rd_kafka_timer_override_once(rd_kafka_timers_t *rkts,
                                  rd_kafka_timer_t *rtmr,
                                  rd_ts_t interval);

/**
 * @returns true if timer is started.
 *
 * @remark Must only be called in the timer's thread (not thread-safe)
 */
rd_bool_t rd_kafka_timer_is_started(rd_kafka_timers_t *rkts,
                                    const rd_kafka_timer_t *rtmr);

void rd_kafka_timers_interrupt(rd_kafka_timers_t *rkts);
rd_ts_t
rd_kafka_timers_next(rd_kafka_timers_t *rkts, int timeout_ms, int do_lock);
void rd_kafka_timers_run(rd_kafka_timers_t *rkts, int timeout_us);
void rd_kafka_timers_destroy(rd_kafka_timers_t *rkts);
void rd_kafka_timers_init(rd_kafka_timers_t *rkte,
                          rd_kafka_t *rk,
                          struct rd_kafka_q_s *wakeq);

#endif /* _RDKAFKA_TIMER_H_ */
