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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/tracking/memory.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo::timeseries::bucket_catalog {

struct ExecutionStats {
    /**
     * Gauges. The values represent a state in the system. They can go up and down and are
     * decremented from the global statistics when collections are dropped.
     */

    // Cardinality of opened and archived buckets. Used to estimate storage engine cache usage for
    // the workload.
    Atomic<long long> numActiveBuckets;

    /**
     * Counters. The values represent statistics of what has happened in the system for diagnostics.
     * The values only increment and are not decremented from the global statistics when collections
     * are dropped.
     */
    Atomic<long long> numBucketInserts;
    Atomic<long long> numBucketUpdates;
    Atomic<long long> numBucketsOpenedDueToMetadata;
    Atomic<long long> numBucketsClosedDueToCount;
    Atomic<long long> numBucketsClosedDueToSchemaChange;
    Atomic<long long> numBucketsClosedDueToSize;
    Atomic<long long> numBucketsClosedDueToCachePressure;
    Atomic<long long> numBucketsClosedDueToTimeForward;
    Atomic<long long> numBucketsClosedDueToMemoryThreshold;
    Atomic<long long> numBucketsClosedDueToReopening;
    Atomic<long long> numBucketsArchivedDueToMemoryThreshold;
    Atomic<long long> numBucketsArchivedDueToTimeBackward;
    Atomic<long long> numBucketsFrozen;
    Atomic<long long> numCompressedBucketsConvertedToUnsorted;
    Atomic<long long> numCommits;
    Atomic<long long> numWaits;
    Atomic<long long> numMeasurementsCommitted;
    Atomic<long long> numBucketsReopened;
    Atomic<long long> numBucketsKeptOpenDueToLargeMeasurements;
    Atomic<long long> numBucketsFetched;
    Atomic<long long> numBucketsQueried;
    Atomic<long long> numBucketFetchesFailed;
    Atomic<long long> numBucketQueriesFailed;
    Atomic<long long> numBucketReopeningsFailedDueToEraMismatch;
    Atomic<long long> numBucketReopeningsFailedDueToMalformedIdField;
    Atomic<long long> numBucketReopeningsFailedDueToHashCollision;
    Atomic<long long> numBucketReopeningsFailedDueToMarkedFrozen;
    Atomic<long long> numBucketReopeningsFailedDueToValidator;
    Atomic<long long> numBucketReopeningsFailedDueToMarkedClosed;
    Atomic<long long> numBucketReopeningsFailedDueToMinMaxCalculation;
    Atomic<long long> numBucketReopeningsFailedDueToSchemaGeneration;
    Atomic<long long> numBucketReopeningsFailedDueToUncompressedTimeColumn;
    Atomic<long long> numBucketReopeningsFailedDueToCompressionFailure;
    Atomic<long long> numBucketReopeningsFailedDueToWriteConflict;
    Atomic<long long> numDuplicateBucketsReopened;
    Atomic<long long> numBucketDocumentsTooLargeInsert;
    Atomic<long long> numBucketDocumentsTooLargeUpdate;
};

class ExecutionStatsController {
public:
    ExecutionStatsController() = default;
    ExecutionStatsController(const tracking::shared_ptr<ExecutionStats>& collectionStats,
                             ExecutionStats& globalStats)
        : _collectionStats(collectionStats), _globalStats(&globalStats) {}

    // Gauges
    void incNumActiveBuckets(long long increment = 1);
    void decNumActiveBuckets(long long decrement = 1);

    // Counters
    void incNumBucketInserts(long long increment = 1);
    void incNumBucketUpdates(long long increment = 1);
    void incNumBucketsOpenedDueToMetadata(long long increment = 1);
    void incNumBucketsClosedDueToCount(long long increment = 1);
    void incNumBucketsClosedDueToSchemaChange(long long increment = 1);
    void incNumBucketsClosedDueToSize(long long increment = 1);
    void incNumBucketsClosedDueToCachePressure(long long increment = 1);
    void incNumBucketsClosedDueToTimeForward(long long increment = 1);
    void incNumBucketsClosedDueToMemoryThreshold(long long increment = 1);
    void incNumBucketsClosedDueToReopening(long long increment = 1);
    void incNumBucketsArchivedDueToMemoryThreshold(long long increment = 1);
    void incNumBucketsArchivedDueToTimeBackward(long long increment = 1);
    void incNumBucketsFrozen(long long increment = 1);
    void incNumCompressedBucketsConvertedToUnsorted(long long increment = 1);
    void incNumCommits(long long increment = 1);
    void incNumWaits(long long increment = 1);
    void incNumMeasurementsCommitted(long long increment = 1);
    void incNumBucketsReopened(long long increment = 1);
    void incNumBucketsKeptOpenDueToLargeMeasurements(long long increment = 1);
    void incNumBucketsFetched(long long increment = 1);
    void incNumBucketsQueried(long long increment = 1);
    void incNumBucketFetchesFailed(long long increment = 1);
    void incNumBucketQueriesFailed(long long increment = 1);
    void incNumBucketReopeningsFailedDueToEraMismatch(long long increment = 1);
    void incNumBucketReopeningsFailedDueToMalformedIdField(long long increment = 1);
    void incNumBucketReopeningsFailedDueToHashCollision(long long increment = 1);
    void incNumBucketReopeningsFailedDueToMarkedFrozen(long long increment = 1);
    void incNumBucketReopeningsFailedDueToValidator(long long increment = 1);
    void incNumBucketReopeningsFailedDueToMarkedClosed(long long increment = 1);
    void incNumBucketReopeningsFailedDueToMinMaxCalculation(long long increment = 1);
    void incNumBucketReopeningsFailedDueToSchemaGeneration(long long increment = 1);
    void incNumBucketReopeningsFailedDueToUncompressedTimeColumn(long long increment = 1);
    void incNumBucketReopeningsFailedDueToCompressionFailure(long long increment = 1);
    void incNumBucketReopeningsFailedDueToWriteConflict(long long increment = 1);
    void incNumDuplicateBucketsReopened(long long increment = 1);
    void incNumBucketDocumentsTooLargeInsert(long long increment = 1);
    void incNumBucketDocumentsTooLargeUpdate(long long increment = 1);

private:
    tracking::shared_ptr<ExecutionStats> _collectionStats;
    ExecutionStats* _globalStats = nullptr;
};

void appendExecutionStatsToBuilder(const ExecutionStats& stats, BSONObjBuilder& builder);

/**
 * Adds the execution stats classified as counters of a collection to both the collection and global
 * stats of an execution stats controller.
 */
void addCollectionExecutionCounters(ExecutionStatsController& stats,
                                    const ExecutionStats& collStats);

/**
 * Adds the execution stats classified as gauges of a collection from an ExecutionStats.
 */
void addCollectionExecutionGauges(ExecutionStats& stats, const ExecutionStats& collStats);

/**
 * Removes the execution stats classified as gauges from an ExecutionStats.
 */
void removeCollectionExecutionGauges(ExecutionStats& stats, ExecutionStats& collStats);

}  // namespace mongo::timeseries::bucket_catalog
