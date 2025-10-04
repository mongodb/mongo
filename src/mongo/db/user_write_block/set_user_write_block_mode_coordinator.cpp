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


#include "mongo/db/user_write_block/set_user_write_block_mode_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/user_write_block/set_user_write_block_mode_gen.h"
#include "mongo/db/user_write_block/user_writes_recoverable_critical_section_service.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future_impl.h"

#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

ShardsvrSetUserWriteBlockMode makeShardsvrSetUserWriteBlockModeCommand(
    bool block, ShardsvrSetUserWriteBlockModePhaseEnum phase) {
    ShardsvrSetUserWriteBlockMode shardsvrSetUserWriteBlockModeCmd;
    shardsvrSetUserWriteBlockModeCmd.setDbName(DatabaseName::kAdmin);
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

    auto shardsvrSetUserWriteBlockModeCmd = makeShardsvrSetUserWriteBlockModeCommand(block, phase);

    generic_argument_util::setOperationSessionInfo(shardsvrSetUserWriteBlockModeCmd, osi);
    generic_argument_util::setMajorityWriteConcern(shardsvrSetUserWriteBlockModeCmd);

    sharding_util::sendCommandToShards(opCtx,
                                       shardsvrSetUserWriteBlockModeCmd.getDbName(),
                                       shardsvrSetUserWriteBlockModeCmd.toBSON(),
                                       allShards,
                                       executor);
}

}  // namespace

bool SetUserWriteBlockModeCoordinator::hasSameOptions(const BSONObj& otherDocBSON) const {
    const auto otherDoc =
        StateDoc::parse(otherDocBSON, IDLParserContext("SetUserWriteBlockModeCoordinatorDocument"));
    return _evalStateDocumentThreadSafe(
        [&](const StateDoc& doc) { return doc.getBlock() == otherDoc.getBlock(); });
}

boost::optional<BSONObj> SetUserWriteBlockModeCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    const auto phase =
        _evalStateDocumentThreadSafe([](const StateDoc& doc) { return doc.getPhase(); });

    BSONObjBuilder bob;
    bob.append("type", "op");
    bob.append("desc", "SetUserWriteBlockModeCoordinator");
    bob.append("op", "command");
    bob.append("currentPhase", phase);
    bob.append("active", true);
    return bob.obj();
}

const ConfigsvrCoordinatorMetadata& SetUserWriteBlockModeCoordinator::metadata() const {
    return _doc.getConfigsvrCoordinatorMetadata();
}

ExecutorFuture<void> SetUserWriteBlockModeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kPrepare,
            [this, anchor = shared_from_this()](auto* opCtx) {
                auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

                // Get an incremented {lsid, txNnumber} pair that will be attached to the command
                // sent to the shards to guarantee message replay protection.
                _updateSession(opCtx);
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
                uassertStatusOK(waitForWriteConcern(
                    opCtx, latestOpTime, defaultMajorityWriteConcern(), &ignoreResult));
            }))
        .then(
            _buildPhaseHandler(Phase::kComplete, [this, anchor = shared_from_this()](auto* opCtx) {
                auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

                // Get an incremented {lsid, txNnumber} pair that will be attached to the command
                // sent to the shards to guarantee message replay protection.
                _updateSession(opCtx);
                const auto session = _getCurrentSession();

                // Ensure the topology is stable so we don't miss propagating the write blocking
                // state to any concurrently added shard. Keep it stable until we have persisted the
                // user write blocking state on the configsvr so that new shards that get added will
                // e see the new state.
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
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                } else {
                    UserWritesRecoverableCriticalSectionService::get(opCtx)
                        ->releaseRecoverableCriticalSection(
                            opCtx,
                            UserWritesRecoverableCriticalSectionService::
                                kGlobalUserWritesNamespace);
                }

                // Wait for majority write concern.
                WriteConcernResult ignoreResult;
                auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                uassertStatusOK(waitForWriteConcern(
                    opCtx, latestOpTime, defaultMajorityWriteConcern(), &ignoreResult));
            }));
}

}  // namespace mongo
