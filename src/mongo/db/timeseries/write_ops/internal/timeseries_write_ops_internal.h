/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>

#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_write_util.h"

namespace mongo::timeseries::write_ops::internal {

struct WriteStageErrorAndIndex {
    Status error;
    size_t index;
};

namespace commit_result {

struct Success {};
struct ContinuableError {
    Status error;
};
struct ContinuableErrorWithAbortBatch {
    Status error;
};
struct ContinuableRetryableError {};
struct ContinuableRetryableErrorWithAbortBatch {
    Status error;
};
struct ContinuableRetryableErrorWithAbortBatchAbandonSnapshot {
    Status error;
};
struct NonContinuableError {
    Status error;
};
struct NonContinuableErrorWithAbortBatch {
    Status error;
};

using Result = std::variant<Success,
                            ContinuableError,
                            ContinuableErrorWithAbortBatch,
                            ContinuableRetryableError,
                            ContinuableRetryableErrorWithAbortBatch,
                            ContinuableRetryableErrorWithAbortBatchAbandonSnapshot,
                            NonContinuableError,
                            NonContinuableErrorWithAbortBatch>;

}  // namespace commit_result

NamespaceString ns(const mongo::write_ops::InsertCommandRequest& request);

/**
 * Writes to a time-series collection, staging and then committing writes for each measurement
 * in the request. If the write for a particular measurement fails due to a retryable error, the
 * write will be retried.
 */
void performUnorderedTimeseriesWritesWithRetries(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    size_t start,
    size_t numDocs,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry);

/**
 * Tries to perform stage and commit writes to the system.buckets collection of a time-series
 * collection. If the ordered write operation fails, falls back to a one-at-a-time unordered insert.
 * Returns the number of documents that were inserted.
 */
size_t performOrderedTimeseriesWrites(OperationContext* opCtx,
                                      const mongo::write_ops::InsertCommandRequest& request,
                                      std::vector<mongo::write_ops::WriteError>* errors,
                                      boost::optional<repl::OpTime>* opTime,
                                      boost::optional<OID>* electionId,
                                      bool* containsRetry);

/**
 * Given vectors of InsertCommandRequests and UpdateCommandRequests, performs the actual storage
 * writes to the underlying system.buckets collection of a time-series collection.
 */
Status performAtomicTimeseriesWrites(
    OperationContext* opCtx,
    const std::vector<mongo::write_ops::InsertCommandRequest>& insertOps,
    const std::vector<mongo::write_ops::UpdateCommandRequest>& updateOps);

/**
 * Given a WriteBatch, will commit and finish the write batch and perform a write to the underlying
 * system.buckets collection for a time-series collection.
 * Returns whether the request can continue.
 */
bool commitTimeseriesBucket(OperationContext* opCtx,
                            std::shared_ptr<bucket_catalog::WriteBatch> batch,
                            size_t start,
                            size_t index,
                            std::vector<StmtId>&& stmtIds,
                            std::vector<mongo::write_ops::WriteError>* errors,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId,
                            std::vector<size_t>* docsToRetry,
                            absl::flat_hash_map<int, int>& retryAttemptsForDup,
                            const mongo::write_ops::InsertCommandRequest& request);

/**
 * Given a WriteBatch, will commit the write batch and perform a write to the
 * underlying system.buckets collection for a time-series collection.
 *
 * Returns an struct indicating to the caller whether the commit was successful or if there was an
 * error, and if there was an error, what clean up work to perform (aborting the write batch, etc).
 */
commit_result::Result commitTimeseriesBucketForBatch(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    size_t start,
    std::vector<mongo::write_ops::WriteError>& errors,
    boost::optional<repl::OpTime>& opTime,
    boost::optional<OID>& electionId,
    std::vector<size_t>& docsToRetry,
    absl::flat_hash_map<int, int>& retryAttemptsForDup,
    const mongo::write_ops::InsertCommandRequest& request);

/**
 * Given a batch of user measurements for a collection that does not have a metaField value, returns
 * a BatchedInsertContext for all of the user measurements. This is a special case - all time-series
 * without a metafield value are grouped within the same batch.
 *
 * Passes through the inputted measurements twice, once to record the index of the measurement in
 * the original user batch for error reporting, and then again to sort the measurements based on
 * their time field.
 *
 * This is slightly more efficient and requires fewer maps/data structures than the metaField
 * variant, because we do not need to split up the measurements into different batches according to
 * their metaField value.
 */
std::vector<bucket_catalog::BatchedInsertContext> buildBatchedInsertContextsNoMetaField(
    const bucket_catalog::BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    bucket_catalog::ExecutionStatsController& stats,
    tracking::Context& trackingContext,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);


/**
 * Given a batch of user measurements for a collection that does have a metaField value, returns a
 * vector of BatchedInsertContexts with each BatchedInsertContext storing the measurements for a
 * particular metaField value.
 *
 * Passes through the inputted measurements twice, once to record the index of the measurement in
 * the original user batch for error reporting, and then again to sort the measurements based on
 * their time field.
 */
std::vector<bucket_catalog::BatchedInsertContext> buildBatchedInsertContextsWithMetaField(
    const bucket_catalog::BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    bucket_catalog::ExecutionStatsController& stats,
    tracking::Context& trackingContext,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Given a set of measurements, splits up the measurements into batches based on the metaField.
 * Returns a vector of BatchedInsertContext where each BatchedInsertContext will contain the batch
 * of measurements for a particular metaField value, sorted on time, as well as other bucket-level
 * metadata.
 *
 * If the time-series collection has no metaField value, then all of the measurements will be
 * batched into one BatchedInsertContext.
 *
 * Any inserted measurements that are malformed (i.e. missing the proper time field) will have their
 * error Status and their index recorded in errorsAndIndices. Callers should check that no errors
 * occured while processing measurements by checking that errorsAndIndices is empty.
 */
std::vector<bucket_catalog::BatchedInsertContext> buildBatchedInsertContexts(
    bucket_catalog::BucketCatalog& bucketCatalog,
    const UUID& collectionUUID,
    const TimeseriesOptions& timeseriesOptions,
    const std::vector<BSONObj>& userMeasurementsBatch,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Given a BatchedInsertContext, will stage writes to eligible buckets until all measurements have
 * been staged into an eligible bucket. When there is any RolloverReason that isn't kNone when
 * attempting to stage a measurement into a bucket, the function will find another eligible
 * buckets until all measurements are inserted.
 */
TimeseriesWriteBatches stageInsertBatch(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const OperationId& opId,
    const StringDataComparator* comparator,
    uint64_t storageCacheSizeBytes,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    bucket_catalog::BatchedInsertContext& batch);

/**
 * Stages compatible measurements into appropriate bucket(s).
 * Returns a non-success status if any measurements are malformed, and further
 * returns the index into 'userMeasurementsBatch' of each failure in 'errorsAndIndices'.
 * Returns a write batch per bucket that the measurements are staged to.
 * 'earlyReturnOnError' decides whether or not staging should happen in the case of any malformed
 * measurements.
 */
StatusWith<TimeseriesWriteBatches> prepareInsertsToBuckets(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const TimeseriesOptions& timeseriesOptions,
    OperationId opId,
    const StringDataComparator* comparator,
    uint64_t storageCacheSizeBytes,
    bool earlyReturnOnError,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    const std::vector<BSONObj>& userMeasurementsBatch,
    std::vector<WriteStageErrorAndIndex>& errorsAndIndices);

/**
 * Rewrites the indices for each write batch's userBatchIndices vector to reflect the measurement's
 * index into the original user batch of measurements relative to the original batch's startIndex,
 * from the indices into the filtered subset. This also populates each write batch's vector of
 * statement ids.
 */
void rewriteIndicesForSubsetOfBatch(OperationContext* opCtx,
                                    const mongo::write_ops::InsertCommandRequest& request,
                                    size_t startIndex,
                                    TimeseriesWriteBatches& writeBatches,
                                    std::vector<size_t>& originalIndices);
/**
 * Processes the errors in `errorsAndIndices` and populates the `errors` vector with them, tying
 * each error to the index of the measurement that caused it in the original user batch of
 * measurements.
 */
void processErrorsForSubsetOfBatch(OperationContext* opCtx,
                                   const std::vector<WriteStageErrorAndIndex>& errorsAndIndices,
                                   const std::vector<size_t>& originalIndices,
                                   std::vector<mongo::write_ops::WriteError>* errors);

}  // namespace mongo::timeseries::write_ops::internal
