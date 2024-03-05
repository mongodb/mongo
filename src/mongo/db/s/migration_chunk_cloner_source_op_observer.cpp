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

#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_operation_source.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

// static
void MigrationChunkClonerSourceOpObserver::assertIntersectingChunkHasNotMoved(
    OperationContext* opCtx,
    const CollectionMetadata& metadata,
    const BSONObj& shardKey,
    const LogicalTime& atClusterTime) {
    // We can assume the simple collation because shard keys do not support non-simple collations.
    auto cmAtTimeOfWrite =
        ChunkManager::makeAtTime(*metadata.getChunkManager(), atClusterTime.asTimestamp());
    auto chunk = cmAtTimeOfWrite.findIntersectingChunkWithSimpleCollation(shardKey);

    // Throws if the chunk has moved since the timestamp of the running transaction's atClusterTime
    // read concern parameter.
    chunk.throwIfMoved();
}

// static
void MigrationChunkClonerSourceOpObserver::assertNoMovePrimaryInProgress(
    OperationContext* opCtx, const NamespaceString& nss) {
    if (!nss.isNormalCollection() && nss.coll() != "system.views" &&
        !nss.isTimeseriesBucketsCollection()) {
        return;
    }

    // TODO SERVER-58222: evaluate whether this is safe or whether acquiring the lock can block.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    Lock::DBLock dblock(opCtx, nss.dbName(), MODE_IS);

    const auto scopedDss =
        DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, nss.dbName());
    if (scopedDss->isMovePrimaryInProgress()) {
        LOGV2(4908600, "assertNoMovePrimaryInProgress", logAttrs(nss));

        uasserted(ErrorCodes::MovePrimaryInProgress,
                  "movePrimary is in progress for namespace " + nss.toStringForErrorMsg());
    }
}

void MigrationChunkClonerSourceOpObserver::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* const opAccumulator) {
    // Return early if we are secondary or in some replication state in which we are not
    // appending entries to the oplog.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    const auto& statements = transactionOperations.getOperationsForOpObserver();

    // It is possible that the transaction resulted in no changes.  In that case, we should
    // not write an empty applyOps entry.
    if (statements.empty()) {
        return;
    }

    if (!opAccumulator) {
        return;
    }

    const auto& commitOpTime = opAccumulator->opTime.writeOpTime;
    invariant(!commitOpTime.isNull());

    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogTransactionOperationsForShardingHandler>(
            *opCtx->getLogicalSessionId(), statements, commitOpTime));
}

void MigrationChunkClonerSourceOpObserver::onInserts(
    OperationContext* opCtx,
    const CollectionPtr& coll,
    std::vector<InsertStatement>::const_iterator first,
    std::vector<InsertStatement>::const_iterator last,
    const std::vector<RecordId>& recordIds,
    std::vector<bool> fromMigrate,
    bool defaultFromMigrate,
    OpStateAccumulator* opAccumulator) {
    // Take ownership of ShardingWriteRouter attached to the op accumulator by OpObserverImpl.
    // Release upon return from this function because this resource is not needed by downstream
    // OpObserver instances.
    // If there's no ShardingWriteRouter instance available, it means that OpObserverImpl did not
    // get far enough to require one so there's nothing to do here but return early.
    auto shardingWriteRouter =
        std::move(shardingWriteRouterOpStateAccumulatorDecoration(opAccumulator));
    if (!shardingWriteRouter) {
        return;
    }

    if (defaultFromMigrate) {
        return;
    }

    const auto& nss = coll->ns();
    if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
        return;
    }

    auto* const css = shardingWriteRouter->getCss();
    css->checkShardVersionOrThrow(opCtx);
    DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.dbName());

    auto* const csr = checked_cast<CollectionShardingRuntime*>(css);
    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        MigrationChunkClonerSourceOpObserver::assertNoMovePrimaryInProgress(opCtx, nss);
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();
    if (inMultiDocumentTransaction && !shard_role_details::getWriteUnitOfWork(opCtx)) {
        return;
    }

    int index = 0;
    const auto& opTimeList = opAccumulator->insertOpTimes;
    for (auto it = first; it != last; it++, index++) {
        auto opTime = opTimeList.empty() ? repl::OpTime() : opTimeList[index];

        if (inMultiDocumentTransaction) {
            const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

            if (atClusterTime) {
                const auto shardKey =
                    metadata->getShardKeyPattern().extractShardKeyFromDocThrows(it->doc);
                MigrationChunkClonerSourceOpObserver::assertIntersectingChunkHasNotMoved(
                    opCtx, *metadata, shardKey, *atClusterTime);
            }

            continue;
        }

        shard_role_details::getRecoveryUnit(opCtx)->registerChange(
            std::make_unique<LogInsertForShardingHandler>(nss, it->doc, opTime));
    }
}

void MigrationChunkClonerSourceOpObserver::onUpdate(OperationContext* opCtx,
                                                    const OplogUpdateEntryArgs& args,
                                                    OpStateAccumulator* opAccumulator) {
    // Take ownership of ShardingWriteRouter attached to the op accumulator by OpObserverImpl.
    // Release upon return from this function because this resource is not needed by downstream
    // OpObserver instances.
    // If there's no ShardingWriteRouter instance available, it means that OpObserverImpl did not
    // get far enough to require one so there's nothing to do here but return early.
    auto shardingWriteRouter =
        std::move(shardingWriteRouterOpStateAccumulatorDecoration(opAccumulator));
    if (!shardingWriteRouter) {
        return;
    }

    if (args.updateArgs->source == OperationSource::kFromMigrate) {
        return;
    }

    if (args.updateArgs->update.isEmpty()) {
        return;
    }

    const auto& nss = args.coll->ns();
    if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
        return;
    }

    const auto& preImageDoc = args.updateArgs->preImageDoc;
    const auto& postImageDoc = args.updateArgs->updatedDoc;

    auto* const css = shardingWriteRouter->getCss();
    css->checkShardVersionOrThrow(opCtx);
    DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.dbName());

    auto* const csr = checked_cast<CollectionShardingRuntime*>(css);
    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        MigrationChunkClonerSourceOpObserver::assertNoMovePrimaryInProgress(opCtx, nss);
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();
    if (inMultiDocumentTransaction) {
        if (auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime()) {
            const auto shardKey =
                metadata->getShardKeyPattern().extractShardKeyFromDocThrows(postImageDoc);
            MigrationChunkClonerSourceOpObserver::assertIntersectingChunkHasNotMoved(
                opCtx, *metadata, shardKey, *atClusterTime);
        }

        return;
    }

    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogUpdateForShardingHandler>(
            nss, preImageDoc, postImageDoc, opAccumulator->opTime.writeOpTime));
}

void MigrationChunkClonerSourceOpObserver::onDelete(OperationContext* opCtx,
                                                    const CollectionPtr& coll,
                                                    StmtId stmtId,
                                                    const BSONObj& doc,
                                                    const OplogDeleteEntryArgs& args,
                                                    OpStateAccumulator* opAccumulator) {
    if (args.fromMigrate) {
        return;
    }

    const auto& nss = coll->ns();
    if (nss == NamespaceString::kSessionTransactionsTableNamespace) {
        return;
    }

    ShardingWriteRouter shardingWriteRouter(opCtx, nss);
    auto* const css = shardingWriteRouter.getCss();
    css->checkShardVersionOrThrow(opCtx);
    DatabaseShardingState::assertMatchingDbVersion(opCtx, nss.dbName());

    auto* const csr = checked_cast<CollectionShardingRuntime*>(css);
    auto metadata = csr->getCurrentMetadataIfKnown();
    if (!metadata || !metadata->isSharded()) {
        assertNoMovePrimaryInProgress(opCtx, nss);
        return;
    }

    auto optDocKey = documentKeyDecoration(args);
    invariant(optDocKey, nss.toStringForErrorMsg());
    auto shardKeyAndId = optDocKey.value().getShardKeyAndId();

    auto txnParticipant = TransactionParticipant::get(opCtx);
    const bool inMultiDocumentTransaction =
        txnParticipant && opCtx->writesAreReplicated() && txnParticipant.transactionIsOpen();
    if (inMultiDocumentTransaction) {
        const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

        if (atClusterTime) {
            const auto shardKey =
                metadata->getShardKeyPattern().extractShardKeyFromDocumentKeyThrows(shardKeyAndId);
            assertIntersectingChunkHasNotMoved(opCtx, *metadata, shardKey, *atClusterTime);
        }

        return;
    }

    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogDeleteForShardingHandler>(
            nss, *optDocKey, opAccumulator->opTime.writeOpTime));
}

void MigrationChunkClonerSourceOpObserver::postTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations) {
    // Return early if we are secondary or in some replication state in which we are not
    // appending entries to the oplog.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    if (reservedSlots.empty()) {
        return;
    }

    const auto& prepareOpTime = reservedSlots.back();
    invariant(!prepareOpTime.isNull());

    const auto& statements = transactionOperations.getOperationsForOpObserver();

    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogTransactionOperationsForShardingHandler>(
            *opCtx->getLogicalSessionId(), statements, prepareOpTime));
}

void MigrationChunkClonerSourceOpObserver::onTransactionPrepareNonPrimary(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    const std::vector<repl::OplogEntry>& statements,
    const repl::OpTime& prepareOpTime) {
    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogTransactionOperationsForShardingHandler>(
            lsid, statements, prepareOpTime));
}

void MigrationChunkClonerSourceOpObserver::onBatchedWriteCommit(
    OperationContext* opCtx,
    WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
    OpStateAccumulator* opAccumulator) {
    // Return early if we are secondary or in some replication state in which we are not
    // appending entries to the oplog.
    if (!opCtx->writesAreReplicated()) {
        return;
    }

    // Return early if this isn't a retryable batched write.
    if (!opAccumulator || opAccumulator->insertOpTimes.empty() ||
        oplogGroupingFormat != WriteUnitOfWork::kGroupForPossiblyRetryableOperations ||
        !opCtx->getTxnNumber() || !opCtx->getLogicalSessionId()) {
        return;
    }

    auto txnParticipant = TransactionParticipant::get(opCtx);
    invariant(txnParticipant);
    const auto affectedNamespaces = txnParticipant.affectedNamespaces();
    std::vector<NamespaceString> namespaces(affectedNamespaces.begin(), affectedNamespaces.end());

    shard_role_details::getRecoveryUnit(opCtx)->registerChange(
        std::make_unique<LogRetryableApplyOpsForShardingHandler>(std::move(namespaces),
                                                                 opAccumulator->insertOpTimes));
}

}  // namespace mongo
