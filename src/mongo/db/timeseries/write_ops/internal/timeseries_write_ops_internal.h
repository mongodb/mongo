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

#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"

#include <boost/optional/optional.hpp>

namespace mongo::timeseries::write_ops::internal {

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
    const CollectionPreConditions& preConditions,
    size_t start,
    size_t numDocs,
    bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
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
                                      const CollectionPreConditions& preConditions,
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
 * Given a WriteBatch, will commit the write batch and perform a write to the
 * underlying system.buckets collection for a time-series collection.
 *
 * Returns an struct indicating to the caller whether the commit was successful or if there was an
 * error, and if there was an error, what clean up work to perform (aborting the write batch, etc).
 */
commit_result::Result commitTimeseriesBucketForBatch(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const mongo::write_ops::InsertCommandRequest& request,
    std::vector<mongo::write_ops::WriteError>& errors,
    boost::optional<repl::OpTime>& opTime,
    boost::optional<OID>& electionId,
    absl::flat_hash_map<int, int>& retryAttemptsForDup);

/**
 * Rewrites the indices for each write batch's userBatchIndices vector to reflect the measurement's
 * index into the original user batch of measurements relative to the original batch's startIndex,
 * from the indices into the filtered subset. This also populates each write batch's vector of
 * statement ids.
 */
void rewriteIndicesForSubsetOfBatch(OperationContext* opCtx,
                                    const mongo::write_ops::InsertCommandRequest& request,
                                    const std::vector<size_t>& originalIndices,
                                    bucket_catalog::TimeseriesWriteBatches& writeBatches);
/**
 * Processes the errors in `errorsAndIndices` and populates the `errors` vector with them, tying
 * each error to the index of the measurement that caused it in the original user batch of
 * measurements.
 */
void processErrorsForSubsetOfBatch(
    OperationContext* opCtx,
    const std::vector<bucket_catalog::WriteStageErrorAndIndex>& errorsAndIndices,
    const std::vector<size_t>& originalIndices,
    std::vector<mongo::write_ops::WriteError>* errors);

/**
 * Stages unordered writes. If an error is encountered while trying to stage a given measurement,
 * will continue staging and committing subsequent measurements if doing so is possible.
 *
 * Stages measurements in the range [startIndex, startIndex + numDocsToStage).
 *
 * On success, returns WriteBatches with the staged writes.
 */
bucket_catalog::TimeseriesWriteBatches stageUnorderedWritesToBucketCatalog(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    size_t startIndex,
    size_t numDocsToStage,
    bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    boost::optional<UUID>& optUuid,
    std::vector<mongo::write_ops::WriteError>* errors);

/**
 * Stages unordered writes. Same as above, but handles retryable writes that have already been
 * executed and also any documents that need to be retried due to continuable errors.
 */
bucket_catalog::TimeseriesWriteBatches stageUnorderedWritesToBucketCatalogUnoptimized(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    size_t startIndex,
    size_t numDocsToStage,
    bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    const std::vector<size_t>& docsToRetry,
    boost::optional<UUID>& optUuid,
    std::vector<mongo::write_ops::WriteError>* errors);

}  // namespace mongo::timeseries::write_ops::internal
