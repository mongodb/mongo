/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/topology/remove_shard_commit_coordinator_document_gen.h"

namespace mongo {
class RemoveShardCommitCoordinator final
    : public RecoverableShardingDDLCoordinator<RemoveShardCommitCoordinatorDocument,
                                               RemoveShardCommitCoordinatorPhaseEnum> {
public:
    using StateDoc = RemoveShardCommitCoordinatorDocument;
    using Phase = RemoveShardCommitCoordinatorPhaseEnum;

    RemoveShardCommitCoordinator(ShardingDDLCoordinatorService* service,
                                 const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "RemoveShardCommitCoordinator", initialState) {
    }

    ~RemoveShardCommitCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    RemoveShardProgress getResult(OperationContext* opCtx);

private:
    StringData serializePhase(const Phase& phase) const override {
        return RemoveShardCommitCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kCommit;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    // Checks that the shard still exists in the cluster and that the draining flag is still set.
    void _checkShardExistsAndIsDraining(OperationContext* opCtx);

    // Get the replica set name from the shard registry and write it to the state document.
    void _setReplicaSetNameOnDocument(OperationContext* opCtx);

    // Joins migrations on the config server if we are transitioning from dedicated and checks
    // if there are range deletions to wait for.
    void _joinMigrationsAndCheckRangeDeletions(OperationContext* opCtx);

    // Stops ongoing ddl operations (excluding topology changes) and waits for any ongoing
    // coordinators to complete.
    void _stopDDLOperations(OperationContext* opCtx);

    // Checks whether there is any data left on the shard after stopping DDL operations.
    void _checkShardIsEmpty(OperationContext* opCtx);

    // Ensures none of the local collections have data in them and drops them. This should only be
    // called during config transitions.
    void _dropLocalCollections(OperationContext* opCtx);

    // Removes the shard and updates the topology time on a control shard.
    void _commitRemoveShard(OperationContext* opCtx,
                            std::shared_ptr<executor::ScopedTaskExecutor> executor);

    // Allows ddl operations to resume in the cluster.
    void _resumeDDLOperations(OperationContext* opCtx);

    // TODO (SERVER-99433) Remove once replica set endpoint is fully discontinued.
    // Updates the "hasTwoOrMoreShard" cluster cardinality parameter if this shard removal leaves
    // only one shard in the cluster and the coordinator was started with the parameter
    // `shouldUpdateClusterCardinality` set to true.
    void _updateClusterCardinalityParameterIfNeeded(OperationContext* opCtx);

    // Sets the result of the remove shard and logs the completion.
    void _finalizeShardRemoval(OperationContext* opCtx);

    // Set on successful completion of the coordinator.
    boost::optional<RemoveShardProgress> _result;
};

}  // namespace mongo
