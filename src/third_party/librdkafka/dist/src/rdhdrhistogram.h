/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018, Magnus Edenhill
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
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _RDHDR_HISTOGRAM_H_
#define _RDHDR_HISTOGRAM_H_

#include <inttypes.h>


typedef struct rd_hdr_histogram_s {
        int64_t lowestTrackableValue;
        int64_t highestTrackableValue;
        int64_t unitMagnitude;
        int64_t significantFigures;
        int32_t subBucketHalfCountMagnitude;
        int32_t subBucketHalfCount;
        int64_t subBucketMask;
        int32_t subBucketCount;
        int32_t bucketCount;
        int32_t countsLen;
        int64_t totalCount;
        int64_t *counts;
        int64_t outOfRangeCount;   /**< Number of rejected records due to
                                    *   value being out of range. */
        int64_t lowestOutOfRange;  /**< Lowest value that was out of range.
                                    *   Initialized to lowestTrackableValue */
        int64_t highestOutOfRange; /**< Highest value that was out of range.
                                    *   Initialized to highestTrackableValue */
        int32_t allocatedSize;     /**< Allocated size of histogram, for
                                    *   sigfigs tuning. */
} rd_hdr_histogram_t;


#endif /* !_RDHDR_HISTOGRAM_H_ */


void rd_hdr_histogram_destroy(rd_hdr_histogram_t *hdr);

/**
 * @brief Create a new Hdr_Histogram.
 *
 * @param significant_figures must be between 1..5
 *
 * @returns a newly allocated histogram, or NULL on error.
 *
 * @sa rd_hdr_histogram_destroy()
 */
rd_hdr_histogram_t *rd_hdr_histogram_new(int64_t minValue,
                                         int64_t maxValue,
                                         int significantFigures);

void rd_hdr_histogram_reset(rd_hdr_histogram_t *hdr);

int rd_hdr_histogram_record(rd_hdr_histogram_t *hdr, int64_t v);

double rd_hdr_histogram_stddev(rd_hdr_histogram_t *hdr);
double rd_hdr_histogram_mean(const rd_hdr_histogram_t *hdr);
int64_t rd_hdr_histogram_max(const rd_hdr_histogram_t *hdr);
int64_t rd_hdr_histogram_min(const rd_hdr_histogram_t *hdr);
int64_t rd_hdr_histogram_quantile(const rd_hdr_histogram_t *hdr, double q);


int unittest_rdhdrhistogram(void);
