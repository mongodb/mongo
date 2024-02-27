/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <mutex>
#include <type_traits>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shardsvr_join_migrations_request_gen.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(pauseSubmissionsFailPoint);

void waitForQuiescedCluster(OperationContext* opCtx) {
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    ShardsvrJoinMigrations joinShardOnMigrationsRequest;
    joinShardOnMigrationsRequest.setDbName(DatabaseName::kAdmin);

    auto unquiescedShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    const auto responses =
        sharding_util::sendCommandToShards(opCtx,
                                           DatabaseName::kAdmin,
                                           joinShardOnMigrationsRequest.toBSON({}),
                                           unquiescedShardIds,
                                           executor,
                                           false /*throwOnError*/);
    for (const auto& r : responses) {
        auto responseOutcome = r.swResponse.isOK()
            ? getStatusFromCommandResult(r.swResponse.getValue().data)
            : r.swResponse.getStatus();

        if (!responseOutcome.isOK()) {
            LOGV2_WARNING(6648001,
                          "Could not complete _ShardsvrJoinMigrations on shard",
                          "error"_attr = responseOutcome,
                          "shard"_attr = r.shardId);
        }
    }
}


Status processRemoteResponse(const executor::RemoteCommandResponse& remoteResponse) {
    if (!remoteResponse.status.isOK()) {
        return remoteResponse.status;
    }
    auto remoteStatus = getStatusFromCommandResult(remoteResponse.data);
    return remoteStatus.withContext("Command request failed on source shard.");
}

}  // namespace

const std::string MergeChunksCommandInfo::kCommandName = "mergeChunks";
const std::string MergeChunksCommandInfo::kBounds = "bounds";
const std::string MergeChunksCommandInfo::kShardName = "shardName";
const std::string MergeChunksCommandInfo::kEpoch = "epoch";
const std::string MergeChunksCommandInfo::kTimestamp = "timestamp";

const std::string DataSizeCommandInfo::kCommandName = "dataSize";
const std::string DataSizeCommandInfo::kKeyPattern = "keyPattern";
const std::string DataSizeCommandInfo::kMinValue = "min";
const std::string DataSizeCommandInfo::kMaxValue = "max";
const std::string DataSizeCommandInfo::kEstimatedValue = "estimate";
const std::string DataSizeCommandInfo::kMaxSizeValue = "maxSize";

BalancerCommandsSchedulerImpl::BalancerCommandsSchedulerImpl() {}

BalancerCommandsSchedulerImpl::~BalancerCommandsSchedulerImpl() {
    stop();
}

void BalancerCommandsSchedulerImpl::start(OperationContext* opCtx) {
    LOGV2(5847200, "Balancer command scheduler start requested");
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(!_workerThreadHandle.joinable());
    if (!_executor) {
        _executor = std::make_unique<executor::ScopedTaskExecutor>(
            Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
    }
    _state = SchedulerState::Recovering;

    try {
        waitForQuiescedCluster(opCtx);
    } catch (const DBException& e) {
        LOGV2_WARNING(
            6648002, "Could not join migration activity on shards", "error"_attr = redact(e));
    }

    LOGV2(6648003, "Balancer scheduler recovery complete. Switching to regular execution");
    _state = SchedulerState::Running;

    _workerThreadHandle = stdx::thread([this] { _workerThread(); });
}

void BalancerCommandsSchedulerImpl::stop() {
    LOGV2(5847201, "Balancer command scheduler stop requested");
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_state == SchedulerState::Stopped) {
            return;
        }

        invariant(_workerThreadHandle.joinable());
        _state = SchedulerState::Stopping;
        _stateUpdatedCV.notify_all();
    }
    _workerThreadHandle.join();
}

void BalancerCommandsSchedulerImpl::disableBalancerForCollection(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
    auto commandInfo = std::make_shared<DisableBalancerCommandInfo>(nss, ShardId::kConfigServerId);

    _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(remoteResponse);
        })
        .getAsync([](auto) {});
}

SemiFuture<void> BalancerCommandsSchedulerImpl::requestMoveRange(
    OperationContext* opCtx,
    const ShardsvrMoveRange& request,
    const WriteConcernOptions& secondaryThrottleWC,
    bool issuedByRemoteUser) {
    auto externalClientInfo =
        issuedByRemoteUser ? boost::optional<ExternalClientInfo>(opCtx) : boost::none;

    auto commandInfo = std::make_shared<MoveRangeCommandInfo>(
        request, secondaryThrottleWC, std::move(externalClientInfo));

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(remoteResponse);
        })
        .semi();
}

SemiFuture<void> BalancerCommandsSchedulerImpl::requestMergeChunks(OperationContext* opCtx,
                                                                   const NamespaceString& nss,
                                                                   const ShardId& shardId,
                                                                   const ChunkRange& chunkRange,
                                                                   const ChunkVersion& version) {

    auto commandInfo = std::make_shared<MergeChunksCommandInfo>(
        nss, shardId, chunkRange.getMin(), chunkRange.getMax(), version);

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(remoteResponse);
        })
        .semi();
}

SemiFuture<DataSizeResponse> BalancerCommandsSchedulerImpl::requestDataSize(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ChunkRange& chunkRange,
    const ShardVersion& version,
    const KeyPattern& keyPattern,
    bool estimatedValue,
    int64_t maxSize) {
    auto commandInfo = std::make_shared<DataSizeCommandInfo>(nss,
                                                             shardId,
                                                             keyPattern.toBSON(),
                                                             chunkRange.getMin(),
                                                             chunkRange.getMax(),
                                                             estimatedValue,
                                                             maxSize,
                                                             version);

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse)
                  -> StatusWith<DataSizeResponse> {
            auto responseStatus = processRemoteResponse(remoteResponse);
            if (!responseStatus.isOK()) {
                return responseStatus;
            }
            long long sizeBytes = remoteResponse.data["size"].number();
            long long numObjects = remoteResponse.data["numObjects"].number();
            bool maxSizeReached = remoteResponse.data["maxReached"].trueValue();
            return DataSizeResponse(sizeBytes, numObjects, maxSizeReached);
        })
        .semi();
}

SemiFuture<NumMergedChunks> BalancerCommandsSchedulerImpl::requestMergeAllChunksOnShard(
    OperationContext* opCtx, const NamespaceString& nss, const ShardId& shardId) {
    auto commandInfo = std::make_shared<MergeAllChunksOnShardCommandInfo>(nss, shardId);
    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse)
                  -> StatusWith<NumMergedChunks> {
            auto responseStatus = processRemoteResponse(remoteResponse);
            if (!responseStatus.isOK()) {
                return responseStatus;
            }

            return MergeAllChunksOnShardResponse::parse(
                       IDLParserContext{"MergeAllChunksOnShardResponse"}, remoteResponse.data)
                .getNumMergedChunks();
        })
        .semi();
}

SemiFuture<void> BalancerCommandsSchedulerImpl::requestMoveCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& toShardId,
    const ShardId& dbPrimaryShardId,
    const DatabaseVersion& dbVersion) {
    auto commandInfo =
        std::make_shared<MoveCollectionCommandInfo>(nss, toShardId, dbPrimaryShardId, dbVersion);

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(remoteResponse);
        })
        .semi();
}

Future<executor::RemoteCommandResponse> BalancerCommandsSchedulerImpl::_buildAndEnqueueNewRequest(
    OperationContext* opCtx, std::shared_ptr<CommandInfo>&& commandInfo) {
    const auto newRequestId = UUID::gen();
    LOGV2_DEBUG(5847202,
                2,
                "Enqueuing new Balancer command request",
                "reqId"_attr = newRequestId,
                "command"_attr = redact(commandInfo->serialise().toString()),
                "recoveryDocRequired"_attr = commandInfo->requiresRecoveryOnCrash());

    RequestData pendingRequest(newRequestId, std::move(commandInfo));

    stdx::unique_lock<Latch> ul(_mutex);
    _stateUpdatedCV.wait(ul, [this] { return _state != SchedulerState::Recovering; });
    auto outcomeFuture = pendingRequest.getOutcomeFuture();
    _enqueueRequest(ul, std::move(pendingRequest));
    return outcomeFuture;
}

void BalancerCommandsSchedulerImpl::_enqueueRequest(WithLock, RequestData&& request) {
    auto requestId = request.getId();
    if (_state == SchedulerState::Recovering || _state == SchedulerState::Running) {
        // A request with persisted recovery info may be enqueued more than once when received while
        // the node is transitioning from Stopped to Recovering; if this happens, just resolve as a
        // no-op.
        if (_requests.find(requestId) == _requests.end()) {
            _requests.emplace(std::make_pair(requestId, std::move(request)));
            _unsubmittedRequestIds.push_back(requestId);
            _stateUpdatedCV.notify_all();
        }
    } else {
        request.setOutcome(Status(ErrorCodes::BalancerInterrupted,
                                  "Request rejected - balancer scheduler is stopped"));
    }
}

CommandSubmissionResult BalancerCommandsSchedulerImpl::_submit(
    OperationContext* opCtx, const CommandSubmissionParameters& params) {
    LOGV2_DEBUG(
        5847203, 2, "Balancer command request submitted for execution", "reqId"_attr = params.id);
    try {
        const auto shardWithStatus =
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, params.commandInfo->getTarget());
        if (!shardWithStatus.isOK()) {
            return CommandSubmissionResult(params.id, shardWithStatus.getStatus());
        }

        const auto shardHostWithStatus = shardWithStatus.getValue()->getTargeter()->findHost(
            opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
        if (!shardHostWithStatus.isOK()) {
            return CommandSubmissionResult(params.id, shardHostWithStatus.getStatus());
        }

        const executor::RemoteCommandRequest remoteCommand =
            executor::RemoteCommandRequest(shardHostWithStatus.getValue(),
                                           params.commandInfo->getTargetDb(),
                                           params.commandInfo->serialise(),
                                           opCtx);
        auto onRemoteResponseReceived =
            [this,
             requestId = params.id](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                _applyCommandResponse(requestId, args.response);
            };

        auto swRemoteCommandHandle =
            (*_executor)->scheduleRemoteCommand(remoteCommand, onRemoteResponseReceived);
        return CommandSubmissionResult(params.id, swRemoteCommandHandle.getStatus());
    } catch (const DBException& e) {
        return CommandSubmissionResult(params.id, e.toStatus());
    }
}

void BalancerCommandsSchedulerImpl::_applySubmissionResult(
    WithLock, CommandSubmissionResult&& submissionResult) {
    auto submittedRequestIt = _requests.find(submissionResult.id);
    tassert(8245206,
            "Submission result ID not found in the requests",
            submittedRequestIt != _requests.end());
    auto& submittedRequest = submittedRequestIt->second;
    auto submissionOutcome = submittedRequest.applySubmissionResult(std::move(submissionResult));
    if (!submissionOutcome.isOK()) {
        // The request was resolved as failed on submission time - move it to the complete list.
        _recentlyCompletedRequestIds.emplace_back(submittedRequestIt->first);
        if (_state == SchedulerState::Recovering && --_numRequestsToRecover == 0) {
            LOGV2(5847213, "Balancer scheduler recovery complete. Switching to regular execution");
            _state = SchedulerState::Running;
        }
    }
}

void BalancerCommandsSchedulerImpl::_applyCommandResponse(
    UUID requestId, const executor::RemoteCommandResponse& response) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        tassert(8245207, "Scheduler is stopped", _state != SchedulerState::Stopped);
        auto requestIt = _requests.find(requestId);
        tassert(8245208, "Request ID is already in use", requestIt != _requests.end());
        auto& request = requestIt->second;
        request.setOutcome(response);
        _recentlyCompletedRequestIds.emplace_back(request.getId());
        if (_state == SchedulerState::Recovering && --_numRequestsToRecover == 0) {
            LOGV2(5847207, "Balancer scheduler recovery complete. Switching to regular execution");
            _state = SchedulerState::Running;
        }
        _stateUpdatedCV.notify_all();
    }
    LOGV2_DEBUG(5847204,
                2,
                "Execution of balancer command request completed",
                "reqId"_attr = requestId,
                "response"_attr = response);
}

void BalancerCommandsSchedulerImpl::_workerThread() {
    ON_BLOCK_EXIT([this] {
        LOGV2(5847208, "Leaving balancer command scheduler thread");
        stdx::lock_guard<Latch> lg(_mutex);
        _state = SchedulerState::Stopped;
        _stateUpdatedCV.notify_all();
    });

    Client::initThread("BalancerCommandsScheduler",
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer));

    bool stopWorkerRequested = false;
    LOGV2(5847205, "Balancer scheduler thread started");

    while (!stopWorkerRequested) {
        std::vector<CommandSubmissionParameters> commandsToSubmit;
        std::vector<CommandSubmissionResult> submissionResults;

        // 1. Check the internal state and plan for the actions to be taken ont this round.
        {
            stdx::unique_lock<Latch> ul(_mutex);
            tassert(8245209, "Scheduler is stopped", _state != SchedulerState::Stopped);
            _stateUpdatedCV.wait(ul, [this] {
                return ((!_unsubmittedRequestIds.empty() &&
                         !MONGO_likely(pauseSubmissionsFailPoint.shouldFail())) ||
                        _state == SchedulerState::Stopping ||
                        !_recentlyCompletedRequestIds.empty());
            });

            for (const auto& requestId : _recentlyCompletedRequestIds) {
                auto it = _requests.find(requestId);
                _requests.erase(it);
            }
            _recentlyCompletedRequestIds.clear();

            for (const auto& requestId : _unsubmittedRequestIds) {
                auto& requestData = _requests.at(requestId);
                if (_state != SchedulerState::Stopping) {
                    commandsToSubmit.push_back(requestData.getSubmissionParameters());
                } else {
                    requestData.setOutcome(
                        Status(ErrorCodes::BalancerInterrupted,
                               "Request cancelled - balancer scheduler is stopping"));
                    _requests.erase(requestId);
                }
            }
            _unsubmittedRequestIds.clear();
            stopWorkerRequested = _state == SchedulerState::Stopping;
        }

        // 2. Serve the picked up requests, submitting their related commands.
        for (auto& submissionInfo : commandsToSubmit) {
            auto opCtxHolder = cc().makeOperationContext();
            if (submissionInfo.commandInfo) {
                submissionInfo.commandInfo.get()->attachOperationMetadataTo(opCtxHolder.get());
            }
            submissionResults.push_back(_submit(opCtxHolder.get(), submissionInfo));
            if (!submissionResults.back().outcome.isOK()) {
                LOGV2(5847206,
                      "Submission for scheduler command request failed",
                      "reqId"_attr = submissionResults.back().id,
                      "cause"_attr = submissionResults.back().outcome);
            }
        }

        // 3. Process the outcome of each submission.
        if (!submissionResults.empty()) {
            stdx::lock_guard<Latch> lg(_mutex);
            for (auto& submissionResult : submissionResults) {
                _applySubmissionResult(lg, std::move(submissionResult));
            }
        }
    }
    // Wait for each outstanding command to complete, clean out its resources and leave.
    (*_executor)->shutdown();
    (*_executor)->join();

    {
        stdx::unique_lock<Latch> ul(_mutex);
        _requests.clear();
        _recentlyCompletedRequestIds.clear();
        _executor.reset();
    }
}


}  // namespace mongo
