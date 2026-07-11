// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/user_write_block/user_write_block_mode_op_observer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/topology/user_write_block/global_user_write_block_state.h"
#include "mongo/db/topology/user_write_block/user_writes_critical_section_document_gen.h"
#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

void UserWriteBlockModeOpObserver::onInserts(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             std::vector<InsertStatement>::const_iterator first,
                                             std::vector<InsertStatement>::const_iterator last,
                                             const std::vector<RecordId>& recordIds,
                                             std::vector<bool> fromMigrate,
                                             bool defaultFromMigrate,
                                             OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();

    if (!defaultFromMigrate && nss != NamespaceString::kReplicaSetWritesCriticalSectionsNamespace) {
        _checkWriteAllowed(opCtx, nss);
    }

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        for (auto it = first; it != last; ++it) {
            const auto& insertedDoc = it->doc;

            const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
                insertedDoc, IDLParserContext("UserWriteBlockOpObserver"));
            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [blockShardedDDL = collCSDoc.getBlockNewUserShardedDDL(),
                 blockWrites = collCSDoc.getBlockUserWrites(),
                 blockUserWritesReason = collCSDoc.getBlockUserWritesReason().value_or(
                     UserWritesBlockReasonEnum::kUnspecified)](OperationContext* opCtx,
                                                               boost::optional<Timestamp>) {
                    if (blockShardedDDL) {
                        GlobalUserWriteBlockState::get(opCtx)->enableUserShardedDDLBlocking(opCtx);
                    }

                    if (blockWrites) {
                        GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(
                            opCtx, blockUserWritesReason);
                    }
                });
        }
    }
}

void UserWriteBlockModeOpObserver::onUpdate(OperationContext* opCtx,
                                            const OplogUpdateEntryArgs& args,
                                            OpStateAccumulator* opAccumulator) {
    const auto& nss = args.coll->ns();

    if (args.updateArgs->source != OperationSource::kFromMigrate &&
        nss != NamespaceString::kReplicaSetWritesCriticalSectionsNamespace) {
        _checkWriteAllowed(opCtx, nss);
    }

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("UserWriteBlockOpObserver"));

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [blockShardedDDL = collCSDoc.getBlockNewUserShardedDDL(),
             blockWrites = collCSDoc.getBlockUserWrites(),
             blockUserWritesReason = collCSDoc.getBlockUserWritesReason().value_or(
                 UserWritesBlockReasonEnum::kUnspecified)](OperationContext* opCtx,
                                                           boost::optional<Timestamp>) {
                if (blockShardedDDL) {
                    GlobalUserWriteBlockState::get(opCtx)->enableUserShardedDDLBlocking(opCtx);
                } else {
                    GlobalUserWriteBlockState::get(opCtx)->disableUserShardedDDLBlocking(opCtx);
                }

                if (blockWrites) {
                    GlobalUserWriteBlockState::get(opCtx)->enableUserWriteBlocking(
                        opCtx, blockUserWritesReason);
                } else {
                    GlobalUserWriteBlockState::get(opCtx)->disableUserWriteBlocking(opCtx);
                }
            });
    }
}

void UserWriteBlockModeOpObserver::onDelete(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            StmtId stmtId,
                                            const BSONObj& doc,
                                            const DocumentKey& documentKey,
                                            const OplogDeleteEntryArgs& args,
                                            OpStateAccumulator* opAccumulator) {
    const auto& nss = coll->ns();
    if (!args.fromMigrate && nss != NamespaceString::kReplicaSetWritesCriticalSectionsNamespace) {
        _checkWriteAllowed(opCtx, nss);
    }

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kUserWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        const auto& deletedDoc = doc;
        invariant(!deletedDoc.isEmpty());
        const auto collCSDoc = UserWriteBlockingCriticalSectionDocument::parse(
            deletedDoc, IDLParserContext("UserWriteBlockOpObserver"));

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [](OperationContext* opCtx, boost::optional<Timestamp> _) {
                GlobalUserWriteBlockState::get(opCtx)->disableUserShardedDDLBlocking(opCtx);
                GlobalUserWriteBlockState::get(opCtx)->disableUserWriteBlocking(opCtx);
            });
    }
}

void UserWriteBlockModeOpObserver::onReplicationRollback(OperationContext* opCtx,
                                                         const RollbackObserverInfo& rbInfo) {
    if (rbInfo.rollbackNamespaces.find(NamespaceString::kUserWritesCriticalSectionsNamespace) !=
        rbInfo.rollbackNamespaces.end()) {
        UserWritesRecoverableCriticalSectionService::get(opCtx)->recoverRecoverableCriticalSections(
            opCtx);
    }
}

void UserWriteBlockModeOpObserver::onCreateIndex(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const UUID& uuid,
                                                 const IndexBuildInfo& indexBuildInfo,
                                                 bool fromMigrate,
                                                 bool isTimeseries) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::onStartIndexBuild(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     const UUID& collUUID,
                                                     const UUID& indexBuildUUID,
                                                     const std::vector<IndexBuildInfo>& indexes,
                                                     bool fromMigrate,
                                                     bool isTimeseries) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                                                const NamespaceString& nss) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::onCreateCollection(
    OperationContext* opCtx,
    const NamespaceString& collectionName,
    const CollectionOptions& options,
    const BSONObj& idIndex,
    const OplogSlot& createOpTime,
    const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
    bool fromMigrate,
    bool isTimeseries,
    bool recordIdsReplicated) {
    _checkWriteAllowed(opCtx, collectionName);
}

void UserWriteBlockModeOpObserver::onCollMod(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const UUID& uuid,
                                             const BSONObj& collModCmd,
                                             const CollectionOptions& oldCollOptions,
                                             boost::optional<IndexCollModInfo> indexInfo,
                                             bool isTimeseries) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::onDropDatabase(OperationContext* opCtx,
                                                  const DatabaseName& dbName,
                                                  bool markFromMigrate) {
    _checkWriteAllowed(opCtx, NamespaceString(dbName));
}

repl::OpTime UserWriteBlockModeOpObserver::onDropCollection(OperationContext* opCtx,
                                                            const NamespaceString& collectionName,
                                                            const UUID& uuid,
                                                            std::uint64_t numRecords,
                                                            bool markFromMigrate,
                                                            bool isTimeseries) {
    _checkWriteAllowed(opCtx, collectionName);
    return repl::OpTime();
}

void UserWriteBlockModeOpObserver::onDropIndex(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& uuid,
                                               const std::string& indexName,
                                               const BSONObj& indexInfo,
                                               bool isTimeseries) {
    _checkWriteAllowed(opCtx, nss);
}

repl::OpTime UserWriteBlockModeOpObserver::preRenameCollection(
    OperationContext* opCtx,
    const NamespaceString& fromCollection,
    const NamespaceString& toCollection,
    const UUID& uuid,
    const boost::optional<UUID>& dropTargetUUID,
    std::uint64_t numRecords,
    bool stayTemp,
    bool markFromMigrate,
    bool isTimeseries) {
    _checkWriteAllowed(opCtx, fromCollection);
    _checkWriteAllowed(opCtx, toCollection);
    return repl::OpTime();
}

void UserWriteBlockModeOpObserver::onRenameCollection(OperationContext* opCtx,
                                                      const NamespaceString& fromCollection,
                                                      const NamespaceString& toCollection,
                                                      const UUID& uuid,
                                                      const boost::optional<UUID>& dropTargetUUID,
                                                      std::uint64_t numRecords,
                                                      bool stayTemp,
                                                      bool markFromMigrate,
                                                      bool isTimeseries) {
    _checkWriteAllowed(opCtx, fromCollection);
    _checkWriteAllowed(opCtx, toCollection);
}

void UserWriteBlockModeOpObserver::onImportCollection(OperationContext* opCtx,
                                                      const UUID& importUUID,
                                                      const NamespaceString& nss,
                                                      long long numRecords,
                                                      long long dataSize,
                                                      const BSONObj& catalogEntry,
                                                      const BSONObj& storageMetadata,
                                                      bool isDryRun,
                                                      bool isTimeseries) {
    _checkWriteAllowed(opCtx, nss);
}

void UserWriteBlockModeOpObserver::_checkWriteAllowed(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    // Evaluate write blocking only on replica set primaries.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().isReplSet() && replCoord->canAcceptWritesFor(opCtx, nss)) {
        GlobalUserWriteBlockState::get(opCtx)->checkUserWritesAllowed(opCtx, nss);
    }
}

}  // namespace mongo
