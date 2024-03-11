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

#include "mongo/db/feature_flag.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo::timeseries::bucket_catalog {

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

void ExecutionStatsController::incNumBucketsClosedDueToTimeBackward(long long increment) {
    _collectionStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
    _globalStats->numBucketsClosedDueToTimeBackward.fetchAndAddRelaxed(increment);
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

void ExecutionStatsController::incNumMeasurementsGroupCommitted(long long increment) {
    _collectionStats->numMeasurementsGroupCommitted.fetchAndAddRelaxed(increment);
    _globalStats->numMeasurementsGroupCommitted.fetchAndAddRelaxed(increment);
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

void ExecutionStatsController::incNumBucketReopeningsFailed(long long increment) {
    _collectionStats->numBucketReopeningsFailed.fetchAndAddRelaxed(increment);
    _globalStats->numBucketReopeningsFailed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumDuplicateBucketsReopened(long long increment) {
    _collectionStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
    _globalStats->numDuplicateBucketsReopened.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBytesUncompressed(long long increment) {
    _collectionStats->numBytesUncompressed.fetchAndAddRelaxed(increment);
    _globalStats->numBytesUncompressed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumBytesCompressed(long long increment) {
    _collectionStats->numBytesCompressed.fetchAndAddRelaxed(increment);
    _globalStats->numBytesCompressed.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumSubObjCompressionRestart(long long increment) {
    _collectionStats->numSubObjCompressionRestart.fetchAndAddRelaxed(increment);
    _globalStats->numSubObjCompressionRestart.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumCompressedBuckets(long long increment) {
    _collectionStats->numCompressedBuckets.fetchAndAddRelaxed(increment);
    _globalStats->numCompressedBuckets.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumUncompressedBuckets(long long increment) {
    _collectionStats->numUncompressedBuckets.fetchAndAddRelaxed(increment);
    _globalStats->numUncompressedBuckets.fetchAndAddRelaxed(increment);
}

void ExecutionStatsController::incNumFailedDecompressBuckets(long long increment) {
    _collectionStats->numFailedDecompressBuckets.fetchAndAddRelaxed(increment);
    _globalStats->numFailedDecompressBuckets.fetchAndAddRelaxed(increment);
}

void appendExecutionStatsToBuilder(const ExecutionStats& stats, BSONObjBuilder& builder) {
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
    builder.appendNumber("numBucketsClosedDueToTimeBackward",
                         stats.numBucketsClosedDueToTimeBackward.load());
    builder.appendNumber("numBucketsClosedDueToMemoryThreshold",
                         stats.numBucketsClosedDueToMemoryThreshold.load());

    auto commits = stats.numCommits.load();
    builder.appendNumber("numCommits", commits);
    builder.appendNumber("numMeasurementsGroupCommitted",
                         stats.numMeasurementsGroupCommitted.load());
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
    builder.appendNumber("numBucketReopeningsFailed", stats.numBucketReopeningsFailed.load());
    builder.appendNumber("numDuplicateBucketsReopened", stats.numDuplicateBucketsReopened.load());

    if (feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }

    // TODO(SERVER-70605): Remove these.
    builder.appendNumber("numBytesUncompressed", stats.numBytesUncompressed.load());
    builder.appendNumber("numBytesCompressed", stats.numBytesCompressed.load());
    builder.appendNumber("numSubObjCompressionRestart", stats.numSubObjCompressionRestart.load());
    builder.appendNumber("numCompressedBuckets", stats.numCompressedBuckets.load());
    builder.appendNumber("numUncompressedBuckets", stats.numUncompressedBuckets.load());
    builder.appendNumber("numFailedDecompressBuckets", stats.numFailedDecompressBuckets.load());
}

void addCollectionExecutionStats(ExecutionStatsController& stats, const ExecutionStats& collStats) {
    stats.incNumBucketInserts(collStats.numBucketInserts.load());
    stats.incNumBucketUpdates(collStats.numBucketUpdates.load());
    stats.incNumBucketsOpenedDueToMetadata(collStats.numBucketsOpenedDueToMetadata.load());
    stats.incNumBucketsClosedDueToCount(collStats.numBucketsClosedDueToCount.load());
    stats.incNumBucketsClosedDueToSchemaChange(collStats.numBucketsClosedDueToSchemaChange.load());
    stats.incNumBucketsClosedDueToSize(collStats.numBucketsClosedDueToSize.load());
    stats.incNumBucketsClosedDueToCachePressure(
        collStats.numBucketsClosedDueToCachePressure.load());
    stats.incNumBucketsClosedDueToTimeForward(collStats.numBucketsClosedDueToTimeForward.load());
    stats.incNumBucketsClosedDueToTimeBackward(collStats.numBucketsClosedDueToTimeBackward.load());
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
    stats.incNumMeasurementsGroupCommitted(collStats.numMeasurementsGroupCommitted.load());
    stats.incNumWaits(collStats.numWaits.load());
    stats.incNumMeasurementsCommitted(collStats.numMeasurementsCommitted.load());
    stats.incNumBucketsReopened(collStats.numBucketsReopened.load());
    stats.incNumBucketsKeptOpenDueToLargeMeasurements(
        collStats.numBucketsKeptOpenDueToLargeMeasurements.load());
    stats.incNumBucketsFetched(collStats.numBucketsFetched.load());
    stats.incNumBucketsQueried(collStats.numBucketsQueried.load());
    stats.incNumBucketFetchesFailed(collStats.numBucketFetchesFailed.load());
    stats.incNumBucketQueriesFailed(collStats.numBucketQueriesFailed.load());
    stats.incNumBucketReopeningsFailed(collStats.numBucketReopeningsFailed.load());
    stats.incNumDuplicateBucketsReopened(collStats.numDuplicateBucketsReopened.load());

    // TODO(SERVER-70605): Remove these.
    stats.incNumBytesUncompressed(collStats.numBytesUncompressed.load());
    stats.incNumBytesCompressed(collStats.numBytesCompressed.load());
    stats.incNumSubObjCompressionRestart(collStats.numSubObjCompressionRestart.load());
    stats.incNumCompressedBuckets(collStats.numCompressedBuckets.load());
    stats.incNumUncompressedBuckets(collStats.numUncompressedBuckets.load());
    stats.incNumFailedDecompressBuckets(collStats.numFailedDecompressBuckets.load());
}

}  // namespace mongo::timeseries::bucket_catalog
