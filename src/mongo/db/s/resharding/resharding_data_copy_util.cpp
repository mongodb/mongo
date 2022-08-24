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

#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_progress_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/scopeguard.h"

namespace mongo::resharding::data_copy {

void ensureCollectionExists(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const CollectionOptions& options) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    writeConflictRetry(opCtx, "resharding::data_copy::ensureCollectionExists", nss.toString(), [&] {
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        if (coll) {
            return;
        }

        WriteUnitOfWork wuow(opCtx);
        coll.ensureDbExists(opCtx)->createCollection(opCtx, nss, options);
        wuow.commit();
    });
}

void ensureCollectionDropped(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    writeConflictRetry(
        opCtx, "resharding::data_copy::ensureCollectionDropped", nss.toString(), [&] {
            AutoGetCollection coll(opCtx, nss, MODE_X);
            if (!coll || (uuid && coll->uuid() != uuid)) {
                // If the collection doesn't exist or exists with a different UUID, then the
                // requested collection has been dropped already.
                return;
            }

            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(coll.getDb()->dropCollectionEvenIfSystem(
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
        ensureCollectionDropped(opCtx, stashNss);

        // Drop the oplog buffer collection for this donor.
        auto oplogBufferNss = getLocalOplogBufferNamespace(sourceUUID, donor.getShardId());
        ensureCollectionDropped(opCtx, oplogBufferNss);
    }
}

void ensureTemporaryReshardingCollectionRenamed(OperationContext* opCtx,
                                                const CommonReshardingMetadata& metadata) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // It is safe for resharding to drop and reacquire locks when checking for collection existence
    // because the coordinator will prevent two resharding operations from running for the same
    // namespace at the same time.
    auto tempReshardingNssExists = [&] {
        AutoGetCollection tempReshardingColl(opCtx, metadata.getTempReshardingNss(), MODE_IS);
        uassert(ErrorCodes::InvalidUUID,
                "Temporary resharding collection exists but doesn't have a UUID matching the"
                " resharding operation",
                !tempReshardingColl || tempReshardingColl->uuid() == metadata.getReshardingUUID());
        return bool(tempReshardingColl);
    }();

    if (!tempReshardingNssExists) {
        AutoGetCollection sourceColl(opCtx, metadata.getSourceNss(), MODE_IS);
        auto errmsg =
            "Temporary resharding collection doesn't exist and hasn't already been renamed"_sd;
        uassert(ErrorCodes::NamespaceNotFound, errmsg, sourceColl);
        uassert(
            ErrorCodes::InvalidUUID, errmsg, sourceColl->uuid() == metadata.getReshardingUUID());
        return;
    }

    RenameCollectionOptions options;
    options.dropTarget = true;
    options.markFromMigrate = true;
    uassertStatusOK(
        renameCollection(opCtx, metadata.getTempReshardingNss(), metadata.getSourceNss(), options));
}

Value findHighestInsertedId(OperationContext* opCtx, const CollectionPtr& collection) {
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
                                                       const CollectionPtr& collection) {
    // TODO SERVER-60824: Remove special handling for empty collections once non-blocking sort is
    // enabled on clustered collections.
    if (collection && collection->isEmpty(opCtx)) {
        return boost::none;
    }

    auto findCommand = std::make_unique<FindCommandRequest>(collection->ns());
    findCommand->setLimit(1);
    findCommand->setSort(BSON("_id" << -1));

    auto recordId = Helpers::findOne(opCtx, collection, std::move(findCommand));
    if (recordId.isNull()) {
        return boost::none;
    }

    auto doc = collection->docFor(opCtx, recordId).value();
    return Document{doc};
}

std::vector<InsertStatement> fillBatchForInsert(Pipeline& pipeline, int batchSizeLimitBytes) {
    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shards. It doing so requires the CurOp
    // to be marked as having started.
    auto* curOp = CurOp::get(pipeline.getContext()->opCtx);
    curOp->ensureStarted();
    ON_BLOCK_EXIT([curOp] { curOp->done(); });

    std::vector<InsertStatement> batch;

    int numBytes = 0;
    do {
        auto doc = pipeline.getNext();
        if (!doc) {
            break;
        }

        auto obj = doc->toBson();
        batch.emplace_back(obj.getOwned());
        numBytes += obj.objsize();
    } while (numBytes < batchSizeLimitBytes);

    return batch;
}

int insertBatch(OperationContext* opCtx,
                const NamespaceString& nss,
                std::vector<InsertStatement>& batch) {
    return writeConflictRetry(opCtx, "resharding::data_copy::insertBatch", nss.ns(), [&] {
        AutoGetCollection outputColl(opCtx, nss, MODE_IX);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection '" << nss << "' did not already exist",
                outputColl);

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
            opCtx, *outputColl, batch.begin(), batch.end(), nullptr));
        wuow.commit();

        return numBytes;
    });
}

boost::optional<SharedSemiFuture<void>> withSessionCheckedOut(OperationContext* opCtx,
                                                              LogicalSessionId lsid,
                                                              TxnNumber txnNumber,
                                                              boost::optional<StmtId> stmtId,
                                                              unique_function<void()> callable) {

    opCtx->setLogicalSessionId(std::move(lsid));
    opCtx->setTxnNumber(txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        txnParticipant.beginOrContinue(
            opCtx, {txnNumber}, boost::none /* autocommit */, boost::none /* startTransaction */);

        if (stmtId && txnParticipant.checkStatementExecuted(opCtx, *stmtId)) {
            // Skip the incoming statement because it has already been logged locally.
            return boost::none;
        }
    } catch (const ExceptionFor<ErrorCodes::TransactionTooOld>&) {
        // txnNumber < txnParticipant.o().activeTxnNumber
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
        // txnNumber == txnParticipant.o().activeTxnNumber &&
        // !txnParticipant.transactionIsInRetryableWriteMode()
        //
        // If the transaction chain is incomplete because the oplog was truncated, just ignore the
        // incoming write and don't attempt to "patch up" the missing pieces.
        //
        // This situation could also happen if the client reused the txnNumber for distinct
        // operations (which is a violation of the protocol). The client would receive an error if
        // they attempted to retry the retryable write they had reused the txnNumber with so it is
        // safe to leave config.transactions as-is.
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
        // txnParticipant.transactionIsPrepared()
        return txnParticipant.onExitPrepare();
    } catch (const ExceptionFor<ErrorCodes::RetryableTransactionInProgress>&) {
        // This is a retryable write that was executed using an internal transaction and there is
        // a retry in progress.
        return txnParticipant.onConflictingInternalTransactionCompletion(opCtx);
    }

    callable();
    return boost::none;
}

void updateSessionRecord(OperationContext* opCtx,
                         BSONObj o2Field,
                         std::vector<StmtId> stmtIds,
                         boost::optional<repl::OpTime> preImageOpTime,
                         boost::optional<repl::OpTime> postImageOpTime) {
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
    oplogEntry.setNss({});
    oplogEntry.setSessionId(sessionId);
    oplogEntry.setTxnNumber(txnNumber);
    oplogEntry.setStatementIds(stmtIds);
    oplogEntry.setPreImageOpTime(std::move(preImageOpTime));
    oplogEntry.setPostImageOpTime(std::move(postImageOpTime));
    oplogEntry.setPrevWriteOpTimeInTransaction(txnParticipant.getLastWriteOpTime());
    oplogEntry.setWallClockTime(opCtx->getServiceContext()->getFastClockSource()->now());
    oplogEntry.setFromMigrate(true);

    writeConflictRetry(
        opCtx,
        "resharding::data_copy::updateSessionRecord",
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        [&] {
            AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);

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

}  // namespace mongo::resharding::data_copy
