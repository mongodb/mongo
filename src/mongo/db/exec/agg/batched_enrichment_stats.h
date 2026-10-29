// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_counter.h"

namespace mongo::exec::agg {

/**
 * Records how many enrich sub-batches BatchedEnrichmentStage has opened.
 * This is not a per-engine metric: it counts enrich sub-batches regardless of which executor the
 * stage's fallback chain actually used to serve them, so it's the right knob-invariance signal for
 * the batching-knob (MaxBatchSize, MaxInputBytes, MaxOutputBytes) sweep.
 */
class BatchedEnrichmentStatsRecorder {
public:
    explicit BatchedEnrichmentStatsRecorder(otel::metrics::Counter<int64_t>& batchesStarted)
        : _batchesStarted(batchesStarted) {}

    static BatchedEnrichmentStatsRecorder makeChangeStreamUpdateLookupRecorder();
    static BatchedEnrichmentStatsRecorder makeSearchIdLookupRecorder();

    /**
     * A recorder over a shared no-op counter, for test doubles that construct
     * BatchedEnrichmentStage directly and don't represent either production consumer.
     */
    static BatchedEnrichmentStatsRecorder makeNoopRecorder_forTest() {
        return BatchedEnrichmentStatsRecorder(*otel::metrics::NoopCounter<int64_t>::instance());
    }

    void recordBatchStarted() {
        _batchesStarted.add(1);
    }

private:
    otel::metrics::Counter<int64_t>& _batchesStarted;
};

}  // namespace mongo::exec::agg
