/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_task_executor.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/transaction/transaction_router.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {

namespace {
const std::string kOperationTimeField = "operationTime";
}

ShardingTaskExecutor::ShardingTaskExecutor(std::unique_ptr<ThreadPoolTaskExecutor> executor)
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
                                                                       const CallbackFn& work) {
    return _executor->onEvent(event, work);
}

void ShardingTaskExecutor::waitForEvent(const EventHandle& event) {
    _executor->waitForEvent(event);
}

StatusWith<stdx::cv_status> ShardingTaskExecutor::waitForEvent(OperationContext* opCtx,
                                                               const EventHandle& event,
                                                               Date_t deadline) {
    return _executor->waitForEvent(opCtx, event, deadline);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWork(
    const CallbackFn& work) {
    return _executor->scheduleWork(work);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    return _executor->scheduleWorkAt(when, work);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request,
    const RemoteCommandCallbackFn& cb,
    const transport::BatonHandle& baton) {

    // schedule the user's callback if there is not opCtx
    if (!request.opCtx) {
        return _executor->scheduleRemoteCommand(request, cb, baton);
    }

    boost::optional<RemoteCommandRequest> newRequest;

    if (request.opCtx->getLogicalSessionId() && !request.cmdObj.hasField("lsid")) {
        newRequest.emplace(request);

        BSONObjBuilder bob(std::move(newRequest->cmdObj));
        {
            // TODO SERVER-33702.
            BSONObjBuilder subbob(bob.subobjStart("lsid"));
            request.opCtx->getLogicalSessionId()->serialize(&subbob);
        }

        newRequest->cmdObj = bob.obj();
    }

    auto txnRouter = TransactionRouter::get(request.opCtx);
    if (txnRouter) {
        auto shard =
            Grid::get(request.opCtx)->shardRegistry()->getShardForHostNoReload(request.target);

        if (!shard) {
            return {ErrorCodes::ShardNotFound,
                    str::stream() << "Could not find shard containing host: "
                                  << request.target.toString()};
        }

        if (!newRequest) {
            newRequest.emplace(request);
        }

        auto& participant = txnRouter->getOrCreateParticipant(shard->getId());
        newRequest->cmdObj = participant.attachTxnFieldsIfNeeded(newRequest->cmdObj);
    }

    std::shared_ptr<OperationTimeTracker> timeTracker = OperationTimeTracker::get(request.opCtx);

    auto clusterGLE = ClusterLastErrorInfo::get(request.opCtx->getClient());

    auto shardingCb = [ timeTracker, clusterGLE, cb, grid = Grid::get(request.opCtx), txnRouter ](
        const TaskExecutor::RemoteCommandCallbackArgs& args) {
        ON_BLOCK_EXIT([&cb, &args]() { cb(args); });

        // Update replica set monitor info.
        auto shard = grid->shardRegistry()->getShardForHostNoReload(args.request.target);
        if (!shard) {
            LOG(1) << "Could not find shard containing host: " << args.request.target.toString();
        } else if (txnRouter) {
            // TODO: SERVER-35707 only mark as sent for non-network error?
            auto& participant = txnRouter->getOrCreateParticipant(shard->getId());
            participant.markAsCommandSent();
        }

        if (!args.response.isOK()) {
            if (isMongos() && args.response.status == ErrorCodes::IncompatibleWithUpgradedServer) {
                severe()
                    << "This mongos server must be upgraded. It is attempting to communicate with "
                       "an upgraded cluster with which it is incompatible. Error: '"
                    << args.response.status.toString()
                    << "' Crashing in order to bring attention to the incompatibility, rather "
                       "than erroring endlessly.";
                fassertNoTrace(50710, false);
            }

            if (shard) {
                shard->updateReplSetMonitor(args.request.target, args.response.status);
            }

            LOG(1) << "Error processing the remote request, not updating operationTime or gLE";
            return;
        }

        if (shard) {
            shard->updateReplSetMonitor(args.request.target,
                                        getStatusFromCommandResult(args.response.data));
        }

        // Update the logical clock.
        invariant(timeTracker);
        auto operationTime = args.response.data[kOperationTimeField];
        if (!operationTime.eoo()) {
            invariant(operationTime.type() == BSONType::bsonTimestamp);
            timeTracker->updateOperationTime(LogicalTime(operationTime.timestamp()));
        }

        // Update getLastError info for the client if we're tracking it.
        if (clusterGLE) {
            auto swShardingMetadata = rpc::ShardingMetadata::readFromMetadata(args.response.data);
            if (swShardingMetadata.isOK()) {
                auto shardingMetadata = std::move(swShardingMetadata.getValue());

                auto shardConn = ConnectionString::parse(args.request.target.toString());
                if (!shardConn.isOK()) {
                    severe() << "got bad host string in saveGLEStats: " << args.request.target;
                }

                clusterGLE->addHostOpTime(shardConn.getValue(),
                                          HostOpTime(shardingMetadata.getLastOpTime(),
                                                     shardingMetadata.getLastElectionId()));
            } else if (swShardingMetadata.getStatus() != ErrorCodes::NoSuchKey) {
                warning() << "Got invalid sharding metadata "
                          << redact(swShardingMetadata.getStatus()) << " metadata object was '"
                          << redact(args.response.data) << "'";
            }
        }
    };

    return _executor->scheduleRemoteCommand(newRequest ? *newRequest : request, shardingCb, baton);
}

void ShardingTaskExecutor::cancel(const CallbackHandle& cbHandle) {
    _executor->cancel(cbHandle);
}

void ShardingTaskExecutor::wait(const CallbackHandle& cbHandle) {
    _executor->wait(cbHandle);
}

void ShardingTaskExecutor::appendConnectionStats(ConnectionPoolStats* stats) const {
    _executor->appendConnectionStats(stats);
}

}  // namespace executor
}  // namespace mongo
