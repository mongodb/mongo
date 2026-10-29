// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/replica_set_write_block_op_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/db/topology/user_write_block/replica_set_writes_critical_section_document_gen.h"
#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace {

void tryPauseAutoCompactForReplicaSetWritesBlockOnSecondary(OperationContext* opCtx) {
    // This runs from the recovery unit's commit callback on the oplog-apply thread, which may
    // neither acquire locks nor block. The storage engine performs one non-blocking attempt.
    Status status = Status::OK();
    try {
        status = opCtx->getServiceContext()
                     ->getStorageEngine()
                     ->tryPauseAutoCompactForReplicaSetWritesBlock(opCtx);
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }

    if (status.isOK()) {
        LOGV2_INFO(13374200, "Requested auto-compaction pause for a replica set write block");
    }
}

}  // namespace

bool ReplicaSetWriteBlockOpObserver::_isReplSetAndCanAcceptWritesForNamespace(
    OperationContext* opCtx, const NamespaceString& nss) const {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    return replCoord->getSettings().isReplSet() && replCoord->canAcceptWritesFor(opCtx, nss);
}

void ReplicaSetWriteBlockOpObserver::_checkReplicaSetWriteAllowed(
    OperationContext* opCtx,
    const NamespaceString& nss,
    ReplicaSetWriteBlockRejectedWriteOp opType) {
    if (!_isReplSetAndCanAcceptWritesForNamespace(opCtx, nss)) {
        return;
    }

    ReplicaSetWriteBlockState::get(opCtx)->checkReplicaSetWritesAllowed(opCtx, nss, opType);
}

void ReplicaSetWriteBlockOpObserver::_checkReplicaSetDeleteAllowed(OperationContext* opCtx,
                                                                   const NamespaceString& nss) {
    if (!_isReplSetAndCanAcceptWritesForNamespace(opCtx, nss)) {
        return;
    }
    // Deletion blocking applies to all delete sources regardless of fromMigrate. This
    // intentionally blocks range deletions (orphan cleanup), which set fromMigrate = true,
    // and user and TTL deletions, which set fromMigrate = false.
    ReplicaSetWriteBlockState::get(opCtx)->checkReplicaSetDeletionsAllowed(opCtx, nss);
}

void ReplicaSetWriteBlockOpObserver::onInserts(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               std::vector<InsertStatement>::const_iterator first,
                                               std::vector<InsertStatement>::const_iterator last,
                                               const std::vector<RecordId>& recordIds,
                                               const std::vector<bool>& fromMigrate,
                                               bool defaultFromMigrate,
                                               OpStateAccumulator* opAccumulator) {
    const auto nss = coll->ns();

    _checkReplicaSetWriteAllowed(opCtx, nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert);

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kReplicaSetWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        const bool isPrimary = _isReplSetAndCanAcceptWritesForNamespace(opCtx, nss);
        for (auto it = first; it != last; ++it) {
            const auto& insertedDoc = it->doc;

            const auto collCSDoc = ReplicaSetWriteBlockingCriticalSectionDocument::parse(
                insertedDoc, IDLParserContext("ReplicaSetWriteBlockOpObserver"));

            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [blockWrites = collCSDoc.getEnabled(),
                 blockDeletions = !collCSDoc.getAllowDeletions(),
                 pauseOnSecondary = !collCSDoc.getAllowDeletions() && !isPrimary,
                 blockUserWritesReason = collCSDoc.getReplicaSetWritesBlockReason()](
                    OperationContext* opCtx, boost::optional<Timestamp>) {
                    auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
                    if (blockWrites) {
                        replicaSetWriteBlockState->enableReplicaSetWriteBlocking(
                            blockUserWritesReason);
                    }
                    if (blockDeletions) {
                        replicaSetWriteBlockState->enableReplicaSetDeletionsBlocking();
                        if (pauseOnSecondary) {
                            tryPauseAutoCompactForReplicaSetWritesBlockOnSecondary(opCtx);
                        }
                    }
                });
        }
    }
}

void ReplicaSetWriteBlockOpObserver::onUpdate(OperationContext* opCtx,
                                              const OplogUpdateEntryArgs& args,
                                              OpStateAccumulator* opAccumulator) {
    const auto nss = args.coll->ns();

    _checkReplicaSetWriteAllowed(opCtx, nss, ReplicaSetWriteBlockRejectedWriteOp::kUpdate);

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kReplicaSetWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        const bool isPrimary = _isReplSetAndCanAcceptWritesForNamespace(opCtx, nss);
        const auto collCSDoc = ReplicaSetWriteBlockingCriticalSectionDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ReplicaSetWriteBlockOpObserver"));

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [blockWrites = collCSDoc.getEnabled(),
             blockDeletions = !collCSDoc.getAllowDeletions(),
             pauseOnSecondary = !collCSDoc.getAllowDeletions() && !isPrimary,
             blockUserWritesReason = collCSDoc.getReplicaSetWritesBlockReason()](
                OperationContext* opCtx, boost::optional<Timestamp>) {
                auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
                if (blockWrites) {
                    // An allowDeletions-only update leaves write blocking active. Count the policy
                    // change without re-entering enableReplicaSetWriteBlocking (which only bumps
                    // on disabled->enabled).
                    if (!replicaSetWriteBlockState->isReplicaSetWriteBlockingEnabled()) {
                        replicaSetWriteBlockState->enableReplicaSetWriteBlocking(
                            blockUserWritesReason);
                    } else {
                        replicaSetWriteBlockState->incrementReplicaSetWritesBlockCounter(
                            blockUserWritesReason);
                    }
                }
                if (blockDeletions) {
                    replicaSetWriteBlockState->enableReplicaSetDeletionsBlocking();
                    if (pauseOnSecondary) {
                        tryPauseAutoCompactForReplicaSetWritesBlockOnSecondary(opCtx);
                    }
                } else {
                    replicaSetWriteBlockState->disableReplicaSetDeletionsBlocking();
                }
            });
    }
}

void ReplicaSetWriteBlockOpObserver::onDelete(OperationContext* opCtx,
                                              const CollectionPtr& coll,
                                              StmtId stmtId,
                                              const BSONObj& doc,
                                              const DocumentKey& documentKey,
                                              const OplogDeleteEntryArgs& args,
                                              OpStateAccumulator* opAccumulator) {
    const auto nss = coll->ns();
    _checkReplicaSetDeleteAllowed(opCtx, nss);

    if (nss == NamespaceString::kReplicaSetWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        invariant(!doc.isEmpty());
        const auto collCSDoc = ReplicaSetWriteBlockingCriticalSectionDocument::parse(
            doc, IDLParserContext("ReplicaSetWriteBlockOpObserver"));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [](OperationContext* opCtx, boost::optional<Timestamp>) {
                auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
                replicaSetWriteBlockState->disableReplicaSetWriteBlocking();
                replicaSetWriteBlockState->disableReplicaSetDeletionsBlocking();
            });
    }
}

void ReplicaSetWriteBlockOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                           const RollbackObserverInfo& rbInfo) {
    if (rbInfo.rollbackNamespaces.find(
            NamespaceString::kReplicaSetWritesCriticalSectionsNamespace) !=
        rbInfo.rollbackNamespaces.end()) {
        UserWritesRecoverableCriticalSectionService::get(opCtx)
            ->recoverReplicaSetWritesCriticalSection(opCtx);
    }
}

void ReplicaSetWriteBlockOpObserver::onStartIndexBuild(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       const UUID& collUUID,
                                                       const UUID& indexBuildUUID,
                                                       const std::vector<IndexBuildInfo>& indexes,
                                                       bool fromMigrate,
                                                       bool isTimeseries) {
    const auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collUUID);
    if (coll && coll->numRecords(opCtx) == 0) {
        return;
    }
    _checkReplicaSetWriteAllowed(opCtx, nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert);
}

void ReplicaSetWriteBlockOpObserver::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    const auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    if (coll && coll->numRecords(opCtx) == 0) {
        return;
    }
    _checkReplicaSetWriteAllowed(opCtx, nss, ReplicaSetWriteBlockRejectedWriteOp::kInsert);
}

}  // namespace mongo
