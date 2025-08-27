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

#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/collection_pre_conditions_util.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_write_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::write_ops::internal {

namespace {

MONGO_FAIL_POINT_DEFINE(failAtomicTimeseriesWrites);
MONGO_FAIL_POINT_DEFINE(failUnorderedTimeseriesInsert);
MONGO_FAIL_POINT_DEFINE(hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection);
MONGO_FAIL_POINT_DEFINE(hangCommitTimeseriesBucketBeforeCheckingTimeseriesCollection);
MONGO_FAIL_POINT_DEFINE(hangCommitTimeseriesBucketsAtomicallyBeforeCheckingTimeseriesCollection);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeCommit);
MONGO_FAIL_POINT_DEFINE(hangTimeseriesInsertBeforeWrite);


using TimeseriesBatches =
    std::vector<std::pair<std::shared_ptr<bucket_catalog::WriteBatch>, size_t>>;

struct TimeseriesSingleWriteResult {
    StatusWith<SingleWriteResult> result;
    bool canContinue = true;
};

enum class StageWritesStatus {
    kSuccess,
    kContainsRetry,
    kCollectionAcquisitionError,
    kStagingError,
};

bool isTimeseriesWriteRetryable(OperationContext* opCtx) {
    return (opCtx->getTxnNumber() && !opCtx->inMultiDocumentTransaction());
}

inline uint64_t getStorageCacheSizeBytes(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}

inline void populateError(OperationContext* opCtx,
                          size_t index,
                          Status errorStatus,
                          std::vector<mongo::write_ops::WriteError>* errors) {
    if (auto error = write_ops_exec::generateError(opCtx, errorStatus, index, errors->size())) {
        invariant(errorStatus.code() != ErrorCodes::WriteConflict);
        errors->emplace_back(std::move(*error));
    }
}
TimeseriesSingleWriteResult getTimeseriesSingleWriteResult(
    write_ops_exec::WriteResult&& reply, const mongo::write_ops::InsertCommandRequest& request) {
    invariant(reply.results.size() == 1,
              str::stream() << "Unexpected number of results (" << reply.results.size()
                            << ") for insert on time-series collection "
                            << internal::ns(request).toStringForErrorMsg());

    return {std::move(reply.results[0]), reply.canContinue};
}

boost::optional<std::pair<Status, bool>> checkFailUnorderedTimeseriesInsertFailPoint(
    const bucket_catalog::BucketMetadata& metadata) {
    bool canContinue = true;
    if (MONGO_unlikely(failUnorderedTimeseriesInsert.shouldFail(
            [&metadata, &canContinue](const BSONObj& data) {
                BSONElementComparator comp(BSONElementComparator::FieldNamesMode::kIgnore, nullptr);
                if (auto continueElem = data["canContinue"]) {
                    canContinue = data["canContinue"].trueValue();
                }
                return comp.compare(data["metadata"], metadata.element()) == 0;
            }))) {
        return std::make_pair(Status(ErrorCodes::FailPointEnabled,
                                     "Failed unordered time-series insert due to "
                                     "failUnorderedTimeseriesInsert fail point"),
                              canContinue);
    }
    return boost::none;
}

/**
 * Returns true if we are both performing retryable time-series writes and there is at least one
 * measurement in the request's batch of measurements that has already been executed. Returns false
 * otherwise.
 */
bool batchContainsExecutedStatements(OperationContext* opCtx,
                                     const mongo::write_ops::InsertCommandRequest& request,
                                     size_t start,
                                     size_t numDocs) {
    if (isTimeseriesWriteRetryable(opCtx)) {
        for (size_t index = 0; index < numDocs; index++) {
            auto stmtId = request.getStmtIds() ? request.getStmtIds()->at(start + index)
                                               : request.getStmtId().value_or(0) + start + index;
            if (TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, stmtId)) {
                return true;
            }
        }
    }
    return false;
}

/**
 * Populates 'filteredBatch' and 'filteredIndices' with only the measurements (and corresponding
 * indices) in request's batch of measurements that have not been already executed.
 */
void filterOutExecutedMeasurements(OperationContext* opCtx,
                                   const mongo::write_ops::InsertCommandRequest& request,
                                   size_t startIndex,
                                   size_t numDocsToStage,
                                   const std::vector<size_t>& indices,
                                   std::vector<BSONObj>& filteredBatch,
                                   std::vector<size_t>& filteredIndices) {
    auto& originalUserMeasurementBatch = request.getDocuments();
    std::function<void(size_t)> filterOutExecutedMeasurement = [&](size_t index) {
        invariant(index < request.getDocuments().size());
        auto stmtId = request.getStmtIds() ? request.getStmtIds()->at(index)
                                           : request.getStmtId().value_or(0) + index;
        if (!TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, stmtId)) {
            filteredBatch.emplace_back(originalUserMeasurementBatch.at(index));
            filteredIndices.emplace_back(index);
        } else {
            RetryableWritesStats::get(opCtx)->incrementRetriedStatementsCount();
        }
    };
    std::function<void(size_t)> filterOutSuccessfullyInsertedMeasurement = [&](size_t index) {
        invariant(index < request.getDocuments().size());
        filteredBatch.emplace_back(originalUserMeasurementBatch.at(index));
        filteredIndices.emplace_back(index);
    };

    auto filterMeasurement = (isTimeseriesWriteRetryable(opCtx))
        ? filterOutExecutedMeasurement
        : filterOutSuccessfullyInsertedMeasurement;

    if (!indices.empty()) {
        std::for_each(indices.begin(), indices.end(), filterMeasurement);
    } else {
        for (size_t index = startIndex; index < startIndex + numDocsToStage; index++) {
            filterMeasurement(index);
        }
    }
}

/**
 * Writes a new time-series bucket to storage.
 */
TimeseriesSingleWriteResult performTimeseriesInsertFromBatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const mongo::write_ops::InsertCommandRequest& request,
    std::shared_ptr<bucket_catalog::WriteBatch> batch) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(batch->bucketKey.metadata)) {
        return {status->first, status->second};
    }
    return getTimeseriesSingleWriteResult(
        write_ops_exec::performInserts(opCtx,
                                       write_ops_utils::makeTimeseriesInsertOpFromBatch(batch, nss),
                                       /*preConditions=*/boost::none,
                                       OperationSource::kTimeseriesInsert),
        request);
}

/**
 * Persists an update to storage of an existing time-series bucket.
 */
TimeseriesSingleWriteResult performTimeseriesUpdate(
    OperationContext* opCtx,
    const bucket_catalog::BucketMetadata& metadata,
    const mongo::write_ops::UpdateCommandRequest& op,
    const mongo::write_ops::InsertCommandRequest& request) {
    if (auto status = checkFailUnorderedTimeseriesInsertFailPoint(metadata)) {
        return {status->first, status->second};
    }

    return getTimeseriesSingleWriteResult(
        write_ops_exec::performUpdates(
            opCtx, op, /* preConditions=*/boost::none, OperationSource::kTimeseriesInsert),
        request);
}

CollectionAcquisition acquireAndValidateBucketsCollection(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionReq, LockMode mode) {

    auto [bucketsAcq, _] =
        timeseries::acquireCollectionWithBucketsLookup(opCtx, acquisitionReq, mode);
    timeseries::assertTimeseriesBucketsCollection(bucketsAcq.getCollectionPtr().get());
    return bucketsAcq;
}

/**
 * Throws if compression fails for any reason.
 */
void compressUncompressedBucketOnReopen(OperationContext* opCtx,
                                        const bucket_catalog::BucketId& bucketId,
                                        const NamespaceString& nss,
                                        StringData timeFieldName) {
    bool validateCompression = gValidateTimeseriesCompression.load();

    auto bucketCompressionFunc = [&](const BSONObj& bucketDoc) -> boost::optional<BSONObj> {
        auto compressed =
            timeseries::compressBucket(bucketDoc, timeFieldName, nss, validateCompression);

        if (!compressed.compressedBucket) {
            uassert(timeseries::BucketCompressionFailure(
                        bucketId.collectionUUID, bucketId.oid, bucketId.keySignature),
                    "Time-series compression failed on reopened bucket",
                    compressed.compressedBucket);
        }

        return compressed.compressedBucket;
    };

    mongo::write_ops::UpdateModification u(std::move(bucketCompressionFunc));
    mongo::write_ops::UpdateOpEntry update(BSON("_id" << bucketId.oid), std::move(u));
    invariant(!update.getMulti(), bucketId.oid.toString());
    invariant(!update.getUpsert(), bucketId.oid.toString());

    mongo::write_ops::UpdateCommandRequest compressionOp(nss, {update});
    mongo::write_ops::WriteCommandRequestBase base;
    // The schema validation configured in the bucket collection is intended for direct
    // operations by end users and is not applicable here.
    base.setBypassDocumentValidation(true);
    // Timeseries compression operation is not a user operation and should not use a
    // statement id from any user op. Set to Uninitialized to bypass.
    base.setStmtIds(std::vector<StmtId>{kUninitializedStmtId});
    compressionOp.setWriteCommandRequestBase(std::move(base));
    auto result = write_ops_exec::performUpdates(
        opCtx, compressionOp, /*preConditions=*/boost::none, OperationSource::kTimeseriesInsert);
    invariant(result.results.size() == 1,
              str::stream() << "Unexpected number of results (" << result.results.size()
                            << ") for update on time-series collection "
                            << nss.toStringForErrorMsg());
}

// For sharded time-series collections, we need to use the granularity from the config
// server (through shard filtering information) as the source of truth for the current
// granularity value, due to the possible inconsistency in the process of granularity
// updates.
void rebuildOptionsWithGranularityFromConfigServer(OperationContext* opCtx,
                                                   const NamespaceString& bucketsNs,
                                                   TimeseriesOptions& timeSeriesOptions) {
    auto collDesc = CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, bucketsNs)
                        ->getCollectionDescription(opCtx);
    if (collDesc.isSharded()) {
        tassert(6102801,
                "Sharded time-series buckets collection is missing time-series fields",
                collDesc.getTimeseriesFields());
        auto granularity = collDesc.getTimeseriesFields()->getGranularity();
        auto bucketMaxSpanSeconds = collDesc.getTimeseriesFields()->getBucketMaxSpanSeconds();

        if (granularity) {
            timeSeriesOptions.setGranularity(granularity.get());
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*granularity));
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(*granularity));
        } else if (!bucketMaxSpanSeconds) {
            timeSeriesOptions.setGranularity(BucketGranularityEnum::Seconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(
                timeseries::getMaxSpanSecondsFromGranularity(*timeSeriesOptions.getGranularity()));
            timeSeriesOptions.setBucketRoundingSeconds(
                timeseries::getBucketRoundingSecondsFromGranularity(
                    *timeSeriesOptions.getGranularity()));
        } else {
            invariant(bucketMaxSpanSeconds);
            timeSeriesOptions.setBucketMaxSpanSeconds(bucketMaxSpanSeconds);

            auto bucketRoundingSeconds = collDesc.getTimeseriesFields()->getBucketRoundingSeconds();
            invariant(bucketRoundingSeconds);
            timeSeriesOptions.setBucketRoundingSeconds(bucketRoundingSeconds);
        }
    }
}

void sortBatchesToCommit(bucket_catalog::TimeseriesWriteBatches& batches) {
    std::sort(batches.begin(), batches.end(), [](auto left, auto right) {
        return left.get()->bucketId.oid < right.get()->bucketId.oid;
    });
}

Status commitTimeseriesBucketsAtomically(OperationContext* opCtx,
                                         const mongo::write_ops::InsertCommandRequest& request,
                                         bucket_catalog::TimeseriesWriteBatches& batches,
                                         boost::optional<repl::OpTime>* opTime,
                                         boost::optional<OID>* electionId) {
    hangCommitTimeseriesBucketsAtomicallyBeforeCheckingTimeseriesCollection.pauseWhileSet();

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    if (batches.empty()) {
        return Status::OK();
    }

    sortBatchesToCommit(batches);

    Status abortStatus = Status::OK();
    ScopeGuard batchGuard{[&] {
        for (auto& batch : batches) {
            if (batch.get()) {
                abort(bucketCatalog, batch, abortStatus);
            }
        }
    }};

    try {
        std::vector<mongo::write_ops::InsertCommandRequest> insertOps;
        std::vector<mongo::write_ops::UpdateCommandRequest> updateOps;

        // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
        // Collection instances remain valid, and the collator is not invalidated.
        auto catalog = CollectionCatalog::get(opCtx);
        NamespaceString nss;
        const CollatorInterface* collator = nullptr;

        try {
            // The associated collection must be acquired before we check for the presence of
            // buckets collection. This ensures that a potential ShardVersion mismatch can be
            // detected, before checking for other errors. Moreover, since e.g. 'prepareCommit()'
            // might block waiting for other batches to complete, limiting the scope of the
            // collectionAcquisition is necessary to prevent deadlocks due to ticket exhaustion.
            const auto bucketsAq = acquireAndValidateBucketsCollection(
                opCtx,
                CollectionAcquisitionRequest::fromOpCtx(
                    opCtx, internal::ns(request), AcquisitionPrerequisites::kRead),
                MODE_IS);
            nss = bucketsAq.nss();
            collator = bucketsAq.getCollectionPtr()->getDefaultCollator();
        } catch (const DBException& ex) {
            if (ex.code() != ErrorCodes::StaleDbVersion &&
                !ErrorCodes::isStaleShardVersionError(ex)) {
                throw;
            }
            // The unsuccessful ordered timeseries insert will resolve into a sequence of unordered
            // inserts. Therefore, do not set the failed operation status on the operation sharding
            // state here, as it will be set during the unordered insert attempt.
            abortStatus = ex.toStatus();
            return ex.toStatus();
        }

        for (auto& batch : batches) {
            auto prepareCommitStatus =
                bucket_catalog::prepareCommit(bucketCatalog, batch, collator);
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return prepareCommitStatus;
            }

            write_ops_utils::makeWriteRequestFromBatch(opCtx, batch, nss, &insertOps, &updateOps);
        }

        hangTimeseriesInsertBeforeWrite.pauseWhileSet();

        auto result = internal::performAtomicTimeseriesWrites(opCtx, insertOps, updateOps);

        if (!result.isOK()) {
            if (result.code() == ErrorCodes::DuplicateKey) {
                bucket_catalog::resetBucketOIDCounter();
            }
            abortStatus = result;
            return result;
        }

        timeseries::getOpTimeAndElectionId(opCtx, opTime, electionId);

        for (auto& batch : batches) {
            bucket_catalog::finish(bucketCatalog, batch);
            batch.reset();
        }
    } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
        bucket_catalog::freeze(
            bucketCatalog,
            bucket_catalog::BucketId{ex->collectionUUID(), ex->bucketId(), ex->keySignature()});
        LOGV2_WARNING(
            8793900,
            "Encountered corrupt bucket while performing insert, will retry on a new bucket",
            "bucketId"_attr = ex->bucketId());
        abortStatus = ex.toStatus();
        return ex.toStatus();
    } catch (...) {
        abortStatus = exceptionToStatus();
        throw;
    }

    batchGuard.dismiss();
    return Status::OK();
}

void populateDocsToRetryFromWriteBatch(
    std::shared_ptr<mongo::timeseries::bucket_catalog::WriteBatch> batch,
    std::vector<size_t>& docsToRetry) {
    for (auto index : batch->userBatchIndices) {
        docsToRetry.emplace_back(index);
    };
}

void populateErrorsFromWriteBatches(OperationContext* opCtx,
                                    bucket_catalog::TimeseriesWriteBatches& batches,
                                    size_t startIndex,
                                    size_t endIndex,
                                    Status errorStatus,
                                    std::vector<mongo::write_ops::WriteError>& errors) {
    invariant(endIndex < batches.size() && startIndex >= 0 && startIndex <= endIndex);
    for (auto batchIndex = startIndex; batchIndex <= endIndex; batchIndex++) {
        auto batch = batches[batchIndex];
        for (auto index : batch->userBatchIndices) {
            populateError(opCtx, static_cast<size_t>(index), errorStatus, &errors);
        }
    }
}

/**
 * Handles the result of attempting to commit a WriteBatch as part of an unordered insert. This
 * function will handle populating the errors vector, vector of documents that need to be retried,
 * and aborting WriteBatches as needed.
 *
 * If a non-continuable error was encountered, populates the errors vector with the indices of all
 * measurements for all batches after, and including, the batch that encountered a non-continuable
 * error. Sets `canContinue` to false in this case.
 */
void processUnorderedCommitResult(OperationContext* opCtx,
                                  commit_result::Result commitResult,
                                  bucket_catalog::TimeseriesWriteBatches& batches,
                                  size_t batchIndex,
                                  std::vector<size_t>& docsToRetry,
                                  std::vector<mongo::write_ops::WriteError>& errors,
                                  boost::optional<repl::OpTime>& opTime,
                                  boost::optional<OID>& electionId,
                                  bool* canContinue) {

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    auto batch = batches[batchIndex];

    auto finishBatch = [&]() {
        timeseries::getOpTimeAndElectionId(opCtx, &opTime, &electionId);
        bucket_catalog::finish(bucketCatalog, batch);
    };
    auto addDocsToRetry = [&]() {
        populateDocsToRetryFromWriteBatch(batch, docsToRetry);
    };
    auto abortBatch = [&](Status errorStatus) {
        abort(bucketCatalog, batch, errorStatus);
    };
    auto abandonSnapshot = [&]() {
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
    };
    auto populateErrorsForBatch = [&](Status errorStatus) {
        populateErrorsFromWriteBatches(opCtx, batches, batchIndex, batchIndex, errorStatus, errors);
    };
    auto handleNonContinuableError = [&](Status errorStatus) {
        *canContinue = false;
        populateErrorsFromWriteBatches(
            opCtx, batches, batchIndex, batches.size() - 1, errorStatus, errors);
    };

    visit(OverloadedVisitor{
              [&](const commit_result::Success&) { finishBatch(); },
              [&](const commit_result::ContinuableError& result) {
                  populateErrorsForBatch(result.error);
              },
              [&](const commit_result::ContinuableErrorWithAbortBatch& result) {
                  populateErrorsForBatch(result.error);
                  abortBatch(result.error);
              },
              [&](const commit_result::ContinuableRetryableError&) { addDocsToRetry(); },
              [&](const commit_result::ContinuableRetryableErrorWithAbortBatch& result) {
                  addDocsToRetry();
                  abortBatch(result.error);
              },
              [&](const commit_result::ContinuableRetryableErrorWithAbortBatchAbandonSnapshot&
                      result) {
                  addDocsToRetry();
                  abortBatch(result.error);
                  abandonSnapshot();
              },
              [&](const commit_result::NonContinuableError& result) {
                  handleNonContinuableError(result.error);
              },
              [&](const commit_result::NonContinuableErrorWithAbortBatch& result) {
                  handleNonContinuableError(result.error);
                  abortBatch(result.error);
              }},
          commitResult);
};

/**
 * If no statement has been retried in 'request', fills stmtIds with stmtIds based on 'request'
 * which can explicitly specify all StmtIds, the begin StmtId, or none at all (implied start from
 * zero). Otherwise, sets 'containsRetry' to true and returns empty 'stmtIds'.
 */
void getStmtIdVectorFromRequest(OperationContext* opCtx,
                                const mongo::write_ops::InsertCommandRequest& request,
                                bool* containsRetry,
                                std::vector<StmtId>& stmtIds) {
    if (isTimeseriesWriteRetryable(opCtx)) {
        std::vector<StmtId> stmtIdsVec(request.getDocuments().size());
        // The driver can specify all stmtIds, the begin index, or none.
        if (request.getStmtIds()) {
            stmtIdsVec = request.getStmtIds().get();
        } else {
            // Fill stmtIdsVec with 0 ... N-1 if no stmtId was provided by the driver.
            // Otherwise, begin at the index provided.
            std::iota(stmtIdsVec.begin(), stmtIdsVec.end(), request.getStmtId().value_or(0));
        }

        for (auto& stmtId : stmtIdsVec) {
            if (TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, stmtId)) {
                *containsRetry = true;
                return;
            }
        }

        stmtIds = std::move(stmtIdsVec);
    }
}

/**
 * Stages ordered writes.
 * On success, returns WriteBatches with the staged writes.
 * Returns empty WriteBatches if:
    - Collection acquisition fails, or
    - Retryable writes have been executed, or
    - Staging the measurements encounters an error
 * Sets 'stageStatus' accordingly.
 */
bucket_catalog::TimeseriesWriteBatches stageOrderedWritesToBucketCatalog(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    std::vector<mongo::write_ops::WriteError>* errors,
    StageWritesStatus& stageStatus) {
    invariant(errors->empty());
    hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection.pauseWhileSet();

    stageStatus = StageWritesStatus::kSuccess;

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    auto& measurementDocs = request.getDocuments();

    // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
    // Collection instances remain valid, and the bucketsColl is not invalidated.
    std::shared_ptr<const CollectionCatalog> catalog;
    const Collection* bucketsColl = nullptr;

    Status collectionAcquisitionStatus = Status::OK();
    TimeseriesOptions timeseriesOptions;

    try {
        // It must be ensured that the CollectionShardingState remains consistent while rebuilding
        // the timeseriesOptions. However, the associated collection must be acquired before
        // we check for the presence of buckets collection. This ensures that a potential
        // ShardVersion mismatch can be detected, before checking for other errors.
        const auto bucketsAcq = acquireAndValidateBucketsCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                    internal::ns(request),
                                                    AcquisitionPrerequisites::kRead,
                                                    preConditions.expectedUUID()),
            MODE_IS);
        CollectionPreConditions::checkAcquisitionAgainstPreConditions(
            opCtx, preConditions, bucketsAcq);

        // We want to ensure that the catalog instance after the scope of the acquisition is the
        // same as before the acquisition. Acquiring the collection involves stashing the
        // current catalog instance, so assigning the catalog in scope of the try block ensures
        // that we have a consistent catalog with the acquisition.
        catalog = CollectionCatalog::get(opCtx);

        bucketsColl = bucketsAcq.getCollectionPtr().get();
        // Process timeseriesOptions
        timeseriesOptions = bucketsColl->getTimeseriesOptions().get();
        rebuildOptionsWithGranularityFromConfigServer(opCtx, bucketsAcq.nss(), timeseriesOptions);
    } catch (const DBException& ex) {
        if (ex.code() != ErrorCodes::StaleDbVersion && !ErrorCodes::isStaleShardVersionError(ex)) {
            throw;
        }

        collectionAcquisitionStatus = ex.toStatus();

        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(ex.toStatus());
    }

    if (!collectionAcquisitionStatus.isOK()) {
        populateError(opCtx, /*index=*/0, collectionAcquisitionStatus, errors);
        stageStatus = StageWritesStatus::kCollectionAcquisitionError;
        return {};
    }

    // It is a layering violation to have the bucket catalog be privy to the details of writing
    // out buckets with write_ops_exec functions. This callable function is a workaround in the
    // case of an uncompressed bucket that gets reopened. The bucket catalog can blindly call
    // this function handed down from write_ops_exec to write out the bucket as compressed.
    // This can be safely removed when query-based reopening is removed.
    bucket_catalog::CompressAndWriteBucketFunc compressAndWriteBucketFunc =
        compressUncompressedBucketOnReopen;

    auto storageCacheSizeBytes = getStorageCacheSizeBytes(opCtx);

    // Early exit before staging if any statements in the user's batch have been retried. Fallback
    // to unordered one-by-one to handle this.
    std::vector<StmtId> stmtIds;
    bool containsRetry = false;
    getStmtIdVectorFromRequest(opCtx, request, &containsRetry, stmtIds);
    if (containsRetry) {
        stageStatus = StageWritesStatus::kContainsRetry;
        return {};
    }

    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto swWriteBatches =
        bucket_catalog::prepareInsertsToBuckets(opCtx,
                                                bucketCatalog,
                                                bucketsColl,
                                                timeseriesOptions,
                                                opCtx->getOpID(),
                                                bucketsColl->getDefaultCollator(),
                                                storageCacheSizeBytes,
                                                /*earlyReturnOnError=*/true,
                                                compressAndWriteBucketFunc,
                                                measurementDocs,
                                                0,
                                                measurementDocs.size(),
                                                {},
                                                bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                errorsAndIndices);

    if (!swWriteBatches.isOK()) {
        invariant(!errorsAndIndices.empty());
        stageStatus = StageWritesStatus::kStagingError;
        return {};
    }

    invariant(errorsAndIndices.empty());
    invariant(!swWriteBatches.getValue().empty());

    auto& writeBatches = swWriteBatches.getValue();

    // Map user batch indexes to stmtIds
    if (isTimeseriesWriteRetryable(opCtx)) {
        for (auto& writeBatch : writeBatches) {
            for (auto userBatchIndex : writeBatch->userBatchIndices) {
                writeBatch->stmtIds.push_back(stmtIds.at(userBatchIndex));
            }
        }
    }

    return std::move(writeBatches);
}

/**
 * Writes to the underlying system.buckets collection as a series of ordered time-series inserts.
 * Returns true on success, false otherwise, filling out errors as appropriate on failure as well
 * as containsRetry which is used at a higher layer to report a retry count metric.
 */
Status performOrderedTimeseriesWritesAtomically(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry) {
    StageWritesStatus stageStatus{StageWritesStatus::kSuccess};
    auto batches =
        stageOrderedWritesToBucketCatalog(opCtx, request, preConditions, errors, stageStatus);

    switch (stageStatus) {
        case StageWritesStatus::kContainsRetry:
            *containsRetry = true;
            [[fallthrough]];
        case StageWritesStatus::kStagingError:
            // Don't attempt commit, retry both of these cases as unordered.
            invariant(batches.empty());
            return Status(ErrorCodes::UnknownError, "Error during time-series write staging"_sd);
        case StageWritesStatus::kCollectionAcquisitionError:
            // No retry, return to user.
            return Status::OK();
        case StageWritesStatus::kSuccess:
            break;
        default:
            MONGO_UNREACHABLE;
    }

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    return commitTimeseriesBucketsAtomically(opCtx, request, batches, opTime, electionId);
}

/**
 * Writes to the underlying system.buckets collection. Returns the indices, of the batch
 * which were attempted in an update operation, but found no bucket to update. These indices
 * can be passed as the 'indices' parameter in a subsequent call to this function, in order
 * to to be retried.
 * In rare cases due to collision from OID generation, we will also retry inserting those bucket
 * documents for a limited number of times.
 * Returns the indices in the original batch of the measurements that need to be retried in
 * 'docsToRetry'.
 */
std::vector<size_t> performUnorderedTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const timeseries::CollectionPreConditions& preConditions,
    size_t start,
    size_t numDocs,
    const bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    std::vector<size_t>& docsToRetry,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry,
    absl::flat_hash_map<int, int>& retryAttemptsForDup) {
    bucket_catalog::TimeseriesWriteBatches batches;
    boost::optional<UUID> optUuid = boost::none;

    // We may have already set this value to true, if we are calling into the unordered path as a
    // fallback from the ordered path.
    if (!*containsRetry) {
        *containsRetry = batchContainsExecutedStatements(opCtx, request, start, numDocs);
    }

    if (*containsRetry || !docsToRetry.empty()) {
        batches = stageUnorderedWritesToBucketCatalogUnoptimized(opCtx,
                                                                 request,
                                                                 preConditions,
                                                                 start,
                                                                 numDocs,
                                                                 allowQueryBasedReopening,
                                                                 docsToRetry,
                                                                 optUuid,
                                                                 errors);
    } else {
        batches = stageUnorderedWritesToBucketCatalog(opCtx,
                                                      request,
                                                      preConditions,
                                                      start,
                                                      numDocs,
                                                      allowQueryBasedReopening,
                                                      optUuid,
                                                      errors);
    }

    tassert(9213700,
            "Timeseries insert did not find bucket collection UUID, but staged inserts in "
            "the in-memory bucket catalog.",
            optUuid || batches.empty());

    hangTimeseriesInsertBeforeCommit.pauseWhileSet();

    if (batches.empty()) {
        return {};
    }

    docsToRetry.clear();
    bool canContinue = true;

    UUID collectionUUID = *optUuid;
    for (size_t i = 0; i < batches.size() && canContinue; ++i) {
        auto& batch = batches[i];
        try {
            commit_result::Result result = internal::commitTimeseriesBucketForBatch(
                opCtx, batch, request, *errors, *opTime, *electionId, retryAttemptsForDup);

            processUnorderedCommitResult(opCtx,
                                         result,
                                         batches,
                                         /*batchIndex=*/i,
                                         docsToRetry,
                                         *errors,
                                         *opTime,
                                         *electionId,
                                         &canContinue);
        } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>& ex) {
            auto bucketId = ex.extraInfo<timeseries::BucketCompressionFailure>()->bucketId();
            auto keySignature =
                ex.extraInfo<timeseries::BucketCompressionFailure>()->keySignature();

            bucket_catalog::freeze(
                bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()),
                bucket_catalog::BucketId{collectionUUID, bucketId, keySignature});

            LOGV2_WARNING(
                8607200,
                "Failed to compress bucket for time-series insert, please retry your write",
                "bucketId"_attr = bucketId);

            for (auto index : batch->userBatchIndices) {
                populateError(opCtx, start + index, ex.toStatus(), errors);
            }
        }

        batch.reset();
    }

    // If we cannot continue the request, we should convert all the 'docsToRetry' into an error.
    if (!canContinue) {
        invariant(!errors->empty());
        for (auto&& index : docsToRetry) {
            errors->emplace_back(index, errors->back().getStatus());
        }
        docsToRetry.clear();
    }
    return docsToRetry;
}
}  // namespace

NamespaceString ns(const mongo::write_ops::InsertCommandRequest& request) {
    return request.getNamespace();
}

Status performAtomicTimeseriesWrites(
    OperationContext* opCtx,
    const std::vector<mongo::write_ops::InsertCommandRequest>& insertOps,
    const std::vector<mongo::write_ops::UpdateCommandRequest>& updateOps) try {
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(!insertOps.empty() || !updateOps.empty());
    auto expectedUUID = !insertOps.empty() ? insertOps.front().getCollectionUUID()
                                           : updateOps.front().getCollectionUUID();
    invariant(expectedUUID.has_value());

    auto ns =
        !insertOps.empty() ? insertOps.front().getNamespace() : updateOps.front().getNamespace();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer(opCtx);
    lastOpFixer.startingOp(ns);

    const auto coll =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              opCtx, ns, AcquisitionPrerequisites::kWrite, expectedUUID),
                          MODE_IX);
    if (!coll.exists()) {
        write_ops::assertTimeseriesBucketsCollectionNotFound(ns);
    }
    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                   .getDatabaseProfileLevel(ns.dbName()));

    mongo::write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    WriteUnitOfWork::OplogEntryGroupType oplogEntryGroupType = WriteUnitOfWork::kDontGroup;
    if (insertOps.size() > 1 && updateOps.empty() &&
        !repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
        oplogEntryGroupType = WriteUnitOfWork::kGroupForPossiblyRetryableOperations;
    }
    WriteUnitOfWork wuow{opCtx, oplogEntryGroupType};

    std::vector<repl::OpTime> oplogSlots;
    boost::optional<std::vector<repl::OpTime>::iterator> slot;
    if (!updateOps.empty()) {
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, ns)) {
            oplogSlots = repl::getNextOpTimes(opCtx, insertOps.size() + updateOps.size());
            slot = oplogSlots.begin();
        }
    }

    auto participant = TransactionParticipant::get(opCtx);
    // Since we are manually updating the "lastWriteOpTime" before committing, we'll also need to
    // manually reset if the storage transaction is aborted.
    if (slot && participant) {
        shard_role_details::getRecoveryUnit(opCtx)->onRollback([](OperationContext* opCtx) {
            TransactionParticipant::get(opCtx).setLastWriteOpTime(opCtx, repl::OpTime());
        });
    }

    std::vector<InsertStatement> inserts;
    inserts.reserve(insertOps.size());

    for (auto& op : insertOps) {
        invariant(op.getDocuments().size() == 1);
        auto doc = op.getDocuments().front();

        // Since this bypasses the usual write path, size validation is needed.
        if (MONGO_unlikely(doc.objsize() > BSONObjMaxUserSize)) {
            LOGV2_WARNING(10856500,
                          "Ordered time-series bucket insert is too large.",
                          "bucketSize"_attr = doc.objsize());
            bucket_catalog::markBucketInsertTooLarge(
                bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()), coll.uuid());

            return {ErrorCodes::BSONObjectTooLarge,
                    "Ordered time-series bucket insert is too large"};
        }

        inserts.emplace_back(op.getStmtIds() ? *op.getStmtIds()
                                             : std::vector<StmtId>{kUninitializedStmtId},
                             doc,
                             slot ? *(*slot)++ : OplogSlot{});
    }

    if (!insertOps.empty()) {
        auto status = collection_internal::insertDocuments(
            opCtx, coll.getCollectionPtr(), inserts.begin(), inserts.end(), &curOp->debug());
        if (!status.isOK()) {
            return status;
        }
        if (slot && participant) {
            // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
            // correctly chained to the previous operations.
            participant.setLastWriteOpTime(opCtx, *(std::prev(*slot)));
        }
    }

    for (auto& op : updateOps) {
        invariant(op.getUpdates().size() == 1);
        auto& update = op.getUpdates().front();

        invariant(coll.getCollectionPtr()->isClustered());
        auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

        auto original = coll.getCollectionPtr()->docFor(opCtx, recordId);

        CollectionUpdateArgs args{original.value()};
        args.criteria = update.getQ();
        if (const auto& stmtIds = op.getStmtIds()) {
            args.stmtIds = *stmtIds;
        }
        args.source = OperationSource::kTimeseriesInsert;

        BSONObj updated;
        BSONObj diffFromUpdate;
        const BSONObj* diffOnIndexes =
            collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.
        if (update.getU().type() == mongo::write_ops::UpdateModification::Type::kDelta) {
            diffFromUpdate = update.getU().getDiff();
            updated = doc_diff::applyDiff(original.value(),
                                          diffFromUpdate,
                                          update.getU().mustCheckExistenceForInsertOperations(),
                                          update.getU().verifierFunction);
            diffOnIndexes = &diffFromUpdate;
            args.update = update_oplog_entry::makeDeltaOplogEntry(diffFromUpdate);
        } else if (update.getU().type() == mongo::write_ops::UpdateModification::Type::kTransform) {
            const auto& transform = update.getU().getTransform();
            auto transformed = transform(original.value());
            tassert(7050400,
                    "Could not apply transformation to time series bucket document",
                    transformed.has_value());
            updated = std::move(transformed.value());
            args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
        } else {
            invariant(false, "Unexpected update type");
        }

        // Since this bypasses the usual write path, size validation is needed.
        if (MONGO_unlikely(updated.objsize() > BSONObjMaxUserSize)) {
            LOGV2_WARNING(
                10856501,
                "Ordered time-series bucket update is too large. Will internally retry write on "
                "a new bucket.",
                "bucketSize"_attr = updated.objsize());
            bucket_catalog::markBucketUpdateTooLarge(
                bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext()), coll.uuid());

            return {ErrorCodes::BSONObjectTooLarge,
                    "Ordered time-series bucket update is too large"};
        }

        if (slot) {
            args.oplogSlots = {**slot};
            fassert(5481600,
                    shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                        args.oplogSlots[0].getTimestamp()));
        }

        collection_internal::updateDocument(opCtx,
                                            coll.getCollectionPtr(),
                                            recordId,
                                            original,
                                            updated,
                                            diffOnIndexes,
                                            nullptr /*indexesAffected*/,
                                            &curOp->debug(),
                                            &args);
        if (slot) {
            if (participant) {
                // Manually sets the timestamp so that the "prevOpTime" field in the oplog entry is
                // correctly chained to the previous operations.
                participant.setLastWriteOpTime(opCtx, **slot);
            }
            ++(*slot);
        }
    }

    if (MONGO_unlikely(failAtomicTimeseriesWrites.shouldFail())) {
        return {ErrorCodes::FailPointEnabled,
                "Failing time-series writes due to failAtomicTimeseriesWrites fail point"};
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();

    return Status::OK();
} catch (const ExceptionFor<ErrorCodes::TimeseriesBucketCompressionFailed>&) {
    // If we encounter a TimeseriesBucketCompressionFailure, we should throw to
    // a higher level (write_ops_exec::performUpdates) so that we can freeze the corrupt bucket.
    throw;
} catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>&) {
    // This particular CollectionUUIDMismatch is re-thrown differently because there is already a
    // check for this error higher up, which means this error must come from the guards installed to
    // enforce that time-series operations are prepared and committed on the same collection.
    uasserted(9748800, "Collection was changed during insert");
} catch (const DBException& ex) {
    return ex.toStatus();
}

commit_result::Result commitTimeseriesBucketForBatch(
    OperationContext* opCtx,
    std::shared_ptr<bucket_catalog::WriteBatch> batch,
    const mongo::write_ops::InsertCommandRequest& request,
    std::vector<mongo::write_ops::WriteError>& errors,
    boost::optional<repl::OpTime>& opTime,
    boost::optional<OID>& electionId,
    absl::flat_hash_map<int, int>& retryAttemptsForDup) try {
    hangCommitTimeseriesBucketBeforeCheckingTimeseriesCollection.pauseWhileSet();

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
    // Collection instances remain valid, and the collator is not invalidated.
    auto catalog = CollectionCatalog::get(opCtx);
    const CollatorInterface* collator = nullptr;
    NamespaceString nss;

    try {
        // The associated collection must be acquired before we check for the presence of
        // buckets collection. This ensures that a potential ShardVersion mismatch can be
        // detected, before checking for other errors. Moreover, since e.g. 'prepareCommit()' might
        // block waiting for other batches to complete, limiting the scope of the
        // collectionAcquisition is necessary to prevent deadlocks due to ticket exhaustion.
        const auto bucketsAcq = acquireAndValidateBucketsCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, internal::ns(request), AcquisitionPrerequisites::kRead),
            MODE_IS);
        nss = bucketsAcq.nss();
        collator = bucketsAcq.getCollectionPtr()->getDefaultCollator();
    } catch (const DBException& ex) {
        if (ex.code() != ErrorCodes::StaleDbVersion && !ErrorCodes::isStaleShardVersionError(ex)) {
            throw;
        }
        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(ex.toStatus());

        return commit_result::NonContinuableError{ex.toStatus()};
    }

    auto status = prepareCommit(bucketCatalog, batch, collator);
    if (!status.isOK()) {
        invariant(bucket_catalog::isWriteBatchFinished(*batch));
        return commit_result::ContinuableRetryableError{};
    }

    hangTimeseriesInsertBeforeWrite.pauseWhileSet();

    const auto docId = batch->bucketId.oid;
    const bool performInsert = batch->numPreviouslyCommittedMeasurements == 0;
    if (performInsert) {
        const auto output = performTimeseriesInsertFromBatch(opCtx, nss, request, batch);
        auto insertStatus = output.result.getStatus();

        if (!insertStatus.isOK()) {
            // Automatically attempts to retry on DuplicateKey error.
            if (insertStatus.code() == ErrorCodes::DuplicateKey &&
                retryAttemptsForDup[batch->userBatchIndices.front()]++ <
                    gTimeseriesInsertMaxRetriesOnDuplicates.load()) {
                return commit_result::ContinuableRetryableErrorWithAbortBatch{insertStatus};
            } else {
                if (insertStatus.code() == ErrorCodes::BSONObjectTooLarge) {
                    LOGV2_WARNING(10856502,
                                  "Unordered time-series bucket insert is too large.",
                                  "statusMsg"_attr = insertStatus.reason());
                    bucket_catalog::markBucketInsertTooLarge(bucketCatalog,
                                                             batch->bucketId.collectionUUID);
                }
                if (output.canContinue)
                    return commit_result::ContinuableErrorWithAbortBatch{insertStatus};
                return commit_result::NonContinuableErrorWithAbortBatch{insertStatus};
            }
        }

        invariant(output.result.getValue().getN() == 1,
                  str::stream() << "Expected 1 insertion of document with _id '" << docId
                                << "', but found " << output.result.getValue().getN() << ".");
    } else {
        auto op = write_ops_utils::makeTimeseriesCompressedDiffUpdateOpFromBatch(opCtx, batch, nss);

        auto const output = performTimeseriesUpdate(opCtx, batch->bucketKey.metadata, op, request);
        auto updateStatus = output.result.getStatus();

        if ((updateStatus.isOK() && output.result.getValue().getNModified() != 1) ||
            updateStatus.code() == ErrorCodes::WriteConflict ||
            updateStatus.code() == ErrorCodes::TemporarilyUnavailable) {
            return commit_result::ContinuableRetryableErrorWithAbortBatchAbandonSnapshot{
                updateStatus.isOK()
                    ? Status{ErrorCodes::WriteConflict, "Could not update non-existent bucket"}
                    : updateStatus};
        } else if (!updateStatus.isOK()) {
            if (updateStatus.code() == ErrorCodes::BSONObjectTooLarge) {
                LOGV2_WARNING(10856503,
                              "Unordered time-series bucket update is too large.",
                              "statusMsg"_attr = updateStatus.reason());
                bucket_catalog::markBucketUpdateTooLarge(bucketCatalog,
                                                         batch->bucketId.collectionUUID);
            }
            if (output.canContinue) {
                return commit_result::ContinuableErrorWithAbortBatch{updateStatus};
            }
            return commit_result::NonContinuableErrorWithAbortBatch{updateStatus};
        }
    }
    return commit_result::Success{};
} catch (const DBException& ex) {
    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
    abort(bucketCatalog, batch, ex.toStatus());
    throw;
}

void performUnorderedTimeseriesWritesWithRetries(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    size_t start,
    size_t numDocs,
    const bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    std::vector<mongo::write_ops::WriteError>* errors,
    boost::optional<repl::OpTime>* opTime,
    boost::optional<OID>* electionId,
    bool* containsRetry) {
    std::vector<size_t> docsToRetry;
    absl::flat_hash_map<int, int> retryAttemptsForDup;
    do {
        docsToRetry = performUnorderedTimeseriesWrites(opCtx,
                                                       request,
                                                       preConditions,
                                                       start,
                                                       numDocs,
                                                       allowQueryBasedReopening,
                                                       docsToRetry,
                                                       errors,
                                                       opTime,
                                                       electionId,
                                                       containsRetry,
                                                       retryAttemptsForDup);
        if (!retryAttemptsForDup.empty()) {
            bucket_catalog::resetBucketOIDCounter();
        }
    } while (!docsToRetry.empty());
}

size_t performOrderedTimeseriesWrites(OperationContext* opCtx,
                                      const mongo::write_ops::InsertCommandRequest& request,
                                      const CollectionPreConditions& preConditions,
                                      std::vector<mongo::write_ops::WriteError>* errors,
                                      boost::optional<repl::OpTime>* opTime,
                                      boost::optional<OID>* electionId,
                                      bool* containsRetry) {
    auto result = performOrderedTimeseriesWritesAtomically(
        opCtx, request, preConditions, errors, opTime, electionId, containsRetry);
    if (result.isOK()) {
        if (!errors->empty()) {
            invariant(errors->size() == 1);
            return errors->front().getIndex();
        }
        return request.getDocuments().size();
    }

    for (size_t i = 0; i < request.getDocuments().size(); ++i) {
        bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening =
            result.code() == ErrorCodes::BSONObjectTooLarge
            ? bucket_catalog::AllowQueryBasedReopening::kDisallow
            : bucket_catalog::AllowQueryBasedReopening::kAllow;
        performUnorderedTimeseriesWritesWithRetries(opCtx,
                                                    request,
                                                    preConditions,
                                                    i,
                                                    1,
                                                    allowQueryBasedReopening,
                                                    errors,
                                                    opTime,
                                                    electionId,
                                                    containsRetry);
        if (!errors->empty()) {
            return i;
        }
    }

    return request.getDocuments().size();
}

void rewriteIndicesForSubsetOfBatch(OperationContext* opCtx,
                                    const mongo::write_ops::InsertCommandRequest& request,
                                    const std::vector<size_t>& originalIndices,
                                    bucket_catalog::TimeseriesWriteBatches& writeBatches) {
    auto stmtIds = request.getStmtIds();
    auto retryableWrites = isTimeseriesWriteRetryable(opCtx);
    for (auto& writeBatch : writeBatches) {
        for (size_t i = 0; i < writeBatch->userBatchIndices.size(); i++) {
            invariant(i < writeBatch->userBatchIndices.size());
            auto shiftedIndex = writeBatch->userBatchIndices[i];
            invariant(shiftedIndex < originalIndices.size());
            auto originalIndex = originalIndices[shiftedIndex];
            writeBatch->userBatchIndices[i] = originalIndex;
            if (retryableWrites) {
                if (stmtIds) {
                    invariant(originalIndex < stmtIds->size());
                }
                auto stmtId = stmtIds ? stmtIds->at(originalIndex)
                                      : request.getStmtId().value_or(0) + originalIndex;
                writeBatch->stmtIds.push_back(stmtId);
            }
        }
    }
}

void processErrorsForSubsetOfBatch(
    OperationContext* opCtx,
    const std::vector<bucket_catalog::WriteStageErrorAndIndex>& errorsAndIndices,
    const std::vector<size_t>& originalIndices,
    std::vector<mongo::write_ops::WriteError>* errors) {
    if (!errorsAndIndices.empty()) {
        for (auto& [errorStatus, index] : errorsAndIndices) {
            invariant(index < originalIndices.size());
            populateError(opCtx, originalIndices[index], errorStatus, errors);
        }
    }
}

bucket_catalog::TimeseriesWriteBatches stageUnorderedWritesToBucketCatalog(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    size_t startIndex,
    size_t numDocsToStage,
    const bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    boost::optional<UUID>& optUuid,
    std::vector<mongo::write_ops::WriteError>* errors) {

    hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection.pauseWhileSet();

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
    // Collection instances remain valid, and the bucketsColl is not invalidated.
    std::shared_ptr<const CollectionCatalog> catalog;
    const Collection* bucketsColl = nullptr;
    TimeseriesOptions timeseriesOptions;

    try {
        // It must be ensured that the CollectionShardingState remains consistent while rebuilding
        // the timeseriesOptions. However, the associated collection must be acquired before
        // we check for the presence of buckets collection. This ensures that a potential
        // ShardVersion mismatch can be detected, before checking for other errors.
        const auto bucketsAcq = acquireAndValidateBucketsCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                    internal::ns(request),
                                                    AcquisitionPrerequisites::kRead,
                                                    preConditions.expectedUUID()),
            MODE_IS);

        CollectionPreConditions::checkAcquisitionAgainstPreConditions(
            opCtx, preConditions, bucketsAcq);

        // We want to ensure that the catalog instance after the scope of the acquisition is the
        // same as before the acquisition. Acquiring the collection involves stashing the
        // current catalog instance, so assigning the catalog in scope of the try block ensures
        // that we have a consistent catalog with the acquisition.
        catalog = CollectionCatalog::get(opCtx);

        bucketsColl = bucketsAcq.getCollectionPtr().get();
        optUuid = bucketsColl->uuid();
        // Process timeseriesOptions
        timeseriesOptions = bucketsColl->getTimeseriesOptions().get();
        rebuildOptionsWithGranularityFromConfigServer(opCtx, bucketsAcq.nss(), timeseriesOptions);
    } catch (const DBException& ex) {
        if (ex.code() != ErrorCodes::StaleDbVersion && !ErrorCodes::isStaleShardVersionError(ex)) {
            throw;
        }

        auto collectionAcquisitionStatus = ex.toStatus();
        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(collectionAcquisitionStatus);

        // Emplace errors with the collectionAcquisitionStatus for all of the measurements we're
        // inserting, and return an empty vector of WriteBatches.
        auto addCollectionAcquisitionError = [&](size_t index) {
            invariant(index < request.getDocuments().size());
            populateError(opCtx, index, collectionAcquisitionStatus, errors);
        };

        for (size_t i = startIndex; i < startIndex + numDocsToStage; i++) {
            addCollectionAcquisitionError(i);
        }

        return {};
    }

    bucket_catalog::CompressAndWriteBucketFunc compressAndWriteBucketFunc =
        compressUncompressedBucketOnReopen;
    auto storageCacheSizeBytes = getStorageCacheSizeBytes(opCtx);

    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto swWriteBatches = bucket_catalog::prepareInsertsToBuckets(opCtx,
                                                                  bucketCatalog,
                                                                  bucketsColl,
                                                                  timeseriesOptions,
                                                                  opCtx->getOpID(),
                                                                  bucketsColl->getDefaultCollator(),
                                                                  storageCacheSizeBytes,
                                                                  /*returnEarlyOnError=*/false,
                                                                  compressAndWriteBucketFunc,
                                                                  request.getDocuments(),
                                                                  startIndex,
                                                                  numDocsToStage,
                                                                  /*indices=*/{},
                                                                  allowQueryBasedReopening,
                                                                  errorsAndIndices);

    // Even if we encountered errors, in the unordered path we will continue and stage write batches
    // for any measurements that we can.
    invariant(swWriteBatches);
    auto& writeBatches = swWriteBatches.getValue();

    if (!errorsAndIndices.empty()) {
        for (auto& [errorStatus, index] : errorsAndIndices) {
            populateError(opCtx, index, errorStatus, errors);
        }
    }

    if (isTimeseriesWriteRetryable(opCtx)) {
        auto stmtIds = request.getStmtIds();
        for (auto& writeBatch : writeBatches) {
            for (auto userBatchIndex : writeBatch->userBatchIndices) {
                auto stmtId = stmtIds ? stmtIds->at(userBatchIndex)
                                      : request.getStmtId().value_or(0) + userBatchIndex;
                writeBatch->stmtIds.push_back(stmtId);
            }
        }
    }

    return std::move(writeBatches);
}

bucket_catalog::TimeseriesWriteBatches stageUnorderedWritesToBucketCatalogUnoptimized(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const CollectionPreConditions& preConditions,
    size_t startIndex,
    size_t numDocsToStage,
    const bucket_catalog::AllowQueryBasedReopening allowQueryBasedReopening,
    const std::vector<size_t>& docsToRetry,
    boost::optional<UUID>& optUuid,
    std::vector<mongo::write_ops::WriteError>* errors) {

    hangInsertIntoBucketCatalogBeforeCheckingTimeseriesCollection.pauseWhileSet();

    auto& bucketCatalog = bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());

    // Explicitly hold a reference to the CollectionCatalog, such that the corresponding
    // Collection instances remain valid, and the bucketsColl is not invalidated.
    std::shared_ptr<const CollectionCatalog> catalog;
    const Collection* bucketsColl = nullptr;
    TimeseriesOptions timeseriesOptions;

    try {
        // It must be ensured that the CollectionShardingState remains consistent while rebuilding
        // the timeseriesOptions. However, the associated collection must be acquired before
        // we check for the presence of buckets collection. This ensures that a potential
        // ShardVersion mismatch can be detected, before checking for other errors.
        const auto bucketsAcq = acquireAndValidateBucketsCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx,
                                                    internal::ns(request),
                                                    AcquisitionPrerequisites::kRead,
                                                    preConditions.expectedUUID()),
            MODE_IS);

        CollectionPreConditions::checkAcquisitionAgainstPreConditions(
            opCtx, preConditions, bucketsAcq);

        // We want to ensure that the catalog instance after the scope of the acquisition is the
        // same as before the acquisition. Acquiring the collection involves stashing the
        // current catalog instance, so assigning the catalog in scope of the try block ensures
        // that we have a consistent catalog with the acquisition.
        catalog = CollectionCatalog::get(opCtx);

        bucketsColl = bucketsAcq.getCollectionPtr().get();
        optUuid = bucketsColl->uuid();
        // Process timeseriesOptions
        timeseriesOptions = bucketsColl->getTimeseriesOptions().get();
        rebuildOptionsWithGranularityFromConfigServer(opCtx, bucketsAcq.nss(), timeseriesOptions);
    } catch (const DBException& ex) {
        if (ex.code() != ErrorCodes::StaleDbVersion && !ErrorCodes::isStaleShardVersionError(ex)) {
            throw;
        }

        auto collectionAcquisitionStatus = ex.toStatus();
        auto& oss{OperationShardingState::get(opCtx)};
        oss.setShardingOperationFailedStatus(collectionAcquisitionStatus);

        // Emplace errors with the collectionAcquisitionStatus for all of the measurements we're
        // inserting, and return an empty vector of WriteBatches.
        auto addCollectionAcquisitionError = [&](size_t index) {
            invariant(index < request.getDocuments().size());
            populateError(opCtx, index, collectionAcquisitionStatus, errors);
        };

        if (!docsToRetry.empty()) {
            std::for_each(docsToRetry.begin(), docsToRetry.end(), addCollectionAcquisitionError);
        } else {
            for (size_t i = startIndex; i < startIndex + numDocsToStage; i++) {
                addCollectionAcquisitionError(i);
            }
        }
        return {};
    }

    std::vector<BSONObj> batchExcludingExecutedStatements;
    std::vector<size_t> originalIndices;
    filterOutExecutedMeasurements(opCtx,
                                  request,
                                  startIndex,
                                  numDocsToStage,
                                  docsToRetry,
                                  batchExcludingExecutedStatements,
                                  originalIndices);
    if (batchExcludingExecutedStatements.empty()) {
        return {};
    }

    bucket_catalog::CompressAndWriteBucketFunc compressAndWriteBucketFunc =
        compressUncompressedBucketOnReopen;
    auto storageCacheSizeBytes = getStorageCacheSizeBytes(opCtx);

    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;
    auto swWriteBatches = bucket_catalog::prepareInsertsToBuckets(
        opCtx,
        bucketCatalog,
        bucketsColl,
        timeseriesOptions,
        opCtx->getOpID(),
        bucketsColl->getDefaultCollator(),
        storageCacheSizeBytes,
        /*returnEarlyOnError=*/false,
        compressAndWriteBucketFunc,
        batchExcludingExecutedStatements,
        /*startIndex=*/0,  // We want to start from the beginning of the filtered batch
        /*numDocsToStage=*/batchExcludingExecutedStatements.size(),
        /*docsToRetry=*/{},  // We take indices into account when filtering
        allowQueryBasedReopening,
        errorsAndIndices);

    // Even if we encountered errors while staging, in the unordered path we will continue and
    // stage write batches for any measurements that we can.
    invariant(swWriteBatches);
    auto& writeBatches = swWriteBatches.getValue();
    rewriteIndicesForSubsetOfBatch(opCtx, request, originalIndices, writeBatches);
    processErrorsForSubsetOfBatch(opCtx, errorsAndIndices, originalIndices, errors);
    return std::move(writeBatches);
}

}  // namespace mongo::timeseries::write_ops::internal
