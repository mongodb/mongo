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


#include "mongo/db/timeseries/timeseries_write_util.h"

#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo::timeseries {
namespace {

// Performs the storage write of an update to a time-series bucket document.
void updateTimeseriesDocument(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const write_ops::UpdateCommandRequest& op,
                              OpDebug* opDebug,
                              bool fromMigrate,
                              StmtId stmtId) {
    invariant(op.getUpdates().size() == 1);
    auto& update = op.getUpdates().front();

    invariant(coll->isClustered());
    auto recordId = record_id_helpers::keyForOID(update.getQ()["_id"].OID());

    auto original = coll->docFor(opCtx, recordId);

    CollectionUpdateArgs args{original.value()};
    args.criteria = update.getQ();
    args.stmtIds = {stmtId};
    if (fromMigrate) {
        args.source = OperationSource::kFromMigrate;
    }

    BSONObj updated;
    BSONObj diffFromUpdate;
    const BSONObj* diffOnIndexes =
        collection_internal::kUpdateAllIndexes;  // Assume all indexes are affected.
    if (update.getU().type() == write_ops::UpdateModification::Type::kDelta) {
        diffFromUpdate = update.getU().getDiff();
        updated = doc_diff::applyDiff(
            original.value(), diffFromUpdate, false, update.getU().verifierFunction);
        diffOnIndexes = &diffFromUpdate;
        args.update = update_oplog_entry::makeDeltaOplogEntry(diffFromUpdate);
    } else if (update.getU().type() == write_ops::UpdateModification::Type::kTransform) {
        const auto& transform = update.getU().getTransform();
        auto transformed = transform(original.value());
        tassert(7667900,
                "Could not apply transformation to time series bucket document",
                transformed.has_value());
        updated = std::move(transformed.value());
        args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
    } else if (update.getU().type() == write_ops::UpdateModification::Type::kReplacement) {
        updated = update.getU().getUpdateReplacement();
        args.update = update_oplog_entry::makeReplacementOplogEntry(updated);
    } else {
        invariant(false, "Unexpected update type");
    }

    collection_internal::updateDocument(opCtx,
                                        coll,
                                        recordId,
                                        original,
                                        updated,
                                        diffOnIndexes,
                                        nullptr /*indexesAffected*/,
                                        opDebug,
                                        &args);
}

std::shared_ptr<bucket_catalog::WriteBatch>& extractFromSelf(
    std::shared_ptr<bucket_catalog::WriteBatch>& batch) {
    return batch;
}

uint64_t getStorageCacheSizeBytes(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
}

BSONObj getSuitableBucketForReopening(OperationContext* opCtx,
                                      const Collection* bucketsColl,
                                      const TimeseriesOptions& options,
                                      bucket_catalog::ReopeningContext& reopeningContext) {
    return visit(
        OverloadedVisitor{
            [](const std::monostate&) { return BSONObj{}; },
            [&](const OID& bucketId) {
                reopeningContext.fetchedBucket = true;
                return DBDirectClient{opCtx}.findOne(bucketsColl->ns(), BSON("_id" << bucketId));
            },
            [&](const std::vector<BSONObj>& pipeline) {
                // Ensure we have a index on meta and time for the time-series collection before
                // performing the query. Without the index we will perform a full collection scan
                // which could cause us to take a performance hit.
                if (auto index = getIndexSupportingReopeningQuery(
                        opCtx, bucketsColl->getIndexCatalog(), options)) {
                    // Resort to Query-Based reopening approach.
                    reopeningContext.queriedBucket = true;
                    DBDirectClient client{opCtx};

                    // Run an aggregation to find a suitable bucket to reopen.
                    AggregateCommandRequest aggRequest(bucketsColl->ns(), pipeline);
                    aggRequest.setHint(index);

                    // TODO SERVER-86094: remove after fixing perf regression.
                    query_settings::QuerySettings querySettings;
                    querySettings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
                    aggRequest.setQuerySettings(querySettings);

                    auto swCursor = DBClientCursor::fromAggregationRequest(
                        &client, aggRequest, false /* secondaryOk */, false /* useExhaust */);
                    if (swCursor.isOK() && swCursor.getValue()->more()) {
                        return swCursor.getValue()->next();
                    }
                }
                return BSONObj{};
            },
        },
        reopeningContext.candidate);
}

StatusWith<bucket_catalog::InsertResult> attemptInsertIntoBucketWithReopening(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    const TimeseriesOptions& options,
    const BSONObj& measurementDoc,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    bucket_catalog::InsertContext& insertContext,
    const Date_t& time) {
    boost::optional<bucket_catalog::BucketId> uncompressedBucketId{boost::none};
    // The purpose of this scope is to destroy the ReopeningContext for the
    // compress-and-write-uncompressed-bucket scenario.
    {
        auto swResult = bucket_catalog::tryInsert(bucketCatalog,
                                                  bucketsColl->getDefaultCollator(),
                                                  measurementDoc,
                                                  opCtx->getOpID(),
                                                  insertContext,
                                                  time,
                                                  getStorageCacheSizeBytes(opCtx));
        if (!swResult.isOK()) {
            return swResult;
        }

        auto visitResult = visit(
            OverloadedVisitor{
                [&](const bucket_catalog::SuccessfulInsertion&)
                    -> StatusWith<bucket_catalog::InsertResult> { return std::move(swResult); },
                [&](bucket_catalog::ReopeningContext& reopeningContext)
                    -> StatusWith<bucket_catalog::InsertResult> {
                    auto suitableBucket = getSuitableBucketForReopening(
                        opCtx, bucketsColl, options, reopeningContext);

                    if (!suitableBucket.isEmpty()) {
                        reopeningContext.bucketToReopen = bucket_catalog::BucketToReopen{
                            suitableBucket, [opCtx, bucketsColl](const BSONObj& bucketDoc) {
                                return bucketsColl->checkValidation(opCtx, bucketDoc);
                            }};
                    }

                    if (!suitableBucket.isEmpty() &&
                        !timeseries::isCompressedBucket(suitableBucket)) {
                        uncompressedBucketId = extractBucketId(
                            bucketCatalog, options, bucketsColl->uuid(), suitableBucket);
                        return StatusWith<bucket_catalog::InsertResult>{
                            bucket_catalog::InsertResult{
                                std::in_place_type<bucket_catalog::ReopeningContext>,
                                std::move(reopeningContext)}};
                    }

                    return bucket_catalog::insertWithReopeningContext(
                        bucketCatalog,
                        bucketsColl->getDefaultCollator(),
                        measurementDoc,
                        opCtx->getOpID(),
                        reopeningContext,
                        insertContext,
                        time,
                        getStorageCacheSizeBytes(opCtx));
                },
                [](bucket_catalog::InsertWaiter& waiter)
                    -> StatusWith<bucket_catalog::InsertResult> {
                    // Need to wait for another operation to finish, then retry. This could be
                    // another reopening request or a previously prepared write batch for the same
                    // series (metaField value). The easiest way to retry here is to return a write
                    // conflict.
                    bucket_catalog::waitToInsert(&waiter);
                    return Status{ErrorCodes::WriteConflict, "waited to retry"};
                },
            },
            swResult.getValue());

        if (!uncompressedBucketId.has_value()) {
            return visitResult;
        }
    }

    try {
        LOGV2(8654200,
              "Compressing uncompressed bucket upon bucket reopen",
              "bucketId"_attr = uncompressedBucketId.get().oid);
        // Compress the uncompressed bucket and write to disk.
        if (compressAndWriteBucketFunc) {
            compressAndWriteBucketFunc(
                opCtx, uncompressedBucketId.get(), bucketsColl->ns(), options.getTimeField());
        }
    } catch (...) {
        bucket_catalog::freeze(bucketCatalog, uncompressedBucketId.get());
        LOGV2_WARNING(8654201,
                      "Failed to compress bucket for time-series insert upon reopening, will retry "
                      "insert on a new bucket",
                      "bucketId"_attr = uncompressedBucketId.get().oid);
        return Status{timeseries::BucketCompressionFailure(bucketsColl->uuid(),
                                                           uncompressedBucketId.get().oid,
                                                           uncompressedBucketId.get().keySignature),
                      "Failed to compress bucket for time-series insert upon reopening"};
    }
    return Status{ErrorCodes::WriteConflict,
                  "existing uncompressed bucket was compressed, retry insert on compressed bucket"};
}

}  // namespace

void assertTimeseriesBucketsCollection(const Collection* bucketsColl) {
    uassert(
        8555700,
        "Catalog changed during operation, could not find time series buckets collection for write",
        bucketsColl);
    uassert(8555701,
            "Catalog changed during operation, missing time-series options",
            bucketsColl->getTimeseriesOptions());
}

BSONObj makeBucketDocument(const std::vector<BSONObj>& measurements,
                           const NamespaceString& nss,
                           const UUID& collectionUUID,
                           const TimeseriesOptions& options,
                           const StringDataComparator* comparator) {
    tracking::Context trackingContext;
    auto res = uassertStatusOK(bucket_catalog::internal::extractBucketingParameters(
        trackingContext, collectionUUID, options, measurements[0]));
    auto time = res.second;
    auto [oid, _] = bucket_catalog::internal::generateBucketOID(time, options);
    write_ops_utils::BucketDocument bucketDoc =
        write_ops_utils::makeNewDocumentForWrite(nss,
                                                 collectionUUID,
                                                 oid,
                                                 measurements,
                                                 res.first.metadata.toBSON(),
                                                 options,
                                                 comparator,
                                                 boost::none);

    invariant(bucketDoc.compressedBucket);
    return *bucketDoc.compressedBucket;
}

void getOpTimeAndElectionId(OperationContext* opCtx,
                            boost::optional<repl::OpTime>* opTime,
                            boost::optional<OID>* electionId) {
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    const auto isReplSet = replCoord->getSettings().isReplSet();

    *opTime = isReplSet
        ? boost::make_optional(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp())
        : boost::none;
    *electionId = isReplSet ? boost::make_optional(replCoord->getElectionId()) : boost::none;
}

StatusWith<bucket_catalog::InsertResult> attemptInsertIntoBucket(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const Collection* bucketsColl,
    TimeseriesOptions& timeSeriesOptions,
    const BSONObj& measurementDoc,
    BucketReopeningPermittance reopening,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    auto insertContextAndDate = bucket_catalog::prepareInsert(
        bucketCatalog, bucketsColl->uuid(), timeSeriesOptions, measurementDoc);

    if (!insertContextAndDate.isOK()) {
        return insertContextAndDate.getStatus();
    }
    switch (reopening) {
        case BucketReopeningPermittance::kAllowed:
            while (true) {
                auto result = attemptInsertIntoBucketWithReopening(
                    opCtx,
                    bucketCatalog,
                    bucketsColl,
                    timeSeriesOptions,
                    measurementDoc,
                    compressAndWriteBucketFunc,
                    std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue()),
                    std::get<Date_t>(insertContextAndDate.getValue()));
                if (!result.isOK()) {
                    if (result.getStatus().code() == ErrorCodes::WriteConflict) {
                        // If there is an era offset (between the bucket we want to reopen and the
                        // catalog's current era), we could hit a WriteConflict error indicating we
                        // will need to refetch a bucket document as it is potentially stale. The
                        // other scenario this will happen is when an uncompressed bucket is
                        // encountered and must be compressed before reattempting a write to the
                        // compressed bucket.

                        for (auto& closedBucket : std::get<bucket_catalog::InsertContext>(
                                                      insertContextAndDate.getValue())
                                                      .closedBuckets) {
                            compressAndWriteBucketFunc(opCtx,
                                                       closedBucket.bucketId,
                                                       bucketsColl->ns(),
                                                       closedBucket.timeField);
                        }

                        // Will update state in the bucket catalog to clear out the closed buckets.
                        std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue())
                            .closedBuckets.clear();
                        continue;
                    } else if (result.getStatus().code() ==
                               ErrorCodes::TimeseriesBucketCompressionFailed) {
                        // This is the case where the reopened bucket was corrupted. Retry the
                        // insert directly on a new bucket.
                        return bucket_catalog::insert(
                            bucketCatalog,
                            bucketsColl->getDefaultCollator(),
                            measurementDoc,
                            opCtx->getOpID(),
                            std::get<bucket_catalog::InsertContext>(
                                insertContextAndDate.getValue()),
                            std::get<Date_t>(insertContextAndDate.getValue()),
                            getStorageCacheSizeBytes(opCtx));
                    }
                }
                return result;
            }
        case BucketReopeningPermittance::kDisallowed:
            return bucket_catalog::insert(
                bucketCatalog,
                bucketsColl->getDefaultCollator(),
                measurementDoc,
                opCtx->getOpID(),
                std::get<bucket_catalog::InsertContext>(insertContextAndDate.getValue()),
                std::get<Date_t>(insertContextAndDate.getValue()),
                getStorageCacheSizeBytes(opCtx));
    }
    MONGO_UNREACHABLE;
}

TimeseriesBatches insertIntoBucketCatalogForUpdate(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& bucketCatalog,
    const CollectionPtr& bucketsColl,
    const std::vector<BSONObj>& measurements,
    const NamespaceString& bucketsNs,
    TimeseriesOptions& timeSeriesOptions,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc) {
    TimeseriesBatches batches;

    for (const auto& measurement : measurements) {
        auto result =
            uassertStatusOK(attemptInsertIntoBucket(opCtx,
                                                    bucketCatalog,
                                                    bucketsColl.get(),
                                                    timeSeriesOptions,
                                                    measurement,
                                                    BucketReopeningPermittance::kDisallowed,
                                                    compressAndWriteBucketFunc));
        auto* insertResult = get_if<bucket_catalog::SuccessfulInsertion>(&result);
        invariant(insertResult);
        batches.emplace_back(std::move(insertResult->batch));
    }

    return batches;
}

void performAtomicWrites(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::variant<write_ops::UpdateCommandRequest,
                                       write_ops::DeleteCommandRequest>>& modificationOp,
    const std::vector<write_ops::InsertCommandRequest>& insertOps,
    const std::vector<write_ops::UpdateCommandRequest>& updateOps,
    bool fromMigrate,
    StmtId stmtId) {
    tassert(
        7655102, "must specify at least one type of write", modificationOp || !insertOps.empty());
    NamespaceString ns = coll->ns();

    DisableDocumentValidation disableDocumentValidation{opCtx};

    write_ops_exec::LastOpFixer lastOpFixer{opCtx};
    lastOpFixer.startingOp(ns);

    auto curOp = CurOp::get(opCtx);
    curOp->raiseDbProfileLevel(DatabaseProfileSettings::get(opCtx->getServiceContext())
                                   .getDatabaseProfileLevel(ns.dbName()));

    write_ops_exec::assertCanWrite_inlock(opCtx, ns);

    // Groups all operations in one or several chained oplog entries to ensure the writes are
    // replicated atomically.
    auto groupOplogEntries =
        !opCtx->getTxnNumber() && (!insertOps.empty() || !updateOps.empty()) && modificationOp
        ? WriteUnitOfWork::kGroupForTransaction
        : WriteUnitOfWork::kDontGroup;
    WriteUnitOfWork wuow{opCtx, groupOplogEntries};

    if (modificationOp) {
        visit(
            OverloadedVisitor{[&](const write_ops::UpdateCommandRequest& updateOp) {
                                  updateTimeseriesDocument(
                                      opCtx, coll, updateOp, &curOp->debug(), fromMigrate, stmtId);
                              },
                              [&](const write_ops::DeleteCommandRequest& deleteOp) {
                                  invariant(deleteOp.getDeletes().size() == 1);
                                  auto deleteId = record_id_helpers::keyForOID(
                                      deleteOp.getDeletes().front().getQ()["_id"].OID());
                                  invariant(recordId == deleteId);
                                  collection_internal::deleteDocument(
                                      opCtx, coll, stmtId, recordId, &curOp->debug(), fromMigrate);
                              }},
            *modificationOp);
    }

    if (!insertOps.empty()) {
        std::vector<InsertStatement> insertStatements;
        for (auto& op : insertOps) {
            invariant(op.getDocuments().size() == 1);
            if (modificationOp) {
                insertStatements.emplace_back(op.getDocuments().front());
            } else {
                // Appends the stmtId for upsert.
                insertStatements.emplace_back(stmtId, op.getDocuments().front());
            }
        }
        uassertStatusOK(collection_internal::insertDocuments(
            opCtx, coll, insertStatements.begin(), insertStatements.end(), &curOp->debug()));
    }

    for (auto& updateOp : updateOps) {
        updateTimeseriesDocument(opCtx, coll, updateOp, &curOp->debug(), fromMigrate, stmtId);
    }

    wuow.commit();

    lastOpFixer.finishedOpSuccessfully();
}

void commitTimeseriesBucketsAtomically(
    OperationContext* opCtx,
    bucket_catalog::BucketCatalog& sideBucketCatalog,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::variant<write_ops::UpdateCommandRequest,
                                       write_ops::DeleteCommandRequest>>& modificationOp,
    TimeseriesBatches* batches,
    const NamespaceString& bucketsNs,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds) {
    auto batchesToCommit = determineBatchesToCommit(*batches, extractFromSelf);
    if (batchesToCommit.empty()) {
        return;
    }

    Status abortStatus = Status::OK();
    ScopeGuard batchGuard{[&] {
        for (auto batch : batchesToCommit) {
            if (batch.get()) {
                abort(sideBucketCatalog, batch, abortStatus);
            }
        }
    }};

    try {
        std::vector<write_ops::InsertCommandRequest> insertOps;
        std::vector<write_ops::UpdateCommandRequest> updateOps;
        auto& mainBucketCatalog =
            bucket_catalog::GlobalBucketCatalog::get(opCtx->getServiceContext());
        for (auto batch : batchesToCommit) {
            auto metadata = getMetadata(sideBucketCatalog, batch.get()->bucketId);
            auto prepareCommitStatus =
                prepareCommit(sideBucketCatalog, batch, coll->getDefaultCollator());
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return;
            }

            TimeseriesStmtIds emptyStmtIds = {};
            write_ops_utils::makeWriteRequest(
                opCtx, batch, metadata, emptyStmtIds, bucketsNs, &insertOps, &updateOps);

            // Starts tracking the newly inserted bucket in the main bucket catalog as a direct
            // write to prevent other writers from modifying it.
            if (batch.get()->numPreviouslyCommittedMeasurements == 0) {
                directWriteStart(mainBucketCatalog.bucketStateRegistry, batch.get()->bucketId);
                bucketIds->insert(batch.get()->bucketId);
            }
        }

        performAtomicWrites(
            opCtx, coll, recordId, modificationOp, insertOps, updateOps, fromMigrate, stmtId);

        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
        getOpTimeAndElectionId(opCtx, &opTime, &electionId);

        for (auto batch : batchesToCommit) {
            finish(sideBucketCatalog, batch, bucket_catalog::CommitInfo{opTime, electionId});
            batch.get().reset();
        }
    } catch (...) {
        abortStatus = exceptionToStatus();
        throw;
    }

    batchGuard.dismiss();
}

void performAtomicWritesForDelete(OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  const RecordId& recordId,
                                  const std::vector<BSONObj>& unchangedMeasurements,
                                  bool fromMigrate,
                                  StmtId stmtId,
                                  Date_t currentMinTime) {
    OID bucketId = record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID();
    auto modificationOp =
        write_ops_utils::makeModificationOp(bucketId, coll, unchangedMeasurements, currentMinTime);
    performAtomicWrites(opCtx, coll, recordId, modificationOp, {}, {}, fromMigrate, stmtId);
}

void performAtomicWritesForUpdate(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    const RecordId& recordId,
    const boost::optional<std::vector<BSONObj>>& unchangedMeasurements,
    const std::vector<BSONObj>& modifiedMeasurements,
    bucket_catalog::BucketCatalog& sideBucketCatalog,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds,
    const CompressAndWriteBucketFunc& compressAndWriteBucketFunc,
    const boost::optional<Date_t> currentMinTime) {
    auto timeSeriesOptions = *coll->getTimeseriesOptions();
    auto batches = insertIntoBucketCatalogForUpdate(opCtx,
                                                    sideBucketCatalog,
                                                    coll,
                                                    modifiedMeasurements,
                                                    coll->ns(),
                                                    timeSeriesOptions,
                                                    compressAndWriteBucketFunc);

    auto modificationRequest = unchangedMeasurements
        ? boost::make_optional(write_ops_utils::makeModificationOp(
              record_id_helpers::toBSONAs(recordId, "_id")["_id"].OID(),
              coll,
              *unchangedMeasurements,
              currentMinTime))
        : boost::none;
    commitTimeseriesBucketsAtomically(opCtx,
                                      sideBucketCatalog,
                                      coll,
                                      recordId,
                                      modificationRequest,
                                      &batches,
                                      coll->ns(),
                                      fromMigrate,
                                      stmtId,
                                      bucketIds);
}

BSONObj timeseriesViewCommand(const BSONObj& cmd, std::string cmdName, StringData viewNss) {
    BSONObjBuilder b;
    for (auto&& e : cmd) {
        if (e.fieldNameStringData() == cmdName) {
            b.append(cmdName, viewNss);
        } else {
            b.append(e);
        }
    }
    return b.obj();
}

void deleteRequestCheckFunction(DeleteRequest* request, const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesDeletesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a delete with a non-empty query on a time-series "
                "collection that "
                "does not have a metaField ",
                options.getMetaField() || request->getQuery().isEmpty());

        uassert(ErrorCodes::IllegalOperation,
                "Cannot perform a non-multi delete on a time-series collection",
                request->getMulti());
        if (auto metaField = options.getMetaField()) {
            request->setQuery(timeseries::translateQuery(request->getQuery(), *metaField));
        }
    }
}

void updateRequestCheckFunction(UpdateRequest* request, const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesUpdatesSupport.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform a non-multi update on a time-series collection",
                request->isMulti());

        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform an upsert on a time-series collection",
                !request->isUpsert());

        auto metaField = options.getMetaField();
        uassert(ErrorCodes::InvalidOptions,
                "Cannot perform an update on a time-series collection that does not have a "
                "metaField",
                options.getMetaField());

        request->setQuery(timeseries::translateQuery(request->getQuery(), *metaField));
        auto modification = uassertStatusOK(
            timeseries::translateUpdate(request->getUpdateModification(), *metaField));
        request->setUpdateModification(modification);
    }
}

TimeseriesBatches insertBatchOfMeasurements(OperationContext* opCtx,
                                            bucket_catalog::BucketCatalog& catalog,
                                            const Collection* bucketsColl,
                                            const StringDataComparator* comparator,
                                            const std::vector<BSONObj>& measurements,
                                            bucket_catalog::InsertContext& insertContext) {

    return bucket_catalog::insertBatch(opCtx,
                                       catalog,
                                       bucketsColl,
                                       comparator,
                                       measurements,
                                       insertContext,
                                       getStorageCacheSizeBytes);
}

}  // namespace mongo::timeseries
