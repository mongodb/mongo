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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseBalancerWorkerThread);

const std::string MergeChunksCommandInfo::kCommandName = "mergeChunks";
const std::string MergeChunksCommandInfo::kBounds = "bounds";
const std::string MergeChunksCommandInfo::kShardName = "shardName";
const std::string MergeChunksCommandInfo::kEpoch = "epoch";

const std::string DataSizeCommandInfo::kCommandName = "dataSize";
const std::string DataSizeCommandInfo::kKeyPattern = "keyPattern";
const std::string DataSizeCommandInfo::kMinValue = "min";
const std::string DataSizeCommandInfo::kMaxValue = "max";
const std::string DataSizeCommandInfo::kEstimatedValue = "estimate";

const std::string SplitVectorCommandInfo::kCommandName = "splitVector";
const std::string SplitVectorCommandInfo::kKeyPattern = "keyPattern";
const std::string SplitVectorCommandInfo::kLowerBound = "min";
const std::string SplitVectorCommandInfo::kUpperBound = "max";
const std::string SplitVectorCommandInfo::kMaxChunkSize = "maxChunkSize";
const std::string SplitVectorCommandInfo::kMaxSplitPoints = "maxSplitPoints";
const std::string SplitVectorCommandInfo::kMaxChunkObjects = "maxChunkObjects";
const std::string SplitVectorCommandInfo::kForceSplit = "force";

const std::string SplitChunkCommandInfo::kCommandName = "splitChunk";
const std::string SplitChunkCommandInfo::kShardName = "from";
const std::string SplitChunkCommandInfo::kKeyPattern = "keyPattern";
const std::string SplitChunkCommandInfo::kLowerBound = "min";
const std::string SplitChunkCommandInfo::kUpperBound = "max";
const std::string SplitChunkCommandInfo::kEpoch = "epoch";
const std::string SplitChunkCommandInfo::kSplitKeys = "splitKeys";

BalancerCommandsSchedulerImpl::BalancerCommandsSchedulerImpl() {}

BalancerCommandsSchedulerImpl::~BalancerCommandsSchedulerImpl() {
    stop();
}

void BalancerCommandsSchedulerImpl::_setState(SchedulerState state) {
    _state = state;
    _stateUpdatedCV.notify_all();
}

void BalancerCommandsSchedulerImpl::start() {
    LOGV2(5847200, "Balancer command scheduler start requested");
    stdx::lock_guard<Latch> lgss(_startStopMutex);
    stdx::lock_guard<Latch> lg(_mutex);
    // TODO init _requestIdCounter here based on the stored running requests from a past invocation?
    invariant(!_workerThreadHandle.joinable());
    _incompleteRequests.reserve(_maxRunningRequests * 10);
    _state = SchedulerState::Running;
    _workerThreadHandle = stdx::thread([this] { _workerThread(); });
}

void BalancerCommandsSchedulerImpl::stop() {
    {
        LOGV2(5847201, "Balancer command scheduler stop requested");
        stdx::lock_guard<Latch> lgss(_startStopMutex);
        {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_state == SchedulerState::Stopped)
                return;
            invariant(_workerThreadHandle.joinable());
            _setState(SchedulerState::Stopping);
            _workerThreadHandle.detach();
        }
        stdx::unique_lock<Latch> ul(_mutex);
        _stateUpdatedCV.wait(ul, [this] { return _state == SchedulerState::Stopped; });
    }
}

std::unique_ptr<MoveChunkResponse> BalancerCommandsSchedulerImpl::requestMoveChunk(
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardId& recipient,
    const MoveChunkSettings& commandSettings) {

    auto commandInfo = std::make_shared<MoveChunkCommandInfo>(nss,
                                                              chunk.getShard(),
                                                              recipient,
                                                              chunk.getMin(),
                                                              chunk.getMax(),
                                                              commandSettings.maxChunkSizeBytes,
                                                              commandSettings.secondaryThrottle,
                                                              commandSettings.waitForDelete,
                                                              commandSettings.forceJumbo,
                                                              chunk.getVersion());

    auto requestCollectionInfo = _enqueueNewRequest(std::move(commandInfo));
    return std::make_unique<MoveChunkResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<MergeChunksResponse> BalancerCommandsSchedulerImpl::requestMergeChunks(
    const NamespaceString& nss, const ChunkType& lowerBound, const ChunkType& upperBound) {
    invariant(lowerBound.getShard() == upperBound.getShard());
    invariant(lowerBound.getMax().woCompare(upperBound.getMin()) <= 0);
    // TODO why this may fail?
    // invariant(lowerBound.epoch() == upperBound.epoch() &&
    //           lowerBound.getVersion().majorVersion() == upperBound.getVersion().majorVersion());

    auto commandInfo = std::make_shared<MergeChunksCommandInfo>(
        nss,
        lowerBound.getShard(),
        lowerBound.getMin(),
        upperBound.getMax(),
        lowerBound.getVersion().isOlderThan(upperBound.getVersion()) ? upperBound.getVersion()
                                                                     : lowerBound.getVersion());

    auto requestCollectionInfo = _enqueueNewRequest(std::move(commandInfo));
    return std::make_unique<MergeChunksResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<SplitVectorResponse> BalancerCommandsSchedulerImpl::requestSplitVector(
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardKeyPattern& shardKeyPattern,
    const SplitVectorSettings& commandSettings) {

    auto commandInfo = std::make_shared<SplitVectorCommandInfo>(nss,
                                                                chunk.getShard(),
                                                                shardKeyPattern.toBSON(),
                                                                chunk.getMin(),
                                                                chunk.getMax(),
                                                                commandSettings.maxSplitPoints,
                                                                commandSettings.maxChunkObjects,
                                                                commandSettings.maxChunkSizeBytes,
                                                                commandSettings.force);

    auto requestCollectionInfo = _enqueueNewRequest(std::move(commandInfo));
    return std::make_unique<SplitVectorResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<SplitChunkResponse> BalancerCommandsSchedulerImpl::requestSplitChunk(
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardKeyPattern& shardKeyPattern,
    const std::vector<BSONObj>& splitPoints) {

    auto commandInfo = std::make_shared<SplitChunkCommandInfo>(nss,
                                                               chunk.getShard(),
                                                               shardKeyPattern.toBSON(),
                                                               chunk.getMin(),
                                                               chunk.getMax(),
                                                               chunk.getVersion(),
                                                               splitPoints);

    auto requestCollectionInfo = _enqueueNewRequest(std::move(commandInfo));
    return std::make_unique<SplitChunkResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<ChunkDataSizeResponse> BalancerCommandsSchedulerImpl::requestChunkDataSize(
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardKeyPattern& shardKeyPattern,
    bool estimatedValue) {
    auto commandInfo = std::make_shared<DataSizeCommandInfo>(nss,
                                                             chunk.getShard(),
                                                             shardKeyPattern.toBSON(),
                                                             chunk.getMin(),
                                                             chunk.getMax(),
                                                             estimatedValue,
                                                             chunk.getVersion());

    auto requestCollectionInfo = _enqueueNewRequest(std::move(commandInfo));
    return std::make_unique<ChunkDataSizeResponseImpl>(std::move(requestCollectionInfo));
}

ResponseHandle BalancerCommandsSchedulerImpl::_enqueueNewRequest(
    std::shared_ptr<CommandInfo>&& commandInfo) {
    const auto newRequestId = _requestIdCounter.fetchAndAddRelaxed(1);
    LOGV2(5847202,
          "Enqueuing new Balancer command request with id {reqId}. Details: {command} ",
          "Enqueuing new Balancer command request",
          "reqId"_attr = newRequestId,
          "command"_attr = commandInfo->serialise().toString());

    RequestData pendingRequest(newRequestId, std::move(commandInfo));
    auto deferredResponseHandle = pendingRequest.getResponseHandle();
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_state == SchedulerState::Running) {
            invariant(_workerThreadHandle.joinable());
            _incompleteRequests.emplace(std::make_pair(newRequestId, std::move(pendingRequest)));
            _pendingRequestIds.push_back(newRequestId);
            _newInfoOnSubmittableRequests = true;
            _stateUpdatedCV.notify_all();
        } else {
            deferredResponseHandle.handle->set(Status(
                ErrorCodes::CallbackCanceled, "Request rejected - balancer scheduler is stopped"));
        }
    }
    return deferredResponseHandle;
}

Status BalancerCommandsSchedulerImpl::_acquireDistLock(OperationContext* opCtx,
                                                       NamespaceString nss) {
    auto it = _migrationLocks.find(nss);
    if (it != _migrationLocks.end()) {
        ++it->second.numMigrations;
        return Status::OK();
    } else {
        boost::optional<DistLockManager::ScopedLock> scopedLock;
        try {
            scopedLock.emplace(DistLockManager::get(opCtx)->lockDirectLocally(
                opCtx, nss.ns(), DistLockManager::kSingleLockAttemptTimeout));

            const std::string whyMessage(str::stream()
                                         << "Migrating chunk(s) in collection " << nss.ns());
            uassertStatusOK(DistLockManager::get(opCtx)->lockDirect(
                opCtx, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout));
        } catch (const DBException& ex) {
            return ex.toStatus(str::stream() << "Could not acquire collection lock for " << nss.ns()
                                             << " to migrate chunks");
        }
        Migrations migrationData(std::move(*scopedLock));
        _migrationLocks.insert(std::make_pair(nss, std::move(migrationData)));
    }
    return Status::OK();
}

void BalancerCommandsSchedulerImpl::_releaseDistLock(OperationContext* opCtx, NamespaceString nss) {
    auto it = _migrationLocks.find(nss);
    if (it == _migrationLocks.end()) {
        return;
    } else if (it->second.numMigrations == 1) {
        DistLockManager::get(opCtx)->unlock(opCtx, nss.ns());
        _migrationLocks.erase(it);
    } else {
        --it->second.numMigrations;
    }
}

CommandSubmissionResult BalancerCommandsSchedulerImpl::_submit(
    OperationContext* opCtx, const CommandSubmissionHandle& handle) {
    LOGV2(5847203,
          "Balancer command request id {reqId} submitted for execution",
          "Balancer command request submitted for execution",
          "reqId"_attr = handle.id);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto shardWithStatus =
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, handle.commandInfo->getTarget());
    if (!shardWithStatus.isOK()) {
        return CommandSubmissionResult(handle.id, false, shardWithStatus.getStatus());
    }

    const auto shardHostWithStatus = shardWithStatus.getValue()->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!shardHostWithStatus.isOK()) {
        return CommandSubmissionResult(handle.id, false, shardHostWithStatus.getStatus());
    }

    const executor::RemoteCommandRequest remoteCommand(shardHostWithStatus.getValue(),
                                                       NamespaceString::kAdminDb.toString(),
                                                       handle.commandInfo->serialise(),
                                                       opCtx);

    auto onRemoteResponseReceived =
        [this, opCtx, requestId = handle.id](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            _applyCommandResponse(opCtx, requestId, args.response);
        };

    if (handle.commandInfo->getType() == CommandInfo::Type::kMoveChunk) {
        Status lockAcquisitionResponse =
            _acquireDistLock(opCtx, handle.commandInfo->getNameSpace());
        if (!lockAcquisitionResponse.isOK()) {
            return CommandSubmissionResult(handle.id, false, lockAcquisitionResponse);
        }
    }

    auto remoteCommandHandleWithStatus =
        executor->scheduleRemoteCommand(remoteCommand, onRemoteResponseReceived);
    return (
        remoteCommandHandleWithStatus.isOK()
            ? CommandSubmissionResult(handle.id, true, remoteCommandHandleWithStatus.getValue())
            : CommandSubmissionResult(handle.id, true, remoteCommandHandleWithStatus.getStatus()));
}

void BalancerCommandsSchedulerImpl::_applySubmissionResult(
    WithLock, OperationContext* opCtx, CommandSubmissionResult&& submissionResult) {
    auto requestToUpdateIt = _incompleteRequests.find(submissionResult.id);
    if (requestToUpdateIt == _incompleteRequests.end()) {
        return;
    }
    /*
     * Rules for processing the outcome of a command submission:
     * If successful,
     * - set the execution context on the RequestData to be able to serve future cancel requests
     * - update the data structure describing running command requests
     * If failed,
     * - persist the outcome into the deferred response
     * - remove the RequestData (and the info on migration)
     */
    auto& requestToUpdate = requestToUpdateIt->second;
    if (submissionResult.context.isOK()) {
        requestToUpdate.addExecutionContext(std::move(submissionResult.context.getValue()));
        ++_numRunningRequests;
    } else {
        const auto& submittedCommandInfo = requestToUpdate.getCommandInfo();
        if (submittedCommandInfo.getType() == CommandInfo::Type::kMoveChunk) {
            for (const auto& involvedShard : submittedCommandInfo.getInvolvedShards()) {
                _shardsPerformingMigrations.erase(involvedShard);
            }
        }
        if (submissionResult.acquiredDistLock) {
            _releaseDistLock(opCtx, submittedCommandInfo.getNameSpace());
        }
        requestToUpdate.setOutcome(submissionResult.context.getStatus());
        _incompleteRequests.erase(requestToUpdateIt);
    }
}

void BalancerCommandsSchedulerImpl::_applyCommandResponse(
    OperationContext* opCtx,
    uint32_t requestId,
    const executor::TaskExecutor::ResponseStatus& response) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        auto requestToCompleteIt = _incompleteRequests.find(requestId);
        if (_state != SchedulerState::Running || requestToCompleteIt == _incompleteRequests.end()) {
            // Drop the response - the request is being cancelled in the worker thread.
            // (TODO modify to handle stepdown scenarios).
            return;
        }
        auto& requestToComplete = requestToCompleteIt->second;
        requestToComplete.setOutcome(response);
        auto& commandInfo = requestToComplete.getCommandInfo();
        if (commandInfo.getType() == CommandInfo::Type::kMoveChunk) {
            for (const auto& shard : commandInfo.getInvolvedShards()) {
                _shardsPerformingMigrations.erase(shard);
            }
            _releaseDistLock(opCtx, commandInfo.getNameSpace());
        }
        _incompleteRequests.erase(requestToCompleteIt);
        _newInfoOnSubmittableRequests = true;
        --_numRunningRequests;
        invariant(_numRunningRequests >= 0);
        _stateUpdatedCV.notify_all();
    }
    LOGV2(5847204,
          "Execution of balancer command request id {reqId} completed",
          "Execution of balancer command request completed",
          "reqId"_attr = requestId,
          "response"_attr = response.toString());
}


void BalancerCommandsSchedulerImpl::_workerThread() {
    ON_BLOCK_EXIT([this] {
        invariant(_migrationLocks.empty(),
                  "BalancerCommandsScheduler worker thread failed to release all locks on exit");
        stdx::lock_guard<Latch> lg(_mutex);

        _setState(SchedulerState::Stopped);

        LOGV2(5847208, "Leaving balancer command scheduler thread");
    });

    Client::initThread("BalancerCommandsScheduler");
    auto opCtxHolder = cc().makeOperationContext();
    stdx::unordered_map<uint32_t, RequestData> requestsToCleanUpOnExit;
    LOGV2(5847205, "Balancer scheduler thread started");

    while (true) {
        std::vector<CommandSubmissionHandle> commandsToSubmit;
        {
            stdx::unique_lock<Latch> ul(_mutex);
            _stateUpdatedCV.wait(ul, [this] {
                return (_state != SchedulerState::Running ||
                        (!_pendingRequestIds.empty() && _numRunningRequests < _maxRunningRequests &&
                         _newInfoOnSubmittableRequests &&
                         MONGO_likely(!pauseBalancerWorkerThread.shouldFail())));
            });

            if (_state != SchedulerState::Running) {
                _numRunningRequests = 0;
                _pendingRequestIds.clear();
                _newInfoOnSubmittableRequests = false;
                _shardsPerformingMigrations.clear();
                _incompleteRequests.swap(requestsToCleanUpOnExit);
                break;
            }

            // 1. Pick up new requests to be submitted from the pending list, if possible.
            const auto availableSubmissionSlots =
                static_cast<size_t>(_maxRunningRequests - _numRunningRequests);
            for (auto it = _pendingRequestIds.cbegin(); it != _pendingRequestIds.end() &&
                 commandsToSubmit.size() < availableSubmissionSlots;) {
                const auto& requestData = _incompleteRequests.at(*it);
                const auto& commandInfo = requestData.getCommandInfo();
                bool canBeSubmitted = true;
                if (commandInfo.getType() == CommandInfo::Type::kMoveChunk) {
                    // Extra check - a shard can only be involved in one running moveChunk command.
                    auto shardsInCommand = commandInfo.getInvolvedShards();
                    canBeSubmitted = [this, &shardsInCommand] {
                        for (const auto& shard : shardsInCommand) {
                            if (_shardsPerformingMigrations.find(shard) !=
                                _shardsPerformingMigrations.end()) {
                                return false;
                            }
                        }
                        return true;
                    }();

                    if (canBeSubmitted) {
                        for (const auto& shard : shardsInCommand) {
                            _shardsPerformingMigrations.insert(shard);
                        }
                    }
                }

                if (canBeSubmitted) {
                    commandsToSubmit.push_back(requestData.getSubmissionInfo());
                    it = _pendingRequestIds.erase(it);
                } else {
                    ++it;
                }
            }

            if (commandsToSubmit.empty()) {
                _newInfoOnSubmittableRequests = false;
                continue;
            }
        }

        // 2. Serve the picked up requests, submitting their related commands.
        std::vector<CommandSubmissionResult> submissionResults;
        for (auto& submissionInfo : commandsToSubmit) {
            submissionResults.push_back(_submit(opCtxHolder.get(), submissionInfo));
            if (!submissionResults.back().context.isOK()) {
                LOGV2(5847206,
                      "Submission for scheduler command request {reqId} failed: cause {cause}",
                      "Submission for scheduler command request {reqId} failed",
                      "reqId"_attr = submissionResults.back().id,
                      "cause"_attr = submissionResults.back().context.getStatus());
            }
        }

        // 3. Process the outcome of each submission.
        {
            stdx::lock_guard<Latch> lg(_mutex);
            for (auto& submissionResult : submissionResults) {
                _applySubmissionResult(lg, opCtxHolder.get(), std::move(submissionResult));
            }
        }
        LOGV2(5847207, "Ending balancer command scheduler round");
    }

    // In case of clean exit, cancel all the pending/running command requests
    auto executor = Grid::get(opCtxHolder.get())->getExecutorPool()->getFixedExecutor();
    for (auto& idAndRequest : requestsToCleanUpOnExit) {
        idAndRequest.second.setOutcome(Status(
            ErrorCodes::CallbackCanceled, "Request cancelled - balancer scheduler is stopping"));
        const auto& cancelHandle = idAndRequest.second.getExecutionContext();
        if (cancelHandle) {
            executor->cancel(*cancelHandle);
        }
        _releaseDistLock(opCtxHolder.get(), idAndRequest.second.getCommandInfo().getNameSpace());
    }
}


}  // namespace mongo
