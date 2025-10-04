/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/s/sharding_task_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace executor {

namespace {
const std::string kOperationTimeField = "operationTime";
}

ShardingTaskExecutor::ShardingTaskExecutor(Passkey,
                                           std::shared_ptr<ThreadPoolTaskExecutor> executor)
    : _executor(std::move(executor)) {}

void ShardingTaskExecutor::startup() {
    _executor->startup();
}

void ShardingTaskExecutor::shutdown() {
    _executor->shutdown();
}

void ShardingTaskExecutor::join() {
    _executor->join();
}

SharedSemiFuture<void> ShardingTaskExecutor::joinAsync() {
    return _executor->joinAsync();
}

bool ShardingTaskExecutor::isShuttingDown() const {
    return _executor->isShuttingDown();
}

void ShardingTaskExecutor::appendDiagnosticBSON(mongo::BSONObjBuilder* builder) const {
    _executor->appendDiagnosticBSON(builder);
}

Date_t ShardingTaskExecutor::now() {
    return _executor->now();
}

StatusWith<TaskExecutor::EventHandle> ShardingTaskExecutor::makeEvent() {
    return _executor->makeEvent();
}

void ShardingTaskExecutor::signalEvent(const EventHandle& event) {
    return _executor->signalEvent(event);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::onEvent(const EventHandle& event,
                                                                       CallbackFn&& work) {
    return _executor->onEvent(event, std::move(work));
}

void ShardingTaskExecutor::waitForEvent(const EventHandle& event) {
    _executor->waitForEvent(event);
}

StatusWith<stdx::cv_status> ShardingTaskExecutor::waitForEvent(OperationContext* opCtx,
                                                               const EventHandle& event,
                                                               Date_t deadline) {
    return _executor->waitForEvent(opCtx, event, deadline);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWork(CallbackFn&& work) {
    return _executor->scheduleWork(std::move(work));
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWorkAt(Date_t when,
                                                                              CallbackFn&& work) {
    return _executor->scheduleWorkAt(when, std::move(work));
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {

    // schedule the user's callback if there is not opCtx
    if (!request.opCtx) {
        return _executor->scheduleRemoteCommand(request, cb, baton);
    }

    boost::optional<RemoteCommandRequest> requestWithFixedLsid = [&] {
        boost::optional<RemoteCommandRequest> newRequest;

        if (!request.opCtx->getLogicalSessionId()) {
            return newRequest;
        }

        if (auto lsidElem =
                request.cmdObj.getField(OperationSessionInfoFromClient::kSessionIdFieldName)) {
            // BSONObj must outlive BSONElement. See BSONElement, BSONObj::getField().
            auto lsidObj = lsidElem.Obj();
            if (auto lsidUIDElem = lsidObj.getField(LogicalSessionFromClient::kUidFieldName)) {
                tassert(10090100,
                        "User digest in the logical session ID from opCtx does not match with the "
                        "command request",
                        SHA256Block::fromBinData(lsidUIDElem._binDataVector()) ==
                            request.opCtx->getLogicalSessionId()->getUid());
                return newRequest;
            }

            newRequest.emplace(request);
            newRequest->cmdObj =
                newRequest->cmdObj.removeField(OperationSessionInfoFromClient::kSessionIdFieldName);
        }

        if (!newRequest) {
            newRequest.emplace(request);
        }

        BSONObjBuilder bob(std::move(newRequest->cmdObj));
        {
            BSONObjBuilder subbob(
                bob.subobjStart(OperationSessionInfoFromClient::kSessionIdFieldName));
            request.opCtx->getLogicalSessionId()->serialize(&subbob);
            subbob.done();
        }

        newRequest->cmdObj = bob.obj();

        return newRequest;
    }();

    std::shared_ptr<OperationTimeTracker> timeTracker = OperationTimeTracker::get(request.opCtx);

    auto shardingCb = [timeTracker, cb, grid = Grid::get(request.opCtx), hosts = request.target](
                          const TaskExecutor::RemoteCommandCallbackArgs& args) {
        ON_BLOCK_EXIT([&cb, &args]() { cb(args); });

        if (!args.response.isOK()) {
            HostAndPort target;

            target = args.response.target;

            auto shard = grid->shardRegistry()->getShardForHostNoReload(target);

            if (!shard) {
                LOGV2_DEBUG(22870, 1, "Could not find shard containing host", "host"_attr = target);
            }

            if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) &&
                args.response.status == ErrorCodes::IncompatibleWithUpgradedServer) {
                LOGV2_FATAL_NOTRACE(
                    50710,
                    "This mongos is attempting to communicate with an upgraded cluster with which "
                    "it is incompatible, so this mongos should be upgraded. Crashing in order to "
                    "bring attention to the incompatibility rather than erroring endlessly.",
                    "error"_attr = args.response.status);
            }

            if (shard) {
                shard->updateReplSetMonitor(target, args.response.status);
            }

            LOGV2_DEBUG(22871,
                        1,
                        "Error processing the remote request, not updating operationTime or gLE",
                        "error"_attr = args.response.status);

            return;
        }

        auto target = args.response.target;

        auto shard = grid->shardRegistry()->getShardForHostNoReload(target);

        if (shard) {
            shard->updateReplSetMonitor(target, getStatusFromCommandResult(args.response.data));
        }

        // Update the logical clock.
        invariant(timeTracker);
        auto operationTime = args.response.data[kOperationTimeField];
        if (!operationTime.eoo()) {
            invariant(operationTime.type() == BSONType::timestamp);
            timeTracker->updateOperationTime(LogicalTime(operationTime.timestamp()));
        }
    };

    return _executor->scheduleRemoteCommand(
        requestWithFixedLsid ? *requestWithFixedLsid : request, shardingCb, baton);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleExhaustRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const BatonHandle& baton) {
    MONGO_UNREACHABLE;
}

bool ShardingTaskExecutor::hasTasks() {
    MONGO_UNREACHABLE;
}

void ShardingTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    _executor->cancel(cbHandle);
}

void ShardingTaskExecutor::wait(const CallbackHandle& cbHandle, Interruptible* interruptible) {
    _executor->wait(cbHandle, interruptible);
}

void ShardingTaskExecutor::appendConnectionStats(ConnectionPoolStats* stats) const {
    _executor->appendConnectionStats(stats);
}

void ShardingTaskExecutor::dropConnections(const HostAndPort& target, const Status& status) {
    _executor->dropConnections(target, status);
}

void ShardingTaskExecutor::appendNetworkInterfaceStats(BSONObjBuilder& bob) const {
    _executor->appendNetworkInterfaceStats(bob);
}

}  // namespace executor
}  // namespace mongo
