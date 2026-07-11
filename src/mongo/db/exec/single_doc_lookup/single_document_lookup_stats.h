// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/util/duration.h"

namespace mongo::exec {

/**
 * Records the outcome (and, where applicable, the latency) of a single-document lookup into one
 * cell. Concrete and non-virtual: an executor borrows a recorder over a process-global cell rather
 * than owning metrics. The latency is recorded only for outcomes that actually ran the lookup
 * (found / not-found); a declined lookup (not-handled) carries no meaningful latency.
 */
class SingleDocumentLookupStatsRecorder {
public:
    explicit SingleDocumentLookupStatsRecorder(otel::metrics::Counter<int64_t>& found,
                                               otel::metrics::Counter<int64_t>& notFound,
                                               otel::metrics::Counter<int64_t>& notHandled,
                                               otel::metrics::Histogram<int64_t>& latencyMicros)
        : _found(found),
          _notFound(notFound),
          _notHandled(notHandled),
          _latencyMicros(latencyMicros) {}

    static SingleDocumentLookupStatsRecorder makeUpdateLookupExpressRecorder();
    static SingleDocumentLookupStatsRecorder makeUpdateLookupAggregationRecorder();
    static SingleDocumentLookupStatsRecorder makeUpdateLookupSbeRecorder();

    void recordFound(Microseconds elapsed) {
        _found.add(1);
        _latencyMicros.record(durationCount<Microseconds>(elapsed));
    }

    void recordNotFound(Microseconds elapsed) {
        _notFound.add(1);
        _latencyMicros.record(durationCount<Microseconds>(elapsed));
    }

    void recordNotHandled() {
        _notHandled.add(1);
    }

private:
    otel::metrics::Counter<int64_t>& _found;
    otel::metrics::Counter<int64_t>& _notFound;
    otel::metrics::Counter<int64_t>& _notHandled;
    otel::metrics::Histogram<int64_t>& _latencyMicros;
};

}  // namespace mongo::exec
