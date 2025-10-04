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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/local_catalog/collection_operation_source.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_exec_util.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/global_bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/timeseries_update_delete_util.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils_internal.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

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

uint64_t getStorageCacheSizeBytes(OperationContext* opCtx) {
    return opCtx->getServiceContext()->getStorageEngine()->getEngine()->getCacheSizeMB() * 1024 *
        1024;
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

std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
determineBatchesToCommit(bucket_catalog::TimeseriesWriteBatches& batches) {
    stdx::unordered_set<bucket_catalog::WriteBatch*> processedBatches;
    std::vector<std::reference_wrapper<std::shared_ptr<timeseries::bucket_catalog::WriteBatch>>>
        batchesToCommit;

    for (auto& batch : batches) {
        if (!processedBatches.contains(batch.get())) {
            batchesToCommit.push_back(batch);
            processedBatches.insert(batch.get());
        }
    }

    // Sort by bucket so that preparing the commit for each batch cannot deadlock.
    std::sort(batchesToCommit.begin(), batchesToCommit.end(), [](auto left, auto right) {
        return left.get()->bucketId.oid < right.get()->bucketId.oid;
    });

    return batchesToCommit;
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
    bucket_catalog::TimeseriesWriteBatches* batches,
    const NamespaceString& bucketsNs,
    bool fromMigrate,
    StmtId stmtId,
    std::set<bucket_catalog::BucketId>* bucketIds) {
    auto batchesToCommit = determineBatchesToCommit(*batches);
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
            auto prepareCommitStatus =
                prepareCommit(sideBucketCatalog, batch, coll->getDefaultCollator());
            if (!prepareCommitStatus.isOK()) {
                abortStatus = prepareCommitStatus;
                return;
            }

            write_ops_utils::makeWriteRequestFromBatch(
                opCtx, batch, bucketsNs, &insertOps, &updateOps);

            // Starts tracking the newly inserted bucket in the main bucket catalog as a direct
            // write to prevent other writers from modifying it.
            if (batch.get()->numPreviouslyCommittedMeasurements == 0) {
                directWriteStart(mainBucketCatalog.bucketStateRegistry, batch.get()->bucketId);
                bucketIds->insert(batch.get()->bucketId);
            }
        }

        performAtomicWrites(
            opCtx, coll, recordId, modificationOp, insertOps, updateOps, fromMigrate, stmtId);

        for (auto batch : batchesToCommit) {
            finish(sideBucketCatalog, batch);
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
    const boost::optional<Date_t> currentMinTime) {
    auto timeseriesOptions = *coll->getTimeseriesOptions();
    auto storageCacheSizeBytes = getStorageCacheSizeBytes(opCtx);
    std::vector<bucket_catalog::WriteStageErrorAndIndex> errorsAndIndices;

    auto swWriteBatches =
        bucket_catalog::prepareInsertsToBuckets(opCtx,
                                                sideBucketCatalog,
                                                coll.get(),
                                                timeseriesOptions,
                                                opCtx->getOpID(),
                                                coll->getDefaultCollator(),
                                                storageCacheSizeBytes,
                                                /*earlyReturnOnError=*/true,
                                                /*compressAndWriteBucketFunc=*/nullptr,
                                                modifiedMeasurements,
                                                0,
                                                modifiedMeasurements.size(),
                                                {},
                                                bucket_catalog::AllowQueryBasedReopening::kAllow,
                                                errorsAndIndices);
    uassertStatusOK(swWriteBatches);

    auto& batches = swWriteBatches.getValue();

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

void deleteRequestCheckFunction(const VersionContext& vCtx,
                                DeleteRequest* request,
                                const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesDeletesSupport.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
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

void updateRequestCheckFunction(const VersionContext& vCtx,
                                UpdateRequest* request,
                                const TimeseriesOptions& options) {
    if (!feature_flags::gTimeseriesUpdatesSupport.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
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

        timeseriesCounters.incrementMetaUpdate();

        request->setQuery(timeseries::translateQuery(request->getQuery(), *metaField));
        auto modification = uassertStatusOK(
            timeseries::translateUpdate(request->getUpdateModification(), *metaField));
        request->setUpdateModification(modification);
    }
}
}  // namespace mongo::timeseries
