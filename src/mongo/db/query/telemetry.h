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
#include "mongo/bson/bsonobj.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/partitioned_cache.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/db/service_context.h"
#include <cstdint>
#include <memory>

namespace mongo {

class OpDebug;
class AggregateCommandRequest;
class FindCommandRequest;

namespace {
/**
 * Type we use to render values to BSON.
 */
using BSONNumeric = long long;
}  // namespace

namespace telemetry {

bool isTelemetryEnabled();

/**
 * An aggregated metric stores a compressed view of data. It balances the loss of information
 * with the reduction in required storage.
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

    void appendTo(BSONObjBuilder& builder, const StringData& fieldName) const {
        BSONObjBuilder metricsBuilder = builder.subobjStart(fieldName);
        metricsBuilder.append("sum", (BSONNumeric)sum);
        metricsBuilder.append("max", (BSONNumeric)max);
        metricsBuilder.append("min", (BSONNumeric)min);
        metricsBuilder.append("sumOfSquares", (BSONNumeric)sumOfSquares);
        metricsBuilder.done();
    }

    uint64_t sum = 0;
    // Default to the _signed_ maximum (which fits in unsigned range) because we cast to
    // BSONNumeric when serializing.
    uint64_t min = (uint64_t)std::numeric_limits<int64_t>::max;
    uint64_t max = 0;

    /**
     * The sum of squares along with (an externally stored) count will allow us to compute the
     * variance/stddev.
     */
    uint64_t sumOfSquares = 0;
};

extern CounterMetric telemetryStoreSizeEstimateBytesMetric;
// Used to aggregate the metrics for one telemetry key over all its executions.
class TelemetryMetrics {
public:
    TelemetryMetrics(const BSONObj& cmdObj,
                     boost::optional<std::string> applicationName,
                     NamespaceStringOrUUID nss)
        : firstSeenTimestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0),
          cmdObj(cmdObj.copy()),
          applicationName(applicationName),
          nss(nss) {
        telemetryStoreSizeEstimateBytesMetric.increment(sizeof(TelemetryMetrics) + sizeof(BSONObj) +
                                                        cmdObj.objsize());
    }

    ~TelemetryMetrics() {
        telemetryStoreSizeEstimateBytesMetric.decrement(sizeof(TelemetryMetrics) + sizeof(BSONObj) +
                                                        cmdObj.objsize());
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder{sizeof(TelemetryMetrics) + 100};
        builder.append("lastExecutionMicros", (BSONNumeric)lastExecutionMicros);
        builder.append("execCount", (BSONNumeric)execCount);
        queryExecMicros.appendTo(builder, "queryExecMicros");
        docsReturned.appendTo(builder, "docsReturned");
        builder.append("firstSeenTimestamp", firstSeenTimestamp);
        return builder.obj();
    }

    /**
     * Redact a given telemetry key and set _keySize.
     */
    StatusWith<BSONObj> redactKey(const BSONObj& key,
                                  bool redactIdentifiers,
                                  OperationContext* opCtx) const;

    /**
     * Timestamp for when this query shape was added to the store. Set on construction.
     */
    const Timestamp firstSeenTimestamp;

    /**
     * Last execution time in microseconds.
     */
    uint64_t lastExecutionMicros = 0;

    /**
     * Number of query executions.
     */
    uint64_t execCount = 0;

    AggregatedMetric queryExecMicros;

    AggregatedMetric docsReturned;

    /**
     * A representative command for a given telemetry key. This is used to derive the redacted
     * telemetry key at read-time.
     */
    BSONObj cmdObj;

    /**
     * The application name that is a part of the query shape. It is necessary to store this
     * separately from the telemetry key since it exists on the OpCtx, not the cmdObj.
     */
    boost::optional<std::string> applicationName;

    NamespaceStringOrUUID nss;

private:
    /**
     * We cache the redacted key the first time it's computed.
     */
    mutable boost::optional<BSONObj> _redactedKey;
};

struct TelemetryPartitioner {
    // The partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const BSONObj& k, const std::size_t nPartitions) const {
        return SimpleBSONObjComparator::Hasher()(k) % nPartitions;
    }
};

struct TelemetryStoreEntryBudgetor {
    size_t operator()(const BSONObj& key, const std::shared_ptr<TelemetryMetrics>& value) {
        // The buget estimator for <key,value> pair in LRU cache accounts for size of value
        // (TelemetryMetrics) size of the key, and size of the key's underlying data struture
        // (BSONObj).
        return sizeof(TelemetryMetrics) + sizeof(BSONObj) + key.objsize();
    }
};

using TelemetryStore = PartitionedCache<BSONObj,
                                        std::shared_ptr<TelemetryMetrics>,
                                        TelemetryStoreEntryBudgetor,
                                        TelemetryPartitioner,
                                        SimpleBSONObjComparator::Hasher,
                                        SimpleBSONObjComparator::EqualTo>;

/**
 * Acquire a reference to the global telemetry store.
 */
TelemetryStore& getTelemetryStore(OperationContext* opCtx);

/**
 * Register a request for telemetry collection. The telemetry machinery may decide not to
 * collect anything but this should be called for all requests. The decision is made based on
 * the feature flag and telemetry parameters such as rate limiting.
 *
 * The caller is still responsible for subsequently calling writeTelemetry() once the request is
 * completed.
 *
 * Note that calling this affects internal state. It should be called once for each request for
 * which telemetry may be collected.
 */
void registerAggRequest(const AggregateCommandRequest& request, OperationContext* opCtx);

void registerFindRequest(const FindCommandRequest& request,
                         const NamespaceString& collection,
                         OperationContext* ocCtx,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Writes telemetry to the telemetry store for the operation identified by `telemetryKey`.
 */
void writeTelemetry(OperationContext* opCtx,
                    boost::optional<BSONObj> telemetryKey,
                    const BSONObj& cmdObj,
                    uint64_t queryExecMicros,
                    uint64_t docsReturned);

/**
 * Serialize the FindCommandRequest according to the Options passed in. Returns the serialized BSON
 * with all field names and literals redacted.
 */
StatusWith<BSONObj> makeTelemetryKey(
    const FindCommandRequest& findCommand,
    const SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<const TelemetryMetrics&> existingMetrics = boost::none);

/**
 * Collects metrics for telemetry from the current operation onto OpDebug. This must be called prior
 * to incrementing metrics on cursors (either ClientCursor or ClusterClientCursor) since cursor
 * metric aggregation happens via OpDebug::AdditiveMetrics.
 */
void collectMetricsOnOpDebug(CurOp* curOp, long long nreturned);
}  // namespace telemetry
}  // namespace mongo
