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

#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"

#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/platform/compiler.h"

#include <limits>

namespace mongo::timeseries::bucket_catalog {
namespace {
constexpr long long kNumActiveBucketsSentinel = std::numeric_limits<long long>::min();

/**
 * Adds 'increment' to 'atomicValue' with conditions.
 * No-op if 'atomicValue' is already set to the sentinel value.
 * 'increment' can be negative but the result of the addition has to be non-negative.
 */
long long addFloored(AtomicWord<long long>& atomicValue, long long increment) {
    if (MONGO_unlikely(!increment)) {
        return static_cast<long long>(0);
    }

    long long current = atomicValue.load();
    long long result = 0;
    long long actualIncrement = increment;

    do {
        if (current == kNumActiveBucketsSentinel) {
            // No-op if a sentinel value was already set.
            return static_cast<long long>(0);
        }
        // Result cannot be negative.
        result = std::max(static_cast<long long>(0), current + increment);
        actualIncrement = result - current;
    } while (!atomicValue.compareAndSwap(&current, result));

    return actualIncrement;
}
}  // namespace

void ExecutionStatsController::incNumActiveBuckets(long long increment) {
    // Increments the global stats if the collection stats are modified.
    if (auto actualIncrement = addFloored(_collectionStats->numActiveBuckets, increment)) {
        tassert(10645100, "numActiveBuckets overflowed", increment == actualIncrement);
        _globalStats->numActiveBuckets.fetchAndAddRelaxed(increment);
    }
}

void ExecutionStatsController::decNumActiveBuckets(long long decrement) {
    // Decrements the global and collection stats with the same value.
    if (auto actualIncrement = addFloored(_collectionStats->numActiveBuckets, -decrement)) {
        addFloored(_globalStats->numActiveBuckets, actualIncrement);
    }
}

void ExecutionStatsController::incNumBucketInserts(long long increment) {
    _collectionStats->numBucketInserts.fetchAndAddRelaxed(increment);
    _globalStats->numBucketInserts.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketUpdates(long long increment) {
    _collectionStats->numBucketUpdates.fetchAndAddRelaxed(increment);
    _globalStats->numBucketUpdates.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsOpenedDueToMetadata(long long increment) {
    _collectionStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsOpenedDueToMetadata.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToCount(long long increment) {
    _collectionStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToCount.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToSchemaChange(long long increment) {
    _collectionStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSchemaChange.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToSize(long long increment) {
    _collectionStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToSize.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToCachePressure(long long increment) {
    _collectionStats->numBucketsClosedDueToCachePressure.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToCachePressure.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToTimeForward(long long increment) {
    _collectionStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeForward.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToMemoryThreshold(long long increment) {
    _collectionStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsClosedDueToReopening(long long increment) {
    _collectionStats->numBucketsClosedDueToReopening.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToReopening.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsArchivedDueToMemoryThreshold(long long increment) {
    _collectionStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToMemoryThreshold.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsArchivedDueToTimeBackward(long long increment) {
    _collectionStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsArchivedDueToTimeBackward.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumCompressedBucketsConvertedToUnsorted(long long increment) {
    _collectionStats->numCompressedBucketsConvertedToUnsorted.fetchAndAddRelaxed(increment);
    _globalStats->numCompressedBucketsConvertedToUnsorted.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsFrozen(long long increment) {
    _collectionStats->numBucketsFrozen.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsFrozen.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumCommits(long long increment) {
    _collectionStats->numCommits.fetchAndAddRelaxed(increment);
    _globalStats->numCommits.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumWaits(long long increment) {
    _collectionStats->numWaits.fetchAndAddRelaxed(increment);
    _globalStats->numWaits.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumMeasurementsCommitted(long long increment) {
    _collectionStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
    _globalStats->numMeasurementsCommitted.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsReopened(long long increment) {
    _collectionStats->numBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsReopened.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsKeptOpenDueToLargeMeasurements(long long increment) {
    _collectionStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsKeptOpenDueToLargeMeasurements.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketFetchesFailed(long long increment) {
    _collectionStats->numBucketFetchesFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketFetchesFailed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketQueriesFailed(long long increment) {
    _collectionStats->numBucketQueriesFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketQueriesFailed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsFetched(long long increment) {
    _collectionStats->numBucketsFetched.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsFetched.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketsQueried(long long increment) {
    _collectionStats->numBucketsQueried.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsQueried.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToEraMismatch(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToEraMismatch.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToEraMismatch.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToMalformedIdField(
    long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToMalformedIdField.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToMalformedIdField.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToHashCollision(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToHashCollision.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToHashCollision.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToMarkedFrozen(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToMarkedFrozen.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToMarkedFrozen.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToValidator(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToValidator.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToValidator.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToMarkedClosed(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToMarkedClosed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToMarkedClosed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToMinMaxCalculation(
    long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToMinMaxCalculation.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToMinMaxCalculation.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToSchemaGeneration(
    long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToSchemaGeneration.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToSchemaGeneration.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToUncompressedTimeColumn(
    long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToUncompressedTimeColumn.fetchAndAddRelaxed(
        increment);
    _globalStats->numBucketReopeningsFailedDueToUncompressedTimeColumn.fetchAndAddRelaxed(
        increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToCompressionFailure(
    long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToCompressionFailure.fetchAndAddRelaxed(
        increment);
    _globalStats->numBucketReopeningsFailedDueToCompressionFailure.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketReopeningsFailedDueToWriteConflict(long long increment) {
    _collectionStats->numBucketReopeningsFailedDueToWriteConflict.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailedDueToWriteConflict.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumDuplicateBucketsReopened(long long increment) {
    _collectionStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketDocumentsTooLargeInsert(long long increment) {
    _collectionStats->numBucketDocumentsTooLargeInsert.fetchAndAddRelaxed(increment);
    _globalStats->numBucketDocumentsTooLargeInsert.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBucketDocumentsTooLargeUpdate(long long increment) {
    _collectionStats->numBucketDocumentsTooLargeUpdate.fetchAndAddRelaxed(increment);
    _globalStats->numBucketDocumentsTooLargeUpdate.fetchAndAddRelaxed(increment);
}

void appendExecutionStatsToBuilder(const ExecutionStats& stats, BSONObjBuilder& builder) {
    builder.appendNumber("numActiveBuckets", stats.numActiveBuckets.load());
    builder.appendNumber("numBucketInserts", stats.numBucketInserts.load());
    builder.appendNumber("numBucketUpdates", stats.numBucketUpdates.load());
    builder.appendNumber("numBucketsOpenedDueToMetadata",
                         stats.numBucketsOpenedDueToMetadata.load());
    builder.appendNumber("numBucketsClosedDueToCount", stats.numBucketsClosedDueToCount.load());
    builder.appendNumber("numBucketsClosedDueToSchemaChange",
                         stats.numBucketsClosedDueToSchemaChange.load());
    builder.appendNumber("numBucketsClosedDueToSize", stats.numBucketsClosedDueToSize.load());
    builder.appendNumber("numBucketsClosedDueToTimeForward",
                         stats.numBucketsClosedDueToTimeForward.load());
    builder.appendNumber("numBucketsClosedDueToMemoryThreshold",
                         stats.numBucketsClosedDueToMemoryThreshold.load());

    auto commits = stats.numCommits.load();
    builder.appendNumber("numCommits", commits);
    builder.appendNumber("numWaits", stats.numWaits.load());
    auto measurementsCommitted = stats.numMeasurementsCommitted.load();
    builder.appendNumber("numMeasurementsCommitted", measurementsCommitted);
    if (commits) {
        builder.appendNumber("avgNumMeasurementsPerCommit", measurementsCommitted / commits);
    }

    builder.appendNumber("numBucketsClosedDueToReopening",
                         stats.numBucketsClosedDueToReopening.load());
    builder.appendNumber("numBucketsArchivedDueToMemoryThreshold",
                         stats.numBucketsArchivedDueToMemoryThreshold.load());
    builder.appendNumber("numBucketsArchivedDueToTimeBackward",
                         stats.numBucketsArchivedDueToTimeBackward.load());
    builder.appendNumber("numBucketsReopened", stats.numBucketsReopened.load());
    builder.appendNumber("numBucketsKeptOpenDueToLargeMeasurements",
                         stats.numBucketsKeptOpenDueToLargeMeasurements.load());
    builder.appendNumber("numBucketsClosedDueToCachePressure",
                         stats.numBucketsClosedDueToCachePressure.load());
    builder.appendNumber("numBucketsFrozen", stats.numBucketsFrozen.load());
    builder.appendNumber("numCompressedBucketsConvertedToUnsorted",
                         stats.numCompressedBucketsConvertedToUnsorted.load());
    builder.appendNumber("numBucketsFetched", stats.numBucketsFetched.load());
    builder.appendNumber("numBucketsQueried", stats.numBucketsQueried.load());
    builder.appendNumber("numBucketFetchesFailed", stats.numBucketFetchesFailed.load());
    builder.appendNumber("numBucketQueriesFailed", stats.numBucketQueriesFailed.load());
    builder.appendNumber("numBucketReopeningsFailedDueToEraMismatch",
                         stats.numBucketReopeningsFailedDueToEraMismatch.load());
    builder.appendNumber("numBucketReopeningsFailedDueToMalformedIdField",
                         stats.numBucketReopeningsFailedDueToMalformedIdField.load());
    builder.appendNumber("numBucketReopeningsFailedDueToHashCollision",
                         stats.numBucketReopeningsFailedDueToHashCollision.load());
    builder.appendNumber("numBucketReopeningsFailedDueToMarkedFrozen",
                         stats.numBucketReopeningsFailedDueToMarkedFrozen.load());
    builder.appendNumber("numBucketReopeningsFailedDueToValidator",
                         stats.numBucketReopeningsFailedDueToValidator.load());
    builder.appendNumber("numBucketReopeningsFailedDueToMarkedClosed",
                         stats.numBucketReopeningsFailedDueToMarkedClosed.load());
    builder.appendNumber("numBucketReopeningsFailedDueToMinMaxCalculation",
                         stats.numBucketReopeningsFailedDueToMinMaxCalculation.load());
    builder.appendNumber("numBucketReopeningsFailedDueToSchemaGeneration",
                         stats.numBucketReopeningsFailedDueToSchemaGeneration.load());
    builder.appendNumber("numBucketReopeningsFailedDueToUncompressedTimeColumn",
                         stats.numBucketReopeningsFailedDueToUncompressedTimeColumn.load());
    builder.appendNumber("numBucketReopeningsFailedDueToCompressionFailure",
                         stats.numBucketReopeningsFailedDueToCompressionFailure.load());
    builder.appendNumber("numBucketReopeningsFailedDueToWriteConflict",
                         stats.numBucketReopeningsFailedDueToWriteConflict.load());
    builder.appendNumber("numDuplicateBucketsReopened", stats.numDuplicateBucketsReopened.load());
    builder.appendNumber("numBucketDocumentsTooLargeInsert",
                         stats.numBucketDocumentsTooLargeInsert.load());
    builder.appendNumber("numBucketDocumentsTooLargeUpdate",
                         stats.numBucketDocumentsTooLargeUpdate.load());
}

void addCollectionExecutionCounters(ExecutionStatsController& stats,
                                    const ExecutionStats& collStats) {
    stats.incNumBucketInserts(collStats.numBucketInserts.load());
    stats.incNumBucketUpdates(collStats.numBucketUpdates.load());
    stats.incNumBucketsOpenedDueToMetadata(collStats.numBucketsOpenedDueToMetadata.load());
    stats.incNumBucketsClosedDueToCount(collStats.numBucketsClosedDueToCount.load());
    stats.incNumBucketsClosedDueToSchemaChange(collStats.numBucketsClosedDueToSchemaChange.load());
    stats.incNumBucketsClosedDueToSize(collStats.numBucketsClosedDueToSize.load());
    stats.incNumBucketsClosedDueToCachePressure(
        collStats.numBucketsClosedDueToCachePressure.load());
    stats.incNumBucketsClosedDueToTimeForward(collStats.numBucketsClosedDueToTimeForward.load());
    stats.incNumBucketsClosedDueToMemoryThreshold(
        collStats.numBucketsClosedDueToMemoryThreshold.load());
    stats.incNumBucketsClosedDueToReopening(collStats.numBucketsClosedDueToReopening.load());
    stats.incNumBucketsArchivedDueToMemoryThreshold(
        collStats.numBucketsArchivedDueToMemoryThreshold.load());
    stats.incNumBucketsArchivedDueToTimeBackward(
        collStats.numBucketsArchivedDueToTimeBackward.load());
    stats.incNumBucketsFrozen(collStats.numBucketsFrozen.load());
    stats.incNumCompressedBucketsConvertedToUnsorted(
        collStats.numCompressedBucketsConvertedToUnsorted.load());
    stats.incNumCommits(collStats.numCommits.load());
    stats.incNumWaits(collStats.numWaits.load());
    stats.incNumMeasurementsCommitted(collStats.numMeasurementsCommitted.load());
    stats.incNumBucketsReopened(collStats.numBucketsReopened.load());
    stats.incNumBucketsKeptOpenDueToLargeMeasurements(
        collStats.numBucketsKeptOpenDueToLargeMeasurements.load());
    stats.incNumBucketsFetched(collStats.numBucketsFetched.load());
    stats.incNumBucketsQueried(collStats.numBucketsQueried.load());
    stats.incNumBucketFetchesFailed(collStats.numBucketFetchesFailed.load());
    stats.incNumBucketQueriesFailed(collStats.numBucketQueriesFailed.load());
    stats.incNumBucketReopeningsFailedDueToEraMismatch(
        collStats.numBucketReopeningsFailedDueToEraMismatch.load());
    stats.incNumBucketReopeningsFailedDueToMalformedIdField(
        collStats.numBucketReopeningsFailedDueToMalformedIdField.load());
    stats.incNumBucketReopeningsFailedDueToHashCollision(
        collStats.numBucketReopeningsFailedDueToHashCollision.load());
    stats.incNumBucketReopeningsFailedDueToMarkedFrozen(
        collStats.numBucketReopeningsFailedDueToMarkedFrozen.load());
    stats.incNumBucketReopeningsFailedDueToValidator(
        collStats.numBucketReopeningsFailedDueToValidator.load());
    stats.incNumBucketReopeningsFailedDueToMarkedClosed(
        collStats.numBucketReopeningsFailedDueToMarkedClosed.load());
    stats.incNumBucketReopeningsFailedDueToMinMaxCalculation(
        collStats.numBucketReopeningsFailedDueToMinMaxCalculation.load());
    stats.incNumBucketReopeningsFailedDueToSchemaGeneration(
        collStats.numBucketReopeningsFailedDueToSchemaGeneration.load());
    stats.incNumBucketReopeningsFailedDueToUncompressedTimeColumn(
        collStats.numBucketReopeningsFailedDueToUncompressedTimeColumn.load());
    stats.incNumBucketReopeningsFailedDueToCompressionFailure(
        collStats.numBucketReopeningsFailedDueToCompressionFailure.load());
    stats.incNumBucketReopeningsFailedDueToWriteConflict(
        collStats.numBucketReopeningsFailedDueToWriteConflict.load());
    stats.incNumDuplicateBucketsReopened(collStats.numDuplicateBucketsReopened.load());
    stats.incNumBucketDocumentsTooLargeInsert(collStats.numBucketDocumentsTooLargeInsert.load());
    stats.incNumBucketDocumentsTooLargeUpdate(collStats.numBucketDocumentsTooLargeUpdate.load());
}

void addCollectionExecutionGauges(ExecutionStats& stats, const ExecutionStats& collStats) {
    stats.numActiveBuckets.fetchAndAdd(collStats.numActiveBuckets.load());
}

void removeCollectionExecutionGauges(ExecutionStats& stats, ExecutionStats& collStats) {
    // Set the collection stats to a sentinel value to prevent modifications by concurrent threads
    // which are still holding the shared pointer.
    auto collNumActiveBuckets = collStats.numActiveBuckets.swap(kNumActiveBucketsSentinel);
    stats.numActiveBuckets.fetchAndSubtract(collNumActiveBuckets);
}

}  // namespace mongo::timeseries::bucket_catalog
