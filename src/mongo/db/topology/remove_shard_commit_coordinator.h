// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/topology/remove_shard_commit_coordinator_document_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
class [[MONGO_MOD_PARENT_PRIVATE]] RemoveShardCommitCoordinator final
    : public RecoverableShardingDDLCoordinator<RemoveShardCommitCoordinatorDocument> {
public:
    RemoveShardCommitCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "RemoveShardCommitCoordinator", initialState) {
    }

    ~RemoveShardCommitCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    RemoveShardProgress getResult(OperationContext* opCtx);

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
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
