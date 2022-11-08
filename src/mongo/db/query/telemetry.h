/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/query/partitioned_cache.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Type we use to render values to BSON.
 */
using BSONNumeric = long long;

/**
 * An aggregated metric stores a compressed view of data. It balances the loss of information with
 * the reduction in required storage.
 */
struct AggregatedMetric {

    /**
     * Aggregate an observed value into the metric.
     */
    void aggregate(uint64_t val) {
        sum += val;
        max = std::max(val, max);
        min = std::min(val, min);
        sumOfSquares += val * val;
    }

    BSONObj toBSON() const {
        return BSON("sum" << (BSONNumeric)sum << "max" << (BSONNumeric)sum << "min"
                          << (BSONNumeric)sum << "sumOfSquares" << (BSONNumeric)sum);
    }

    uint64_t sum = 0;
    uint64_t min = 0;
    uint64_t max = 0;

    /**
     * The sum of squares along with (an externally stored) count will allow us to compute the
     * variance/stddev.
     */
    uint64_t sumOfSquares = 0;
};

class TelemetryMetrics {
public:
    BSONObj toBSON() const {
        BSONObjBuilder builder{sizeof(TelemetryMetrics) + 100};
        builder.append("lastExecutionMicros", (BSONNumeric)lastExecutionMicros);
        builder.append("execCount", (BSONNumeric)execCount);
        builder.append("queryOptTime", queryOptMicros.toBSON());
        builder.append("queryExecMicros", queryExecMicros.toBSON());
        builder.append("docsReturned", docsReturned.toBSON());
        builder.append("docsScanned", docsScanned.toBSON());
        builder.append("keysScanned", keysScanned.toBSON());
        return builder.obj();
    }

    /**
     * Last execution time in microseconds.
     */
    uint64_t lastExecutionMicros = 0;

    /**
     * Number of query executions.
     */
    uint64_t execCount = 0;

    AggregatedMetric queryOptMicros;

    AggregatedMetric queryExecMicros;

    AggregatedMetric docsReturned;

    AggregatedMetric docsScanned;

    AggregatedMetric keysScanned;
};

struct TelemetryPartitioner {
    // The partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const BSONObj& k, const std::size_t nPartitions) const {
        return SimpleBSONObjComparator::Hasher()(k) % nPartitions;
    }
};

struct ComputeEntrySize {
    size_t operator()(const TelemetryMetrics& entry) {
        return sizeof(TelemetryMetrics);
    }
};

using TelemetryStore = PartitionedCache<BSONObj,
                                        TelemetryMetrics,
                                        ComputeEntrySize,
                                        TelemetryPartitioner,
                                        SimpleBSONObjComparator::Hasher,
                                        SimpleBSONObjComparator::EqualTo>;

/**
 * Acquire a reference to the global telemetry store.
 */
std::pair<TelemetryStore*, Lock::ResourceLock> getTelemetryStoreForRead(ServiceContext* serviceCtx);

std::unique_ptr<TelemetryStore> resetTelemetryStore(ServiceContext* serviceCtx);

namespace telemetry_util {

/**
 * Callback called on a change of telemetryCacheSize parameter.
 */
Status onTelemetryCacheSizeUpdate(const std::string& str);

/**
 * Callback called on validation of telemetryCacheSize parameter.
 */
Status validateTelemetryCacheSize(const std::string& str, const boost::optional<TenantId>&);

}  // namespace telemetry_util
}  // namespace mongo
