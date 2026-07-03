/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
