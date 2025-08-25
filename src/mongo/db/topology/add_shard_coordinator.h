/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/topology/add_shard_coordinator_document_gen.h"
#include "mongo/db/topology/topology_change_helpers.h"
#include "mongo/s/sharding_task_executor.h"

namespace mongo {
class AddShardCoordinator final
    : public RecoverableShardingDDLCoordinator<AddShardCoordinatorDocument,
                                               AddShardCoordinatorPhaseEnum> {
public:
    using StateDoc = AddShardCoordinatorDocument;
    using Phase = AddShardCoordinatorPhaseEnum;

    AddShardCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState);

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
    StringData serializePhase(const Phase& phase) const override {
        return AddShardCoordinatorPhase_serializer(phase);
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _dropBlockFCVChangesCollection(OperationContext* opCtx,
                                        std::shared_ptr<executor::TaskExecutor> executor);

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    void _verifyInput() const;

    bool _isPristineReplicaset(OperationContext* opCtx, RemoteCommandTargeter& targeter) const;

    RemoteCommandTargeter& _getTargeter(OperationContext* opCtx);

    bool _validateShardIdentityDocumentOnReplicaSet(OperationContext* opCtx);

    void _runWithRetries(std::function<void()>&& function,
                         std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token);

    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> _osiGenerator();

    void _standardizeClusterParameters(OperationContext* opCt);

    bool _isFirstShard(OperationContext* opCtx);

    void _setFCVOnReplicaSet(OperationContext* opCtx,
                             mongo::ServerGlobalParams::FCVSnapshot::FCV fcv);

    void _blockUserWrites(OperationContext* opCtx);

    void _restoreUserWrites(OperationContext* opCtx);

    void _blockFCVChangesOnReplicaSet(OperationContext* opCtx,
                                      std::shared_ptr<executor::TaskExecutor> executor);

    void _unblockFCVChangesOnNewShard(OperationContext* opCtx,
                                      std::shared_ptr<executor::TaskExecutor> executor);

    topology_change_helpers::UserWriteBlockingLevel _getUserWritesBlockFromReplicaSet(
        OperationContext* opCtx);

    void _dropSessionsCollection(OperationContext* opCtx);

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
};

}  // namespace mongo
