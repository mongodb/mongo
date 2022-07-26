/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/s/config/set_user_write_block_mode_coordinator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/s/user_writes_recoverable_critical_section_service.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

ShardsvrSetUserWriteBlockMode makeShardsvrSetUserWriteBlockModeCommand(
    bool block, ShardsvrSetUserWriteBlockModePhaseEnum phase) {
    ShardsvrSetUserWriteBlockMode shardsvrSetUserWriteBlockModeCmd;
    shardsvrSetUserWriteBlockModeCmd.setDbName(NamespaceString::kAdminDb);
    SetUserWriteBlockModeRequest setUserWriteBlockModeRequest(block /* global */);
    shardsvrSetUserWriteBlockModeCmd.setSetUserWriteBlockModeRequest(
        std::move(setUserWriteBlockModeRequest));
    shardsvrSetUserWriteBlockModeCmd.setPhase(phase);

    return shardsvrSetUserWriteBlockModeCmd;
}

void sendSetUserWriteBlockModeCmdToAllShards(OperationContext* opCtx,
                                             std::shared_ptr<executor::TaskExecutor> executor,
                                             bool block,
                                             ShardsvrSetUserWriteBlockModePhaseEnum phase,
                                             const OperationSessionInfo& osi) {
    const auto allShards = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    const auto shardsvrSetUserWriteBlockModeCmd =
        makeShardsvrSetUserWriteBlockModeCommand(block, phase);

    sharding_util::sendCommandToShards(opCtx,
                                       shardsvrSetUserWriteBlockModeCmd.getDbName(),
                                       CommandHelpers::appendMajorityWriteConcern(
                                           shardsvrSetUserWriteBlockModeCmd.toBSON(osi.toBSON())),
                                       allShards,
                                       executor);
}

}  // namespace

bool SetUserWriteBlockModeCoordinator::hasSameOptions(const BSONObj& otherDocBSON) const {
    const auto otherDoc =
        StateDoc::parse(IDLParserContext("SetUserWriteBlockModeCoordinatorDocument"), otherDocBSON);

    return _doc.getBlock() == otherDoc.getBlock();
}

boost::optional<BSONObj> SetUserWriteBlockModeCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "SetUserWriteBlockModeCoordinator");
    bob.append("op", "command");
    bob.append("currentPhase", _doc.getPhase());
    bob.append("active", true);
    return bob.obj();
}

void SetUserWriteBlockModeCoordinator::_enterPhase(Phase newPhase) {
    StateDoc newDoc(_doc);
    newDoc.setPhase(newPhase);

    LOGV2_DEBUG(
        6347305,
        2,
        "SetUserWriteBlockModeCoordinator phase transition",
        "newPhase"_attr = SetUserWriteBlockModeCoordinatorPhase_serializer(newDoc.getPhase()),
        "oldPhase"_attr = SetUserWriteBlockModeCoordinatorPhase_serializer(_doc.getPhase()));

    auto opCtx = cc().makeOperationContext();

    if (_doc.getPhase() == Phase::kUnset) {
        PersistentTaskStore<StateDoc> store(NamespaceString::kConfigsvrCoordinatorsNamespace);
        try {
            store.add(opCtx.get(), newDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of step-up and step-down events can cause a node to try and insert the
            // document when it has already been persisted locally, but we must still wait for
            // majority commit.
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
            const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
            WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(lastLocalOpTime, opCtx.get()->getCancellationToken())
                .get(opCtx.get());
        }
    } else {
        _updateStateDocument(opCtx.get(), newDoc);
    }

    _doc = std::move(newDoc);
}

const ConfigsvrCoordinatorMetadata& SetUserWriteBlockModeCoordinator::metadata() const {
    return _doc.getConfigsvrCoordinatorMetadata();
}

ExecutorFuture<void> SetUserWriteBlockModeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_executePhase(
            Phase::kPrepare,
            [this, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

                // Get an incremented {lsid, txNnumber} pair that will be attached to the command
                // sent to the shards to guarantee message replay protection.
                _doc = _updateSession(opCtx, _doc);
                const auto session = _getCurrentSession();

                // Ensure the topology is stable so we don't miss propagating the write blocking
                // state to any concurrently added shard. Keep it stable until we have persisted the
                // user write blocking state on the configsvr so that new shards that get added will
                // see the new state.
                Lock::SharedLock stableTopologyRegion =
                    ShardingCatalogManager::get(opCtx)->enterStableTopologyRegion(opCtx);

                // Propagate the state to the shards.
                sendSetUserWriteBlockModeCmdToAllShards(
                    opCtx,
                    executor,
                    _doc.getBlock(),
                    ShardsvrSetUserWriteBlockModePhaseEnum::kPrepare,
                    session);

                // Durably store the state on the configsvr.
                if (_doc.getBlock()) {
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->acquireRecoverableCriticalSectionBlockNewShardedDDL(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                } else {
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->demoteRecoverableCriticalSectionToNoLongerBlockUserWrites(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                }

                // Wait for majority write concern.
                WriteConcernResult ignoreResult;
                auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                uassertStatusOK(waitForWriteConcern(opCtx,
                                                    latestOpTime,
                                                    WriteConcerns::kMajorityWriteConcernNoTimeout,
                                                    &ignoreResult));
            }))
        .then(_executePhase(Phase::kComplete, [this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

            // Get an incremented {lsid, txNnumber} pair that will be attached to the command sent
            // to the shards to guarantee message replay protection.
            _doc = _updateSession(opCtx, _doc);
            const auto session = _getCurrentSession();

            // Ensure the topology is stable so we don't miss propagating the write blocking state
            // to any concurrently added shard. Keep it stable until we have persisted the user
            // write blocking state on the configsvr so that new shards that get added will e see
            // the new state.
            Lock::SharedLock stableTopologyRegion =
                ShardingCatalogManager::get(opCtx)->enterStableTopologyRegion(opCtx);

            // Propagate the state to the shards.
            sendSetUserWriteBlockModeCmdToAllShards(
                opCtx,
                executor,
                _doc.getBlock(),
                ShardsvrSetUserWriteBlockModePhaseEnum::kComplete,
                session);

            // Durably store the state on the configsvr.
            if (_doc.getBlock()) {
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->promoteRecoverableCriticalSectionToBlockUserWrites(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
            } else {
                UserWritesRecoverableCriticalSectionService::get(opCtx)
                    ->releaseRecoverableCriticalSection(
                        opCtx,
                        UserWritesRecoverableCriticalSectionService::kGlobalUserWritesNamespace);
            }

            // Wait for majority write concern.
            WriteConcernResult ignoreResult;
            auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            uassertStatusOK(waitForWriteConcern(
                opCtx, latestOpTime, WriteConcerns::kMajorityWriteConcernNoTimeout, &ignoreResult));
        }));
}

}  // namespace mongo
