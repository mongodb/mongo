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

#include "mongo/db/s/sharding_task_executor.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/sharding_metadata.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/util/log.h"

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

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWork(
    const CallbackFn& work) {
    return _executor->scheduleWork(work);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleWorkAt(
    Date_t when, const CallbackFn& work) {
    return _executor->scheduleWorkAt(when, work);
}

StatusWith<TaskExecutor::CallbackHandle> ShardingTaskExecutor::scheduleRemoteCommand(
    const RemoteCommandRequest& request, const RemoteCommandCallbackFn& cb) {

    // schedule the user's callback if there is not opCtx
    if (!request.opCtx) {
        return _executor->scheduleRemoteCommand(request, cb);
    }

    std::shared_ptr<OperationTimeTracker> timeTracker = OperationTimeTracker::get(request.opCtx);
    if (!timeTracker) {  // install the time tracker on the opCtx
        timeTracker = std::make_shared<OperationTimeTracker>();
        OperationTimeTracker::set(request.opCtx, timeTracker);
    }

    auto clusterGLE = ClusterLastErrorInfo::get(request.opCtx->getClient());

    auto shardingCb = [timeTracker, clusterGLE, request, cb](
        const TaskExecutor::RemoteCommandCallbackArgs& args) {
        cb(args);

        if (!args.response.isOK()) {
            LOG(1) << "Error processing the remote request, not updating operationTime or gLE";
            return;
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
            auto swShardingMetadata =
                rpc::ShardingMetadata::readFromMetadata(args.response.metadata);
            if (swShardingMetadata.isOK()) {
                auto shardingMetadata = std::move(swShardingMetadata.getValue());

                auto shardConn = ConnectionString::parse(request.target.toString());
                if (!shardConn.isOK()) {
                    severe() << "got bad host string in saveGLEStats: " << request.target;
                }

                clusterGLE->addHostOpTime(shardConn.getValue(),
                                          HostOpTime(shardingMetadata.getLastOpTime(),
                                                     shardingMetadata.getLastElectionId()));
            } else if (swShardingMetadata.getStatus() != ErrorCodes::NoSuchKey) {
                warning() << "Got invalid sharding metadata "
                          << redact(swShardingMetadata.getStatus()) << " metadata object was '"
                          << redact(args.response.metadata) << "'";
            }
        }
    };

    return _executor->scheduleRemoteCommand(request, shardingCb);
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
