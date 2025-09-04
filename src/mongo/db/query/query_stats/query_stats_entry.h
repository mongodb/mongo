/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/clonable_ptr.h"
#include "mongo/db/query/query_stats/aggregated_metric.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/supplemental_metrics_stats.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <memory>

namespace mongo::query_stats {

/**
 * The value stored in the query stats store. It contains a Key representing this "kind" of
 * query, and some metrics about that shape. This class is responsible for knowing its size and
 * updating our server status metrics about the size of the query stats store accordingly. At the
 * time of this writing, the LRUCache utility does not easily expose its size in a way we could use
 * as server status metrics.
 */
struct QueryStatsEntry {
    QueryStatsEntry(std::unique_ptr<const Key> key_)
        : firstSeenTimestamp(Date_t::now()), key(std::move(key_)) {}

    BSONObj toBSON() const;

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
    AggregatedMetric<uint64_t> totalExecMicros;

    /**
     * Aggregates the time for execution for first batch only.
     */
    AggregatedMetric<uint64_t> firstResponseExecMicros;

    /**
     * Aggregates the number of documents returned for the query including getMore requests.
     */
    AggregatedMetric<uint64_t> docsReturned;

    /**
     * Aggregates the number of keys examined including getMore requests.
     */
    AggregatedMetric<uint64_t> keysExamined;

    /**
     * Aggregates the number of documents examined including getMore requests.
     */
    AggregatedMetric<uint64_t> docsExamined;

    /**
     * Aggregates the number of bytes read including getMore requests.
     */
    AggregatedMetric<uint64_t> bytesRead;

    /**
     * Aggregates the amount of time spent reading from storage including getMore requests.
     */
    AggregatedMetric<int64_t> readTimeMicros;

    /**
     * Aggregates the executing time (excluding time spent blocked) including getMore requests.
     */
    AggregatedMetric<int64_t> workingTimeMillis;

    /**
     * Aggregates the executing time including getMore requests.
     */
    AggregatedMetric<int64_t> cpuNanos;

    /**
     * Aggregates the delinquent acquisitions stats including getMore requests.
     */
    AggregatedMetric<uint64_t> delinquentAcquisitions;
    AggregatedMetric<int64_t> totalAcquisitionDelinquencyMillis;
    AggregatedMetric<int64_t> maxAcquisitionDelinquencyMillis;

    /**
     * Aggregates the checkForInterrupt stats including getMore requests.
     */
    AggregatedMetric<uint64_t> numInterruptChecksPerSec;
    AggregatedMetric<int64_t> overdueInterruptApproxMaxMillis;

    /**
     * Counts the frequency of the boolean value hasSortStage.
     */
    AggregatedBool hasSortStage;

    /**
     * Counts the frequency of the boolean value usedDisk.
     */
    AggregatedBool usedDisk;

    /**
     * Counts the frequency of the boolean value fromMultiPlanner.
     */
    AggregatedBool fromMultiPlanner;

    /**
     * Counts the frequency of the boolean value fromPlanCache.
     */
    AggregatedBool fromPlanCache;

    /**
     * The Key that can generate the query stats key for this request.
     */
    std::shared_ptr<const Key> key;

    /**
     * Adds supplemental metric to supplementalStatsMap.
     */
    void addSupplementalStats(std::unique_ptr<SupplementalStatsEntry> metric);

    /**
     *  Supplemental metrics. The data structure is not allocated and the pointer is null if
     * optional metrics are not collected.
     */
    clonable_ptr<SupplementalStatsMap> supplementalStatsMap;
};

}  // namespace mongo::query_stats
