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

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/partitioned_cache.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_stats_key_generator.h"
#include "mongo/db/query/query_stats_transform_algorithm_gen.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/views/view.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {
/**
 * Type we use to render values to BSON.
 */
using BSONNumeric = long long;
}  // namespace

namespace query_stats {

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

extern CounterMetric queryStatsStoreSizeEstimateBytesMetric;
const auto kKeySize = sizeof(std::size_t);
// Used to aggregate the metrics for one query stats key over all its executions.
class QueryStatsEntry {
public:
    QueryStatsEntry(std::unique_ptr<KeyGenerator> keyGenerator)
        : firstSeenTimestamp(Date_t::now()), keyGenerator(std::move(keyGenerator)) {
        // Increment by size of query stats store key (hash returns size_t) and value
        // (QueryStatsEntry)
        queryStatsStoreSizeEstimateBytesMetric.increment(kKeySize + size());
    }

    QueryStatsEntry(QueryStatsEntry& entry) = delete;

    QueryStatsEntry(QueryStatsEntry&& entry) = delete;

    ~QueryStatsEntry() {
        // Decrement by size of query stats store key (hash returns size_t) and value
        // (QueryStatsEntry)
        queryStatsStoreSizeEstimateBytesMetric.decrement(kKeySize + size());
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder{sizeof(QueryStatsEntry) + 100};
        builder.append("lastExecutionMicros", (BSONNumeric)lastExecutionMicros);
        builder.append("execCount", (BSONNumeric)execCount);
        totalExecMicros.appendTo(builder, "totalExecMicros");
        firstResponseExecMicros.appendTo(builder, "firstResponseExecMicros");
        docsReturned.appendTo(builder, "docsReturned");
        builder.append("firstSeenTimestamp", firstSeenTimestamp);
        builder.append("latestSeenTimestamp", latestSeenTimestamp);
        return builder.obj();
    }

    int64_t size() {
        return sizeof(*this) + (keyGenerator ? keyGenerator->size() : 0);
    }

    /**
     * Generate the queryStats key for this entry's request. If algorithm is not
     * TransformAlgorithm::kNone, any identifying information (field names, namespace) will be
     * anonymized.
     */
    BSONObj computeQueryStatsKey(OperationContext* opCtx,
                                 TransformAlgorithmEnum algorithm,
                                 std::string hmacKey) const;

    BSONObj getRepresentativeQueryShapeForDebug() const {
        return keyGenerator->getRepresentativeQueryShapeForDebug();
    }

    /**
     * Timestamp for when this query shape was added to the store. Set on construction.
     */
    const Date_t firstSeenTimestamp;

    /**
     * Timestamp for when the latest time this query shape was seen.
     */
    Date_t latestSeenTimestamp;

    /**
     * Last execution time in microseconds.
     */
    uint64_t lastExecutionMicros = 0;

    /**
     * Number of query executions.
     */
    uint64_t execCount = 0;

    /**
     * Aggregates the total time for execution including getMore requests.
     */
    AggregatedMetric totalExecMicros;

    /**
     * Aggregates the time for execution for first batch only.
     */
    AggregatedMetric firstResponseExecMicros;

    AggregatedMetric docsReturned;

    /**
     * The KeyGenerator that can generate the query stats key for this request.
     */
    std::unique_ptr<KeyGenerator> keyGenerator;
};
struct TelemetryPartitioner {
    // The partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const std::size_t k, const std::size_t nPartitions) const {
        return k % nPartitions;
    }
};

struct QueryStatsStoreEntryBudgetor {
    size_t operator()(const std::size_t key, const std::shared_ptr<QueryStatsEntry>& value) {
        return sizeof(decltype(key)) + value->size();
    }
};
using QueryStatsStore = PartitionedCache<std::size_t,
                                         std::shared_ptr<QueryStatsEntry>,
                                         QueryStatsStoreEntryBudgetor,
                                         TelemetryPartitioner>;

/**
 * Acquire a reference to the global queryStats store.
 */
QueryStatsStore& getQueryStatsStore(OperationContext* opCtx);

/**
 * Registers a request for query stats collection. The function may decide not to collect anything,
 * so this should be called for all requests. The decision is made based on the feature flag and
 * query stats rate limiting.
 *
 * The originating command/query does not persist through the end of query execution due to
 * optimizations made to the original query and the expiration of OpCtx across getMores. In order
 * to pair the query stats metrics that are collected at the end of execution with the original
 * query, it is necessary to store the original query during planning and persist it through
 * getMores.
 *
 * During planning, registerRequest is called to serialize the query stats key and save it to
 * OpDebug. If a query's execution is complete within the original operation,
 * collectQueryStatsMongod/collectQueryStatsMongos will call writeQueryStats() and pass along the
 * query stats key to be saved in the query stats store alongside metrics collected.
 *
 * However, OpDebug does not persist through cursor iteration, so if a query's execution will span
 * more than one request/operation, it's necessary to save the query stats context to the cursor
 * upon cursor registration. In these cases, collectQueryStatsMongod/collectQueryStatsMongos will
 * aggregate each operation's metrics within the cursor. Once the request is eventually complete,
 * the cursor calls writeQueryStats() on its destruction.
 *
 * Notes:
 * - It's important to call registerRequest with the original request, before canonicalizing or
 *   optimizing it, in order to preserve the user's input for the query shape.
 * - Calling this affects internal state. It should be called exactly once for each request for
 *   which query stats may be collected.
 * - The std::function argument to construct an abstracted KeyGenerator is provided to break
 *   library cycles so this library does not need to know how to parse everything. It is done as a
 *   deferred construction callback to ensure that this feature does not impact performance if
 *   collecting stats is not needed due to the feature being disabled or the request being rate
 *   limited.
 */
void registerRequest(OperationContext* opCtx,
                     const NamespaceString& collection,
                     std::function<std::unique_ptr<KeyGenerator>(void)> makeKeyGenerator);

/**
 * Writes query stats to the query stats store for the operation identified by `queryStatsKeyHash`.
 *
 * Direct calls to writeQueryStats in new code should be avoided in favor of calling existing
 * functions:
 *  - collectQueryStatsMongod/collectQueryStatsMongos in the case of requests that span one
 *    operation
 *  - ClientCursor::dispose/ClusterClientCursorImpl::kill in the case of requests that span
 *    multiple operations (via getMore)
 */
void writeQueryStats(OperationContext* opCtx,
                     boost::optional<size_t> queryStatsKeyHash,
                     std::unique_ptr<KeyGenerator> keyGenerator,
                     uint64_t queryExecMicros,
                     uint64_t firstResponseExecMicros,
                     uint64_t docsReturned);
}  // namespace query_stats
}  // namespace mongo
