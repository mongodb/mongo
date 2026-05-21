/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/topology/user_write_block/replica_set_write_block_op_observer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_state.h"
#include "mongo/db/topology/user_write_block/replica_set_writes_critical_section_document_gen.h"
#include "mongo/db/topology/user_write_block/writes_recoverable_critical_section_service.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

namespace mongo {

bool ReplicaSetWriteBlockOpObserver::_isReplSetAndCanAcceptWritesForNamespace(
    OperationContext* opCtx, const NamespaceString& nss) const {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    return replCoord->getSettings().isReplSet() && replCoord->canAcceptWritesFor(opCtx, nss);
}

void ReplicaSetWriteBlockOpObserver::_checkReplicaSetWriteAllowed(OperationContext* opCtx,
                                                                  const NamespaceString& nss,
                                                                  bool fromMigrate) {
    if (!_isReplSetAndCanAcceptWritesForNamespace(opCtx, nss)) {
        return;
    }
    auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
    if (!fromMigrate) {
        replicaSetWriteBlockState->checkReplicaSetWritesAllowed(opCtx, nss);
    }
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
                                               std::vector<bool> fromMigrate,
                                               bool defaultFromMigrate,
                                               OpStateAccumulator* opAccumulator) {
    const auto nss = coll->ns();

    _checkReplicaSetWriteAllowed(opCtx, nss, defaultFromMigrate);

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kReplicaSetWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        for (auto it = first; it != last; ++it) {
            const auto& insertedDoc = it->doc;

            const auto collCSDoc = ReplicaSetWriteBlockingCriticalSectionDocument::parse(
                insertedDoc, IDLParserContext("ReplicaSetWriteBlockOpObserver"));

            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [blockWrites = collCSDoc.getEnabled(),
                 blockDeletions = !collCSDoc.getAllowDeletions(),
                 blockUserWritesReason = collCSDoc.getReplicaSetWritesBlockReason()](
                    OperationContext* opCtx, boost::optional<Timestamp>) {
                    auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
                    if (blockWrites) {
                        replicaSetWriteBlockState->enableReplicaSetWriteBlocking(
                            blockUserWritesReason);
                    }
                    if (blockDeletions) {
                        replicaSetWriteBlockState->enableReplicaSetDeletionsBlocking();
                    }
                });
        }
    }
}

void ReplicaSetWriteBlockOpObserver::onUpdate(OperationContext* opCtx,
                                              const OplogUpdateEntryArgs& args,
                                              OpStateAccumulator* opAccumulator) {
    const auto nss = args.coll->ns();

    const bool fromMigrate = args.updateArgs->source == OperationSource::kFromMigrate;
    _checkReplicaSetWriteAllowed(opCtx, nss, fromMigrate);

    // TODO (SERVER-91506): Determine if we should change this to check isDataConsistent.
    if (nss == NamespaceString::kReplicaSetWritesCriticalSectionsNamespace &&
        !repl::ReplicationCoordinator::get(opCtx)->isInInitialSyncOrRollback()) {
        const auto collCSDoc = ReplicaSetWriteBlockingCriticalSectionDocument::parse(
            args.updateArgs->updatedDoc, IDLParserContext("ReplicaSetWriteBlockOpObserver"));

        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [blockWrites = collCSDoc.getEnabled(),
             blockDeletions = !collCSDoc.getAllowDeletions(),
             blockUserWritesReason = collCSDoc.getReplicaSetWritesBlockReason()](
                OperationContext* opCtx, boost::optional<Timestamp>) {
                auto* replicaSetWriteBlockState = ReplicaSetWriteBlockState::get(opCtx);
                if (blockWrites) {
                    replicaSetWriteBlockState->enableReplicaSetWriteBlocking(blockUserWritesReason);
                } else {
                    replicaSetWriteBlockState->disableReplicaSetWriteBlocking();
                }
                if (blockDeletions) {
                    replicaSetWriteBlockState->enableReplicaSetDeletionsBlocking();
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

}  // namespace mongo
