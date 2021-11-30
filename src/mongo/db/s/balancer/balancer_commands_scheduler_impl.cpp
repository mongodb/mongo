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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(pauseSubmissionsFailPoint);
MONGO_FAIL_POINT_DEFINE(deferredCleanupCompletedCheckpoint);

Status processRemoteResponse(OperationContext* opCtx,
                             const executor::RemoteCommandResponse& remoteResponse) {
    // Since requests are executed by a separate thread, the related operationTime needs to be
    // explicitly retrieved and set on the original context of the requestor to ensure
    // it will be propagated back to the router.
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    replClient.setLastOpToSystemLastOpTime(opCtx);

    if (!remoteResponse.status.isOK()) {
        return remoteResponse.status;
    }
    auto remoteStatus = getStatusFromCommandResult(remoteResponse.data);
    return Shard::shouldErrorBePropagated(remoteStatus.code())
        ? remoteStatus
        : Status(ErrorCodes::OperationFailed,
                 str::stream() << "Command request failed on source shard. "
                               << causedBy(remoteStatus));
}

}  // namespace

const std::string MergeChunksCommandInfo::kCommandName = "mergeChunks";
const std::string MergeChunksCommandInfo::kBounds = "bounds";
const std::string MergeChunksCommandInfo::kShardName = "shardName";
const std::string MergeChunksCommandInfo::kEpoch = "epoch";

const std::string DataSizeCommandInfo::kCommandName = "dataSize";
const std::string DataSizeCommandInfo::kKeyPattern = "keyPattern";
const std::string DataSizeCommandInfo::kMinValue = "min";
const std::string DataSizeCommandInfo::kMaxValue = "max";
const std::string DataSizeCommandInfo::kEstimatedValue = "estimate";

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

void BalancerCommandsSchedulerImpl::start(OperationContext* opCtx) {
    LOGV2(5847200, "Balancer command scheduler start requested");
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(!_workerThreadHandle.joinable());
    if (!_executor) {
        _executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    }
    auto requestsToRecover = _loadRequestsToRecover(opCtx);
    _numRequestsToRecover = requestsToRecover.size();
    _state = _numRequestsToRecover == 0 ? SchedulerState::Running : SchedulerState::Recovering;

    for (auto& requestToRecover : requestsToRecover) {
        _enqueueRequest(lg, std::move(requestToRecover));
    }

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

SemiFuture<void> BalancerCommandsSchedulerImpl::requestMoveChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardId& recipient,
    const MoveChunkSettings& commandSettings,
    bool issuedByRemoteUser) {

    auto externalClientInfo =
        issuedByRemoteUser ? boost::optional<ExternalClientInfo>(opCtx) : boost::none;

    auto commandInfo = std::make_shared<MoveChunkCommandInfo>(nss,
                                                              chunk.getShard(),
                                                              recipient,
                                                              chunk.getMin(),
                                                              chunk.getMax(),
                                                              commandSettings.maxChunkSizeBytes,
                                                              commandSettings.secondaryThrottle,
                                                              commandSettings.waitForDelete,
                                                              commandSettings.forceJumbo,
                                                              chunk.getVersion(),
                                                              std::move(externalClientInfo));

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([opCtx](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(opCtx, remoteResponse);
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
        .then([opCtx](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(opCtx, remoteResponse);
        })
        .semi();
}

SemiFuture<std::vector<BSONObj>> BalancerCommandsSchedulerImpl::requestAutoSplitVector(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const BSONObj& keyPattern,
    const BSONObj& minKey,
    const BSONObj& maxKey,
    int64_t maxChunkSizeBytes) {
    auto commandInfo = std::make_shared<AutoSplitVectorCommandInfo>(
        nss, shardId, keyPattern, minKey, maxKey, maxChunkSizeBytes);
    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([opCtx](const executor::RemoteCommandResponse& remoteResponse)
                  -> StatusWith<std::vector<BSONObj>> {
            auto responseStatus = processRemoteResponse(opCtx, remoteResponse);
            if (!responseStatus.isOK()) {
                return responseStatus;
            }
            const auto payload = AutoSplitVectorResponse::parse(
                IDLParserErrorContext("AutoSplitVectorResponse"), std::move(remoteResponse.data));
            return payload.getSplitKeys();
        })
        .semi();
}

SemiFuture<void> BalancerCommandsSchedulerImpl::requestSplitChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ChunkVersion& collectionVersion,
    const KeyPattern& keyPattern,
    const BSONObj& minKey,
    const BSONObj& maxKey,
    const std::vector<BSONObj>& splitPoints) {

    auto commandInfo = std::make_shared<SplitChunkCommandInfo>(
        nss, shardId, keyPattern.toBSON(), minKey, maxKey, collectionVersion, splitPoints);

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([opCtx](const executor::RemoteCommandResponse& remoteResponse) {
            return processRemoteResponse(opCtx, remoteResponse);
        })
        .semi();
}

SemiFuture<DataSizeResponse> BalancerCommandsSchedulerImpl::requestDataSize(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ChunkRange& chunkRange,
    const ChunkVersion& version,
    const KeyPattern& keyPattern,
    bool estimatedValue) {
    auto commandInfo = std::make_shared<DataSizeCommandInfo>(nss,
                                                             shardId,
                                                             keyPattern.toBSON(),
                                                             chunkRange.getMin(),
                                                             chunkRange.getMax(),
                                                             estimatedValue,
                                                             version);

    return _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo))
        .then([opCtx](const executor::RemoteCommandResponse& remoteResponse)
                  -> StatusWith<DataSizeResponse> {
            auto responseStatus = processRemoteResponse(opCtx, remoteResponse);
            if (!responseStatus.isOK()) {
                return responseStatus;
            }
            long long sizeBytes = remoteResponse.data["size"].number();
            long long numObjects = remoteResponse.data["numObjects"].number();
            return DataSizeResponse(sizeBytes, numObjects);
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

    if (commandInfo->requiresRecoveryOnCrash()) {
        DBDirectClient dbClient(opCtx);
        PersistedBalancerCommand recoveryDoc(newRequestId,
                                             commandInfo->serialise(),
                                             commandInfo->getTarget(),
                                             commandInfo->getNameSpace(),
                                             commandInfo->requiresDistributedLock());
        std::vector<BSONObj> serialisedRecoveryInfo;
        serialisedRecoveryInfo.emplace_back(recoveryDoc.toBSON());
        auto reply = dbClient.insertAcknowledged(
            NamespaceString::kConfigBalancerCommandsNamespace.toString(),
            serialisedRecoveryInfo,
            true,
            WriteConcernOptions::Majority);

        if (auto writeStatus = getStatusFromWriteCommandReply(reply); !writeStatus.isOK()) {
            LOGV2_WARNING(5847210,
                          "Failed to persist request command document",
                          "reqId"_attr = newRequestId,
                          "error"_attr = writeStatus);
            return Future<executor::RemoteCommandResponse>::makeReady(writeStatus);
        }
    }

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
    bool distLockTaken = false;

    const auto shardWithStatus =
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, params.commandInfo->getTarget());
    if (!shardWithStatus.isOK()) {
        return CommandSubmissionResult(params.id, distLockTaken, shardWithStatus.getStatus());
    }

    const auto shardHostWithStatus = shardWithStatus.getValue()->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!shardHostWithStatus.isOK()) {
        return CommandSubmissionResult(params.id, distLockTaken, shardHostWithStatus.getStatus());
    }

    const executor::RemoteCommandRequest remoteCommand(shardHostWithStatus.getValue(),
                                                       NamespaceString::kAdminDb.toString(),
                                                       params.commandInfo->serialise(),
                                                       opCtx);

    auto onRemoteResponseReceived =
        [this,
         requestId = params.id](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            _applyCommandResponse(requestId, args.response);
        };

    if (params.commandInfo->requiresDistributedLock()) {
        Status lockAcquisitionResponse =
            _distributedLocks.acquireFor(opCtx, params.commandInfo->getNameSpace());
        if (!lockAcquisitionResponse.isOK()) {
            return CommandSubmissionResult(params.id, distLockTaken, lockAcquisitionResponse);
        }
        distLockTaken = true;
    }

    auto swRemoteCommandHandle =
        _executor->scheduleRemoteCommand(remoteCommand, onRemoteResponseReceived);
    return (
        swRemoteCommandHandle.isOK()
            ? CommandSubmissionResult(params.id, distLockTaken, swRemoteCommandHandle.getValue())
            : CommandSubmissionResult(params.id, distLockTaken, swRemoteCommandHandle.getStatus()));
}

void BalancerCommandsSchedulerImpl::_applySubmissionResult(
    WithLock, CommandSubmissionResult&& submissionResult) {
    auto submittedRequestIt = _requests.find(submissionResult.id);
    invariant(submittedRequestIt != _requests.end());
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
        if (_state == SchedulerState::Stopping || _state == SchedulerState::Stopped) {
            // Drop the response - the request is being cancelled in the worker thread.
            return;
        }
        auto requestIt = _requests.find(requestId);
        invariant(requestIt != _requests.end());
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

std::vector<RequestData> BalancerCommandsSchedulerImpl::_loadRequestsToRecover(
    OperationContext* opCtx) {
    std::vector<RequestData> requestsToRecover;
    auto documentProcessor = [&requestsToRecover, opCtx](const BSONObj& commandToRecoverDoc) {
        auto originalCommand = PersistedBalancerCommand::parse(
            IDLParserErrorContext("BalancerCommandsScheduler"), commandToRecoverDoc);
        auto recoveryCommand = std::make_shared<RecoveryCommandInfo>(originalCommand);
        LOGV2_DEBUG(5847212,
                    1,
                    "Command request recovered and set for rescheduling",
                    "reqId"_attr = originalCommand.getRequestId(),
                    "command"_attr = redact(recoveryCommand->serialise()));
        requestsToRecover.emplace_back(originalCommand.getRequestId(), std::move(recoveryCommand));
    };
    DBDirectClient dbClient(opCtx);
    try {
        dbClient.query(
            documentProcessor, NamespaceString::kConfigBalancerCommandsNamespace, BSONObj());
    } catch (const DBException& e) {
        LOGV2_ERROR(5847215, "Failed to load requests to recover", "error"_attr = redact(e));
    }

    return requestsToRecover;
}

void BalancerCommandsSchedulerImpl::_performDeferredCleanup(
    OperationContext* opCtx, std::vector<RequestData>&& requestsHoldingResources) {
    std::vector<UUID> persistedRequestsIds;
    for (const auto& request : requestsHoldingResources) {
        if (request.holdsDistributedLock()) {
            _distributedLocks.releaseFor(opCtx, request.getNamespace());
        }
        if (request.isRecoverable()) {
            persistedRequestsIds.emplace_back(request.getId());
        }
    }

    if (persistedRequestsIds.empty()) {
        return;
    }
    BSONArrayBuilder idsToRemoveBuilder;
    for (const auto& requestId : persistedRequestsIds) {
        requestId.appendToArrayBuilder(&idsToRemoveBuilder);
    }
    BSONObjBuilder queryBuilder;
    queryBuilder.append(PersistedBalancerCommand::kRequestIdFieldName,
                        BSON("$in" << idsToRemoveBuilder.arr()));

    auto query = queryBuilder.obj();
    DBDirectClient dbClient(opCtx);
    try {
        dbClient.remove(NamespaceString::kConfigBalancerCommandsNamespace.toString(), query);
    } catch (const DBException& e) {
        LOGV2_ERROR(5847214, "Failed to remove recovery info", "error"_attr = redact(e));
    }

    deferredCleanupCompletedCheckpoint.pauseWhileSet();
}

void BalancerCommandsSchedulerImpl::_workerThread() {
    ON_BLOCK_EXIT([this] {
        LOGV2(5847208, "Leaving balancer command scheduler thread");
        stdx::lock_guard<Latch> lg(_mutex);
        _state = SchedulerState::Stopped;
        _stateUpdatedCV.notify_all();
    });

    Client::initThread("BalancerCommandsScheduler");
    bool stopWorkerRequested = false;
    stdx::unordered_map<UUID, RequestData, UUID::Hash> requestsToCleanUpOnExit;
    LOGV2(5847205, "Balancer scheduler thread started");

    while (!stopWorkerRequested) {
        std::vector<CommandSubmissionParameters> commandsToSubmit;
        std::vector<CommandSubmissionResult> submissionResults;
        std::vector<RequestData> completedRequestsToCleanUp;

        // 1. Check the internal state and plan for the actions to be taken ont this round.
        {
            stdx::unique_lock<Latch> ul(_mutex);
            invariant(_state != SchedulerState::Stopped);
            _stateUpdatedCV.wait(ul, [this, &ul] {
                return ((!_unsubmittedRequestIds.empty() &&
                         !MONGO_likely(pauseSubmissionsFailPoint.shouldFail())) ||
                        _state == SchedulerState::Stopping ||
                        !_recentlyCompletedRequestIds.empty());
            });

            for (const auto& requestId : _recentlyCompletedRequestIds) {
                auto it = _requests.find(requestId);
                completedRequestsToCleanUp.emplace_back(std::move(it->second));
                _requests.erase(it);
            }
            _recentlyCompletedRequestIds.clear();

            if (_state == SchedulerState::Stopping) {
                // reset the internal state and
                _unsubmittedRequestIds.clear();
                _requests.swap(requestsToCleanUpOnExit);
                stopWorkerRequested = true;
            } else {
                // Pick up new commands to be submitted
                for (const auto& requestId : _unsubmittedRequestIds) {
                    const auto& requestData = _requests.at(requestId);
                    commandsToSubmit.push_back(requestData.getSubmissionParameters());
                }
                _unsubmittedRequestIds.clear();
            }
        }

        // 2.a Free any resource acquired by already completed/aborted requests.
        {
            auto opCtxHolder = cc().makeOperationContext();
            _performDeferredCleanup(opCtxHolder.get(), std::move(completedRequestsToCleanUp));
        }

        // 2.b Serve the picked up requests, submitting their related commands.
        for (auto& submissionInfo : commandsToSubmit) {
            auto opCtxHolder = cc().makeOperationContext();
            if (submissionInfo.commandInfo) {
                submissionInfo.commandInfo.get()->attachOperationMetadataTo(opCtxHolder.get());
            }
            submissionResults.push_back(_submit(opCtxHolder.get(), submissionInfo));
            if (!submissionResults.back().context.isOK()) {
                LOGV2(5847206,
                      "Submission for scheduler command request failed",
                      "reqId"_attr = submissionResults.back().id,
                      "cause"_attr = submissionResults.back().context.getStatus());
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

    // In case of clean exit, cancel all the pending/running command requests
    // (but keep the related descriptor documents to ensure they will be reissued on recovery).
    auto opCtxHolder = cc().makeOperationContext();
    for (auto& idAndRequest : requestsToCleanUpOnExit) {
        idAndRequest.second.setOutcome(Status(
            ErrorCodes::BalancerInterrupted, "Request cancelled - balancer scheduler is stopping"));
        const auto& cancelHandle = idAndRequest.second.getExecutionContext();
        if (cancelHandle) {
            _executor->cancel(*cancelHandle);
        }
        _distributedLocks.releaseFor(opCtxHolder.get(), idAndRequest.second.getNamespace());
    }
}


}  // namespace mongo
