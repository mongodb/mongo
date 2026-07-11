// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/sharding_environment/sharding_task_executor.h"
#include "mongo/db/topology/add_shard_coordinator_document_gen.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/util/modules.h"

namespace mongo {
class [[MONGO_MOD_PARENT_PRIVATE]] AddShardCoordinator final
    : public RecoverableShardingDDLCoordinator<AddShardCoordinatorDocument> {
public:
    AddShardCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState);

    ~AddShardCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    const std::string& getResult(OperationContext* opCtx) const;

    bool canAlwaysStartWhenUserWritesAreDisabled() const override;

    static std::shared_ptr<AddShardCoordinator> create(OperationContext* opCtx,
                                                       const FixedFCVRegion& fcvRegion,
                                                       const mongo::ConnectionString& target,
                                                       boost::optional<std::string> name,
                                                       bool isConfigShard);

protected:
    bool _mustAlwaysMakeProgress() override;

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _dropBlockFCVChangesCollection(OperationContext* opCtx,
                                        std::shared_ptr<executor::TaskExecutor> executor);

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    bool isInCriticalSection(Phase phase) const override;

    void _verifyInput() const;

    bool _isPristineReplicaset(OperationContext* opCtx, RemoteCommandTargeter& targeter) const;

    RemoteCommandTargeter& _getTargeter(OperationContext* opCtx);

    bool _hasShardingDataOnReplicaSet(OperationContext* opCtx);

    Status _runWithRetries(std::function<void()>&& function,
                           std::shared_ptr<executor::ScopedTaskExecutor> executor,
                           const CancellationToken& token);

    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> _osiGenerator();

    void _standardizeClusterParameters(OperationContext* opCt);

    bool _isFirstShard(OperationContext* opCtx);

    mongo::ServerGlobalParams::FCVSnapshot::FCV _getFCVOnReplicaSet(OperationContext* opCtx);

    void _setFCVOnReplicaSet(OperationContext* opCtx,
                             mongo::ServerGlobalParams::FCVSnapshot::FCV fcv,
                             bool isWriteBlockedNewReplicaSet);

    void _blockUserWrites(OperationContext* opCtx);

    void _restoreUserWrites(OperationContext* opCtx);

    void _blockFCVChangesOnReplicaSet(OperationContext* opCtx,
                                      std::shared_ptr<executor::TaskExecutor> executor);

    void _unblockFCVChangesOnNewShard(OperationContext* opCtx,
                                      std::shared_ptr<executor::TaskExecutor> executor,
                                      bool isAuthoritative);

    topology_change_helpers::UserWriteBlockingLevel _getUserWritesBlockFromReplicaSet(
        OperationContext* opCtx);

    /**
     * Checks if we are adding a new replica set to the cluster; in this case we ensure it's
     * pristine (empty), block user writes to ensure it remains so, and return true.
     * Returns false otherwise (in this case, user writes won't have been blocked).
     */
    bool _tryBlockNewReplicaSet(OperationContext* opCtx, RemoteCommandTargeter& targeter);

    void _dropSessionsCollection(OperationContext* opCtx);

    void _drainOngoingDDLOperations(OperationContext* opCtx);

    void _installShardIdentity(OperationContext* opCtx,
                               std::shared_ptr<executor::TaskExecutor> executor);

    // Set on successful completion of the coordinator.
    boost::optional<std::string> _result;

    std::unique_ptr<Shard> _shardConnection;

    // We create a new executor without the gossiping protocol because we don't want to
    // expose any cluster-related vector clock internals (e.g., cluster time, config time)
    // before the replica set has officially joined the cluster.
    // If the replica set were to receive vector clock values and then fail to join,
    // it could potentially corrupt another cluster's vector clock during a subsequent attempt to
    // join.
    std::shared_ptr<executor::ShardingTaskExecutor> _executorWithoutGossip;

    const BSONObj _critSecReason;

    bool _giveUpTrying{false};
};

}  // namespace mongo
