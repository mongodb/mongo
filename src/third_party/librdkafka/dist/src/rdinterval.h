/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
 *               2023 Confluent Inc.
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

#ifndef _RDINTERVAL_H_
#define _RDINTERVAL_H_

#include "rd.h"
#include "rdrand.h"

typedef struct rd_interval_s {
        rd_ts_t ri_ts_last; /* last interval timestamp */
        rd_ts_t ri_fixed;   /* fixed interval if provided interval is 0 */
        int ri_backoff;     /* back off the next interval by this much */
} rd_interval_t;


static RD_INLINE RD_UNUSED void rd_interval_init(rd_interval_t *ri) {
        memset(ri, 0, sizeof(*ri));
}



/**
 * Returns the number of microseconds the interval has been over-shot.
 * If the return value is >0 (i.e., time for next intervalled something) then
 * the time interval is updated to the current time.
 *
 * The current time can be provided in 'now', or if this is set to 0 the time
 * will be gathered automatically.
 *
 * If 'interval_us' is set to 0 the fixed interval will be used, see
 * 'rd_interval_fixed()'.
 *
 * If this is the first time rd_interval() is called after an _init() or
 * _reset() or the \p immediate parameter is true, then a positive value
 * will be returned immediately even though the initial interval has not
 * passed.
 */
#define rd_interval(ri, interval_us, now) rd_interval0(ri, interval_us, now, 0)
#define rd_interval_immediate(ri, interval_us, now)                            \
        rd_interval0(ri, interval_us, now, 1)
static RD_INLINE RD_UNUSED rd_ts_t rd_interval0(rd_interval_t *ri,
                                                rd_ts_t interval_us,
                                                rd_ts_t now,
                                                int immediate) {
        rd_ts_t diff;

        if (!now)
                now = rd_clock();
        if (!interval_us)
                interval_us = ri->ri_fixed;

        if (ri->ri_ts_last || !immediate) {
                diff = now - (ri->ri_ts_last + interval_us + ri->ri_backoff);
        } else
                diff = 1;
        if (unlikely(diff > 0)) {
                ri->ri_ts_last = now;
                ri->ri_backoff = 0;
        }

        return diff;
}


/**
 * Reset the interval to zero, i.e., the next call to rd_interval()
 * will be immediate.
 */
static RD_INLINE RD_UNUSED void rd_interval_reset(rd_interval_t *ri) {
        ri->ri_ts_last = 0;
        ri->ri_backoff = 0;
}

/**
 * Reset the interval to 'now'. If now is 0, the time will be gathered
 * automatically.
 */
static RD_INLINE RD_UNUSED void rd_interval_reset_to_now(rd_interval_t *ri,
                                                         rd_ts_t now) {
        if (!now)
                now = rd_clock();

        ri->ri_ts_last = now;
        ri->ri_backoff = 0;
}

/**
 * Reset the interval to 'now' with the given backoff ms and max_jitter as
 * percentage. The backoff is given just for absolute jitter calculation. If now
 * is 0, the time will be gathered automatically.
 */
static RD_INLINE RD_UNUSED void
rd_interval_reset_to_now_with_jitter(rd_interval_t *ri,
                                     rd_ts_t now,
                                     int64_t backoff_ms,
                                     int max_jitter) {
        rd_interval_reset_to_now(ri, now);
        /* We are multiplying by 10 as (backoff_ms * percent * 1000)/100 ->
         * backoff_ms * jitter * 10 */
        ri->ri_backoff = backoff_ms * rd_jitter(-max_jitter, max_jitter) * 10;
}

/**
 * Back off the next interval by `backoff_us` microseconds.
 */
static RD_INLINE RD_UNUSED void rd_interval_backoff(rd_interval_t *ri,
                                                    int backoff_us) {
        ri->ri_backoff = backoff_us;
}

/**
 * Expedite (speed up) the next interval by `expedite_us` microseconds.
 * If `expedite_us` is 0 the interval will be set to trigger
 * immedately on the next rd_interval() call.
 */
static RD_INLINE RD_UNUSED void rd_interval_expedite(rd_interval_t *ri,
                                                     int expedite_us) {
        if (!expedite_us)
                ri->ri_ts_last = 0;
        else
                ri->ri_backoff = -expedite_us;
}

/**
 * Specifies a fixed interval to use if rd_interval() is called with
 * `interval_us` set to 0.
 */
static RD_INLINE RD_UNUSED void rd_interval_fixed(rd_interval_t *ri,
                                                  rd_ts_t fixed_us) {
        ri->ri_fixed = fixed_us;
}

/**
 * Disables the interval (until rd_interval_init()/reset() is called).
 * A disabled interval will never return a positive value from
 * rd_interval().
 */
static RD_INLINE RD_UNUSED void rd_interval_disable(rd_interval_t *ri) {
        /* Set last beat to a large value a long time in the future. */
        ri->ri_ts_last = 6000000000000000000LL; /* in about 190000 years */
}

/**
 * Returns true if the interval is disabled.
 */
static RD_INLINE RD_UNUSED int rd_interval_disabled(const rd_interval_t *ri) {
        return ri->ri_ts_last == 6000000000000000000LL;
}

#endif /* _RDINTERVAL_H_ */
