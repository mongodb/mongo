/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_data_copy_util.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/rename_collection.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/query/write_ops/update.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_fetcher_progress_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <mutex>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo::resharding::data_copy {

void ensureCollectionExists(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    writeConflictRetry(opCtx, "resharding::data_copy::ensureCollectionExists", nss, [&] {
        AutoGetDb db(opCtx, nss.dbName(), MODE_IX);
        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        if (coll.exists()) {
            return;
        }
        WriteUnitOfWork wuow(opCtx);
        db.ensureDbExists(opCtx)->createCollection(opCtx, nss, options);
        wuow.commit();
    });
}

void ensureCollectionDropped(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    writeConflictRetry(opCtx, "resharding::data_copy::ensureCollectionDropped", nss, [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        if (!coll.exists() || (uuid && coll.uuid() != uuid)) {
            // If the collection doesn't exist or exists with a different UUID, then the
            // requested collection has been dropped already.
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        uassertStatusOK(autoDb.getDb()->dropCollectionEvenIfSystem(
            opCtx, nss, {} /* dropOpTime */, true /* markFromMigrate */));
        wuow.commit();
    });
}

void ensureOplogCollectionsDropped(OperationContext* opCtx,
                                   const UUID& reshardingUUID,
                                   const UUID& sourceUUID,
                                   const std::vector<DonorShardFetchTimestamp>& donorShards) {
    for (const auto& donor : donorShards) {
        auto reshardingSourceId = ReshardingSourceId{reshardingUUID, donor.getShardId()};

        // Remove the oplog fetcher progress doc for this donor.
        PersistentTaskStore<ReshardingOplogFetcherProgress> oplogFetcherProgressStore(
            NamespaceString::kReshardingFetcherProgressNamespace);
        oplogFetcherProgressStore.remove(
            opCtx,
            BSON(ReshardingOplogFetcherProgress::kOplogSourceIdFieldName
                 << reshardingSourceId.toBSON()),
            WriteConcernOptions());

        // Remove the oplog applier progress doc for this donor.
        PersistentTaskStore<ReshardingOplogApplierProgress> oplogApplierProgressStore(
            NamespaceString::kReshardingApplierProgressNamespace);
        oplogApplierProgressStore.remove(
            opCtx,
            BSON(ReshardingOplogApplierProgress::kOplogSourceIdFieldName
                 << reshardingSourceId.toBSON()),
            WriteConcernOptions());

        // Remove the txn cloner progress doc for this donor.
        PersistentTaskStore<ReshardingTxnClonerProgress> txnClonerProgressStore(
            NamespaceString::kReshardingTxnClonerProgressNamespace);
        txnClonerProgressStore.remove(
            opCtx,
            BSON(ReshardingTxnClonerProgress::kSourceIdFieldName << reshardingSourceId.toBSON()),
            WriteConcernOptions());

        // Drop the conflict stash collection for this donor.
        auto stashNss = getLocalConflictStashNamespace(sourceUUID, donor.getShardId());
        resharding::data_copy::ensureCollectionDropped(opCtx, stashNss);

        // Drop the oplog buffer collection for this donor.
        auto oplogBufferNss = getLocalOplogBufferNamespace(sourceUUID, donor.getShardId());
        resharding::data_copy::ensureCollectionDropped(opCtx, oplogBufferNss);
    }
}

void ensureTemporaryReshardingCollectionRenamed(OperationContext* opCtx,
                                                const CommonReshardingMetadata& metadata) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    // It is safe for resharding to drop and reacquire locks when checking for collection existence
    // because the coordinator will prevent two resharding operations from running for the same
    // namespace at the same time.

    auto tempReshardingNssExists = [&] {
        const auto tempReshardingColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                opCtx, metadata.getTempReshardingNss(), AcquisitionPrerequisites::kRead),
            MODE_IS);
        uassert(ErrorCodes::InvalidUUID,
                "Temporary resharding collection exists but doesn't have a UUID matching the"
                " resharding operation",
                !tempReshardingColl.exists() ||
                    tempReshardingColl.uuid() == metadata.getReshardingUUID());
        return tempReshardingColl.exists();
    }();

    if (!tempReshardingNssExists) {
        const auto sourceColl =
            acquireCollection(opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  opCtx, metadata.getSourceNss(), AcquisitionPrerequisites::kRead),
                              MODE_IS);
        auto errmsg =
            "Temporary resharding collection doesn't exist and hasn't already been renamed"_sd;
        uassert(ErrorCodes::NamespaceNotFound, errmsg, sourceColl.exists());
        uassert(ErrorCodes::InvalidUUID, errmsg, sourceColl.uuid() == metadata.getReshardingUUID());
        return;
    }

    RenameCollectionOptions options;
    options.dropTarget = true;
    options.markFromMigrate = true;
    uassertStatusOK(
        renameCollection(opCtx, metadata.getTempReshardingNss(), metadata.getSourceNss(), options));
}

bool isCollectionCapped(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound,
            "Temporary resharding collection doesn't exist."_sd,
            coll.exists());
    return coll.getCollectionPtr()->isCapped();
}

void deleteRecipientResumeData(OperationContext* opCtx, const UUID& reshardingUUID) {
    writeConflictRetry(
        opCtx,
        "resharding::data_copy::deleteRecipientResumeData",
        NamespaceString::kRecipientReshardingResumeDataNamespace,
        [&] {
            const auto coll =
                acquireCollection(opCtx,
                                  CollectionAcquisitionRequest(
                                      NamespaceString::kRecipientReshardingResumeDataNamespace,
                                      PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                      repl::ReadConcernArgs::get(opCtx),
                                      AcquisitionPrerequisites::kWrite),
                                  MODE_IX);
            if (!coll.exists())
                return;
            deleteObjects(opCtx,
                          coll,
                          BSON(ReshardingRecipientResumeData::kIdFieldName + "." +
                                   ReshardingRecipientResumeDataId::kReshardingUUIDFieldName
                               << reshardingUUID),
                          false /* justOne */);
        });
}

Value findHighestInsertedId(OperationContext* opCtx, const CollectionAcquisition& collection) {
    auto doc = findDocWithHighestInsertedId(opCtx, collection);
    if (!doc) {
        return Value{};
    }

    auto value = (*doc)["_id"];
    uassert(4929300,
            "Missing _id field for document in temporary resharding collection",
            !value.missing());

    return value;
}

boost::optional<Document> findDocWithHighestInsertedId(OperationContext* opCtx,
                                                       const CollectionAcquisition& collection) {
    if (collection.exists() && collection.getCollectionPtr()->isEmpty(opCtx)) {
        return boost::none;
    }

    auto findCommand = std::make_unique<FindCommandRequest>(collection.nss());
    findCommand->setLimit(1);
    findCommand->setSort(BSON("_id" << -1));

    auto recordId = Helpers::findOne(opCtx, collection, std::move(findCommand));
    if (recordId.isNull()) {
        return boost::none;
    }

    auto doc = collection.getCollectionPtr()->docFor(opCtx, recordId).value();
    return Document{doc};
}

int insertBatchTransactionally(OperationContext* opCtx,
                               const NamespaceString& nss,
                               TxnNumber& txnNumber,
                               std::vector<InsertStatement>& batch,
                               const UUID& reshardingUUID,
                               const ShardId& donorShard,
                               const HostAndPort& donorHost,
                               const BSONObj& resumeToken,
                               bool storeProgress) {
    long long numBytes = 0;
    int attempt = 1;
    for (auto insert = batch.begin(); insert != batch.end(); ++insert) {
        numBytes += insert->doc.objsize();
    }
    LOGV2_DEBUG(7763605,
                3,
                "resharding_data_copy_util::insertBatchTransactionally",
                "reshardingUUID"_attr = reshardingUUID,
                "reshardingTmpNss"_attr = nss,
                "batchSizeBytes"_attr = numBytes,
                "txnNumber"_attr = txnNumber,
                "donorShard"_attr = donorShard,
                "donorHost"_attr = donorHost,
                "resumeToken"_attr = resumeToken,
                "storeProgress"_attr = storeProgress);

    while (true) {
        try {
            ++txnNumber;
            opCtx->setTxnNumber(txnNumber);
            runWithTransactionFromOpCtx(opCtx, nss, [&](OperationContext* opCtx) {
                const auto outputColl =
                    acquireCollection(opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx, nss, AcquisitionPrerequisites::kWrite),
                                      MODE_IX);
                uassert(ErrorCodes::NamespaceNotFound,
                        str::stream() << "Collection '" << nss.toStringForErrorMsg()
                                      << "' did not already exist",
                        outputColl.exists());

                if (numBytes > 0) {
                    // It is legal to have an empty batch; in that case we should still write the
                    // resume token.
                    uassertStatusOK(collection_internal::insertDocuments(
                        opCtx, outputColl.getCollectionPtr(), batch.begin(), batch.end(), nullptr));
                }

                auto resumeDataColl = acquireCollection(
                    opCtx,
                    CollectionAcquisitionRequest::fromOpCtx(
                        opCtx,
                        static_cast<const NamespaceString&>(
                            NamespaceString::kRecipientReshardingResumeDataNamespace),
                        AcquisitionPrerequisites::kWrite),
                    MODE_IX);

                auto resumeDataIdBSON =
                    ReshardingRecipientResumeDataId{reshardingUUID, donorShard}.toBSON();
                auto query = BSON(ReshardingRecipientResumeData::kIdFieldName << resumeDataIdBSON);
                BSONObjBuilder updateModBuilder;
                updateModBuilder.append(
                    "$set",
                    BSON(ReshardingRecipientResumeData::kIdFieldName
                         << resumeDataIdBSON << ReshardingRecipientResumeData::kDonorHostFieldName
                         << donorHost.toString()
                         << ReshardingRecipientResumeData::kResumeTokenFieldName << resumeToken));
                if (storeProgress) {
                    updateModBuilder.append(
                        "$inc",
                        BSON(ReshardingRecipientResumeData::kDocumentsCopiedFieldName
                             << static_cast<long long>(batch.size())
                             << ReshardingRecipientResumeData::kBytesCopiedFieldName << numBytes));
                }

                UpdateRequest updateRequest;
                updateRequest.setNamespaceString(
                    NamespaceString::kRecipientReshardingResumeDataNamespace);
                updateRequest.setQuery(query);
                updateRequest.setUpdateModification(updateModBuilder.obj());
                updateRequest.setUpsert(true);
                updateRequest.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
                UpdateResult ur = update(opCtx, resumeDataColl, updateRequest);
                // When we have new data we always expect to make progress.  If we don't,
                // we just inserted duplicate documents, so fail.
                invariant(ur.numDocsModified == 1 || !ur.upsertedId.isEmpty() || numBytes == 0);
            });
            return numBytes;
        } catch (const DBException& ex) {
            // Stale config errors requires that we refresh shard version, not just try again, so
            // we let the layer above handle them. We set isCommitOrAbort to true to avoid retrying
            // on errors in ErrorCodes::isRetriableError like InterruptedDueToReplStateChange.
            if (ErrorCodes::isStaleShardVersionError(ex.code()) ||
                !isTransientTransactionError(
                    ex.code(), false /* hasWriteConcernError */, true /* isCommitOrAbort */))
                throw;
            logAndBackoff(7973400,
                          MONGO_LOGV2_DEFAULT_COMPONENT,
                          logv2::LogSeverity::Debug(1),
                          attempt++,
                          "Transient transaction error while inserting data, retrying.",
                          "reason"_attr = redact(ex.toStatus()));
        }
    }
}

int insertBatch(OperationContext* opCtx,
                const NamespaceString& nss,
                std::vector<InsertStatement>& batch) {
    return writeConflictRetry(opCtx, "resharding::data_copy::insertBatch", nss, [&] {
        const auto outputColl = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection '" << nss.toStringForErrorMsg()
                              << "' did not already exist",
                outputColl.exists());

        int numBytes = 0;
        WriteUnitOfWork wuow(opCtx);

        // Populate 'slots' with new optimes for each insert.
        // This also notifies the storage engine of each new timestamp.
        auto oplogSlots = repl::getNextOpTimes(opCtx, batch.size());
        for (auto [insert, slot] = std::make_pair(batch.begin(), oplogSlots.begin());
             slot != oplogSlots.end();
             ++insert, ++slot) {
            invariant(insert != batch.end());
            insert->oplogSlot = *slot;
            numBytes += insert->doc.objsize();
        }

        uassertStatusOK(collection_internal::insertDocuments(
            opCtx, outputColl.getCollectionPtr(), batch.begin(), batch.end(), nullptr));
        wuow.commit();

        return numBytes;
    });
}

void runWithTransactionFromOpCtx(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 unique_function<void(OperationContext*)> func) {
    auto* const client = opCtx->getClient();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    AuthorizationSession::get(client)->grantInternalAuthorization();
    TxnNumber txnNumber = *opCtx->getTxnNumber();
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);

    ScopeGuard guard([opCtx, &txnParticipant] {
        try {
            if (txnParticipant.transactionIsInProgress()) {
                txnParticipant.abortTransaction(opCtx);
            }
        } catch (DBException& e) {
            LOGV2_WARNING(
                4990200,
                "Failed to abort transaction in resharding::data_copy::runWithTransaction",
                "error"_attr = redact(e));
        }
    });

    txnParticipant.beginOrContinue(opCtx,
                                   {txnNumber},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    txnParticipant.unstashTransactionResources(opCtx, "reshardingOplogApplication");

    func(opCtx);

    if (!txnParticipant.retrieveCompletedTransactionOperations(opCtx)->isEmpty()) {
        // Similar to the `isTimestamped` check in `applyOperation`, we only want to commit the
        // transaction if we're doing replicated writes.
        txnParticipant.commitUnpreparedTransaction(opCtx);
    } else {
        txnParticipant.abortTransaction(opCtx);
    }
    txnParticipant.stashTransactionResources(opCtx);

    guard.dismiss();
}

void updateSessionRecord(OperationContext* opCtx,
                         BSONObj o2Field,
                         std::vector<StmtId> stmtIds,
                         boost::optional<repl::OpTime> preImageOpTime,
                         boost::optional<repl::OpTime> postImageOpTime,
                         NamespaceString sourceNss) {
    invariant(opCtx->getLogicalSessionId());
    invariant(opCtx->getTxnNumber());

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant, "Must be called with session checked out");

    const auto sessionId = *opCtx->getLogicalSessionId();
    const auto txnNumber = *opCtx->getTxnNumber();

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setObject(SessionCatalogMigration::kSessionOplogTag);
    oplogEntry.setObject2(std::move(o2Field));
    oplogEntry.setNss(std::move(sourceNss));
    oplogEntry.setSessionId(sessionId);
    oplogEntry.setTxnNumber(txnNumber);
    oplogEntry.setStatementIds(stmtIds);
    oplogEntry.setPreImageOpTime(std::move(preImageOpTime));
    oplogEntry.setPostImageOpTime(std::move(postImageOpTime));
    oplogEntry.setPrevWriteOpTimeInTransaction(txnParticipant.getLastWriteOpTime());
    oplogEntry.setWallClockTime(opCtx->fastClockSource().now());
    oplogEntry.setFromMigrate(true);

    writeConflictRetry(
        opCtx,
        "resharding::data_copy::updateSessionRecord",
        NamespaceString::kSessionTransactionsTableNamespace,
        [&] {
            AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);

            WriteUnitOfWork wuow(opCtx);
            repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);

            uassert(4989901,
                    str::stream() << "Failed to create new oplog entry: "
                                  << redact(oplogEntry.toBSON()),
                    !opTime.isNull());

            // Use the same wallTime as the oplog entry since SessionUpdateTracker
            // looks at the oplog entry wallTime when replicating.
            SessionTxnRecord sessionTxnRecord(
                sessionId, txnNumber, std::move(opTime), oplogEntry.getWallClockTime());
            if (isInternalSessionForRetryableWrite(sessionId)) {
                sessionTxnRecord.setParentSessionId(*getParentSessionId(sessionId));
            }

            txnParticipant.onRetryableWriteCloningCompleted(opCtx, stmtIds, sessionTxnRecord);

            wuow.commit();
        });
}

std::vector<ReshardingRecipientResumeData> getRecipientResumeData(OperationContext* opCtx,
                                                                  const UUID& reshardingUUID) {
    DBDirectClient client(opCtx);
    FindCommandRequest findCommand(NamespaceString::kRecipientReshardingResumeDataNamespace);
    const auto filterField = ReshardingRecipientResumeData::kIdFieldName + "." +
        mongo::ReshardingRecipientResumeDataId::kReshardingUUIDFieldName;
    findCommand.setFilter(BSON(filterField << reshardingUUID));
    auto cursor = client.find(findCommand, ReadPreferenceSetting{}, ExhaustMode::kOff);
    std::vector<ReshardingRecipientResumeData> results;
    while (cursor->more()) {
        auto obj = cursor->nextSafe();
        results.emplace_back(ReshardingRecipientResumeData::parseOwned(
            std::move(obj), IDLParserContext("resharding::data_copy::getRecipientResumeData")));
    }
    return results;
}

}  // namespace mongo::resharding::data_copy
