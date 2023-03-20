/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
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

#ifndef _RDAVG_H_
#define _RDAVG_H_


#if WITH_HDRHISTOGRAM
#include "rdhdrhistogram.h"
#endif

typedef struct rd_avg_s {
        struct {
                int64_t maxv;
                int64_t minv;
                int64_t avg;
                int64_t sum;
                int cnt;
                rd_ts_t start;
        } ra_v;
        mtx_t ra_lock;
        int ra_enabled;
        enum { RD_AVG_GAUGE,
               RD_AVG_COUNTER,
        } ra_type;
#if WITH_HDRHISTOGRAM
        rd_hdr_histogram_t *ra_hdr;
#endif
        /* Histogram results, calculated for dst in rollover().
         * Will be all zeroes if histograms are not supported. */
        struct {
                /* Quantiles */
                int64_t p50;
                int64_t p75;
                int64_t p90;
                int64_t p95;
                int64_t p99;
                int64_t p99_99;

                int64_t oor;     /**< Values out of range */
                int32_t hdrsize; /**< hdr.allocatedSize */
                double stddev;
                double mean;
        } ra_hist;
} rd_avg_t;


/**
 * @brief Add value \p v to averager \p ra.
 */
static RD_UNUSED void rd_avg_add(rd_avg_t *ra, int64_t v) {
        mtx_lock(&ra->ra_lock);
        if (!ra->ra_enabled) {
                mtx_unlock(&ra->ra_lock);
                return;
        }
        if (v > ra->ra_v.maxv)
                ra->ra_v.maxv = v;
        if (ra->ra_v.minv == 0 || v < ra->ra_v.minv)
                ra->ra_v.minv = v;
        ra->ra_v.sum += v;
        ra->ra_v.cnt++;
#if WITH_HDRHISTOGRAM
        rd_hdr_histogram_record(ra->ra_hdr, v);
#endif
        mtx_unlock(&ra->ra_lock);
}


/**
 * @brief Calculate the average
 */
static RD_UNUSED void rd_avg_calc(rd_avg_t *ra, rd_ts_t now) {
        if (ra->ra_type == RD_AVG_GAUGE) {
                if (ra->ra_v.cnt)
                        ra->ra_v.avg = ra->ra_v.sum / ra->ra_v.cnt;
                else
                        ra->ra_v.avg = 0;
        } else {
                rd_ts_t elapsed = now - ra->ra_v.start;

                if (elapsed)
                        ra->ra_v.avg = (ra->ra_v.sum * 1000000llu) / elapsed;
                else
                        ra->ra_v.avg = 0;

                ra->ra_v.start = elapsed;
        }
}


/**
 * @returns the quantile \q for \p ra, or 0 if histograms are not supported
 *          in this build.
 *
 * @remark ra will be not locked by this function.
 */
static RD_UNUSED int64_t rd_avg_quantile(const rd_avg_t *ra, double q) {
#if WITH_HDRHISTOGRAM
        return rd_hdr_histogram_quantile(ra->ra_hdr, q);
#else
        return 0;
#endif
}

/**
 * @brief Rolls over statistics in \p src and stores the average in \p dst.
 * \p src is cleared and ready to be reused.
 *
 * Caller must free avg internal members by calling rd_avg_destroy()
 * on the \p dst.
 */
static RD_UNUSED void rd_avg_rollover(rd_avg_t *dst, rd_avg_t *src) {
        rd_ts_t now;

        mtx_lock(&src->ra_lock);
        if (!src->ra_enabled) {
                memset(dst, 0, sizeof(*dst));
                dst->ra_type = src->ra_type;
                mtx_unlock(&src->ra_lock);
                return;
        }

        mtx_init(&dst->ra_lock, mtx_plain);
        dst->ra_type = src->ra_type;
        dst->ra_v    = src->ra_v;
#if WITH_HDRHISTOGRAM
        dst->ra_hdr = NULL;

        dst->ra_hist.stddev  = rd_hdr_histogram_stddev(src->ra_hdr);
        dst->ra_hist.mean    = rd_hdr_histogram_mean(src->ra_hdr);
        dst->ra_hist.oor     = src->ra_hdr->outOfRangeCount;
        dst->ra_hist.hdrsize = src->ra_hdr->allocatedSize;
        dst->ra_hist.p50     = rd_hdr_histogram_quantile(src->ra_hdr, 50.0);
        dst->ra_hist.p75     = rd_hdr_histogram_quantile(src->ra_hdr, 75.0);
        dst->ra_hist.p90     = rd_hdr_histogram_quantile(src->ra_hdr, 90.0);
        dst->ra_hist.p95     = rd_hdr_histogram_quantile(src->ra_hdr, 95.0);
        dst->ra_hist.p99     = rd_hdr_histogram_quantile(src->ra_hdr, 99.0);
        dst->ra_hist.p99_99  = rd_hdr_histogram_quantile(src->ra_hdr, 99.99);
#else
        memset(&dst->ra_hist, 0, sizeof(dst->ra_hist));
#endif
        memset(&src->ra_v, 0, sizeof(src->ra_v));

        now             = rd_clock();
        src->ra_v.start = now;

#if WITH_HDRHISTOGRAM
        /* Adapt histogram span to fit future out of range entries
         * from this period. */
        if (src->ra_hdr->totalCount > 0) {
                int64_t vmin = src->ra_hdr->lowestTrackableValue;
                int64_t vmax = src->ra_hdr->highestTrackableValue;
                int64_t mindiff, maxdiff;

                mindiff = src->ra_hdr->lowestTrackableValue -
                          src->ra_hdr->lowestOutOfRange;

                if (mindiff > 0) {
                        /* There were low out of range values, grow lower
                         * span to fit lowest out of range value + 20%. */
                        vmin = src->ra_hdr->lowestOutOfRange +
                               (int64_t)((double)mindiff * 0.2);
                }

                maxdiff = src->ra_hdr->highestOutOfRange -
                          src->ra_hdr->highestTrackableValue;

                if (maxdiff > 0) {
                        /* There were high out of range values, grow higher
                         * span to fit highest out of range value + 20%. */
                        vmax = src->ra_hdr->highestOutOfRange +
                               (int64_t)((double)maxdiff * 0.2);
                }

                if (vmin == src->ra_hdr->lowestTrackableValue &&
                    vmax == src->ra_hdr->highestTrackableValue) {
                        /* No change in min,max, use existing hdr */
                        rd_hdr_histogram_reset(src->ra_hdr);

                } else {
                        int sigfigs = (int)src->ra_hdr->significantFigures;
                        /* Create new hdr for adapted range */
                        rd_hdr_histogram_destroy(src->ra_hdr);
                        src->ra_hdr = rd_hdr_histogram_new(vmin, vmax, sigfigs);
                }

        } else {
                /* No records, no need to reset. */
        }
#endif

        mtx_unlock(&src->ra_lock);

        rd_avg_calc(dst, now);
}


/**
 * Initialize an averager
 */
static RD_UNUSED void rd_avg_init(rd_avg_t *ra,
                                  int type,
                                  int64_t exp_min,
                                  int64_t exp_max,
                                  int sigfigs,
                                  int enable) {
        memset(ra, 0, sizeof(*ra));
        mtx_init(&ra->ra_lock, 0);
        ra->ra_enabled = enable;
        if (!enable)
                return;
        ra->ra_type    = type;
        ra->ra_v.start = rd_clock();
#if WITH_HDRHISTOGRAM
        /* Start off the histogram with expected min,max span,
         * we'll adapt the size on each rollover. */
        ra->ra_hdr = rd_hdr_histogram_new(exp_min, exp_max, sigfigs);
#endif
}


/**
 * Destroy averager
 */
static RD_UNUSED void rd_avg_destroy(rd_avg_t *ra) {
#if WITH_HDRHISTOGRAM
        if (ra->ra_hdr)
                rd_hdr_histogram_destroy(ra->ra_hdr);
#endif
        mtx_destroy(&ra->ra_lock);
}

#endif /* _RDAVG_H_ */
