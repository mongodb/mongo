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

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/database_name.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_version.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Utility class to extract and hold information describing the remote client that submitted a
 * command.
 */
struct ExternalClientInfo {
    ExternalClientInfo(OperationContext* opCtx)
        : operationMetadata(opCtx), apiParameters(APIParameters::get(opCtx)) {}

    const ForwardableOperationMetadata operationMetadata;
    const APIParameters apiParameters;
};


/**
 * Base class describing the common traits of a shard command associated to a Request
 * received by BalancerCommandSchedulerImpl.
 */
class CommandInfo {
public:
    CommandInfo(const ShardId& targetShardId,
                const NamespaceString& nss,
                boost::optional<ExternalClientInfo>&& clientInfo)
        : _targetShardId(targetShardId), _nss(nss), _clientInfo(clientInfo) {}

    virtual ~CommandInfo() {}

    virtual BSONObj serialise() const = 0;

    virtual bool requiresRecoveryOnCrash() const {
        return false;
    }

    virtual bool requiresRecoveryCleanupOnCompletion() const {
        return false;
    }

    virtual DatabaseName getTargetDb() const {
        return DatabaseName::kAdmin;
    }

    const ShardId& getTarget() const {
        return _targetShardId;
    }

    const NamespaceString& getNameSpace() const {
        return _nss;
    }

    void attachOperationMetadataTo(OperationContext* opCtx) {
        if (_clientInfo) {
            _clientInfo.get().operationMetadata.setOn(opCtx);
        }
    }

    void appendCommandMetadataTo(BSONObjBuilder* commandBuilder) const {
        if (_clientInfo && _clientInfo.get().apiParameters.getParamsPassed()) {
            _clientInfo.get().apiParameters.appendInfo(commandBuilder);
        }
    }

private:
    ShardId _targetShardId;
    NamespaceString _nss;
    boost::optional<ExternalClientInfo> _clientInfo;
};

/**
 * Set of command-specific subclasses of CommandInfo.
 */

class MoveRangeCommandInfo : public CommandInfo {
public:
    MoveRangeCommandInfo(const ShardsvrMoveRange& request,
                         const WriteConcernOptions& writeConcern,
                         boost::optional<ExternalClientInfo>&& clientInfo)
        : CommandInfo(request.getFromShard(), request.getCommandParameter(), std::move(clientInfo)),
          _request(request),
          _wc(writeConcern) {}

    const ShardsvrMoveRange& getMoveRangeRequest() {
        return _request;
    }

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        _request.serialize(BSON(WriteConcernOptions::kWriteConcernField << _wc.toBSON()),
                           &commandBuilder);
        appendCommandMetadataTo(&commandBuilder);
        return commandBuilder.obj();
    }

private:
    const ShardsvrMoveRange _request;
    const WriteConcernOptions _wc;
};

class DisableBalancerCommandInfo : public CommandInfo {
public:
    DisableBalancerCommandInfo(const NamespaceString& nss, const ShardId& shardId)
        : CommandInfo(shardId, nss, boost::none) {}

    BSONObj serialise() const override {
        BSONObjBuilder updateCmd;
        updateCmd.append("$set", BSON("noBalance" << true));

        const auto updateOp = BatchedCommandRequest::buildUpdateOp(
            CollectionType::ConfigNS,
            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                     getNameSpace(), SerializationContext::stateDefault())) /* query */,
            updateCmd.obj() /* update */,
            false /* upsert */,
            false /* multi */);
        BSONObjBuilder cmdObj(updateOp.toBSON());
        cmdObj.append(WriteConcernOptions::kWriteConcernField,
                      WriteConcernOptions::kInternalWriteDefault);
        return cmdObj.obj();
    }

    DatabaseName getTargetDb() const override {
        return DatabaseName::kConfig;
    }
};

class MergeChunksCommandInfo : public CommandInfo {
public:
    MergeChunksCommandInfo(const NamespaceString& nss,
                           const ShardId& shardId,
                           const BSONObj& lowerBoundKey,
                           const BSONObj& upperBoundKey,
                           const ChunkVersion& version)
        : CommandInfo(shardId, nss, boost::none),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _version(version) {}

    BSONObj serialise() const override {
        BSONArrayBuilder boundsArrayBuilder;
        boundsArrayBuilder.append(_lowerBoundKey).append(_upperBoundKey);

        BSONObjBuilder commandBuilder;
        commandBuilder
            .append(kCommandName,
                    NamespaceStringUtil::serialize(getNameSpace(),
                                                   SerializationContext::stateDefault()))
            .appendArray(kBounds, boundsArrayBuilder.arr())
            .append(kShardName, getTarget().toString())
            .append(kEpoch, _version.epoch())
            .append(kTimestamp, _version.getTimestamp());

        _version.serialize(ChunkVersion::kChunkVersionField, &commandBuilder);

        return commandBuilder.obj();
    }

private:
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    ChunkVersion _version;

    static const std::string kCommandName;
    static const std::string kBounds;
    static const std::string kShardName;
    static const std::string kEpoch;
    static const std::string kTimestamp;
};

class DataSizeCommandInfo : public CommandInfo {
public:
    DataSizeCommandInfo(const NamespaceString& nss,
                        const ShardId& shardId,
                        const BSONObj& shardKeyPattern,
                        const BSONObj& lowerBoundKey,
                        const BSONObj& upperBoundKey,
                        bool estimatedValue,
                        int64_t maxSize,
                        const ShardVersion& version)
        : CommandInfo(shardId, nss, boost::none),
          _shardKeyPattern(shardKeyPattern),
          _lowerBoundKey(lowerBoundKey),
          _upperBoundKey(upperBoundKey),
          _estimatedValue(estimatedValue),
          _maxSize(maxSize),
          _version(version) {}

    BSONObj serialise() const override {
        BSONObjBuilder commandBuilder;
        commandBuilder
            .append(kCommandName,
                    NamespaceStringUtil::serialize(getNameSpace(),
                                                   SerializationContext::stateDefault()))
            .append(kKeyPattern, _shardKeyPattern)
            .append(kMinValue, _lowerBoundKey)
            .append(kMaxValue, _upperBoundKey)
            .append(kEstimatedValue, _estimatedValue)
            .append(kMaxSizeValue, _maxSize);

        _version.serialize(ShardVersion::kShardVersionField, &commandBuilder);

        return commandBuilder.obj();
    }

private:
    BSONObj _shardKeyPattern;
    BSONObj _lowerBoundKey;
    BSONObj _upperBoundKey;
    bool _estimatedValue;
    int64_t _maxSize;
    ShardVersion _version;

    static const std::string kCommandName;
    static const std::string kKeyPattern;
    static const std::string kMinValue;
    static const std::string kMaxValue;
    static const std::string kEstimatedValue;
    static const std::string kMaxSizeValue;
};

class MergeAllChunksOnShardCommandInfo : public CommandInfo {
public:
    MergeAllChunksOnShardCommandInfo(const NamespaceString& nss, const ShardId& shardId)
        : CommandInfo(shardId, nss, boost::none) {}

    BSONObj serialise() const override {
        ShardSvrMergeAllChunksOnShard req(getNameSpace(), getTarget());
        req.setMaxNumberOfChunksToMerge(AutoMergerPolicy::MAX_NUMBER_OF_CHUNKS_TO_MERGE);
        return req.toBSON({});
    }
};

class MoveCollectionCommandInfo : public CommandInfo {
public:
    MoveCollectionCommandInfo(const NamespaceString& nss,
                              const ShardId& toShardId,
                              const ShardId& dbPrimaryShard,
                              const DatabaseVersion& dbVersion)
        : CommandInfo(dbPrimaryShard, nss, boost::none),
          _toShardId(toShardId),
          _dbVersion(dbVersion) {}

    BSONObj serialise() const override {
        ShardsvrReshardCollection shardsvrReshardCollection(getNameSpace());
        shardsvrReshardCollection.setDbName(getNameSpace().dbName());

        ReshardCollectionRequest reshardCollectionRequest;
        reshardCollectionRequest.setKey(BSON("_id" << 1));
        reshardCollectionRequest.setProvenance(ProvenanceEnum::kBalancerMoveCollection);

        std::vector<mongo::ShardKeyRange> destinationShard = {_toShardId};
        reshardCollectionRequest.setShardDistribution(destinationShard);
        reshardCollectionRequest.setForceRedistribution(true);
        reshardCollectionRequest.setNumInitialChunks(1);

        shardsvrReshardCollection.setReshardCollectionRequest(std::move(reshardCollectionRequest));
        return appendDbVersionIfPresent(
            CommandHelpers::appendMajorityWriteConcern(shardsvrReshardCollection.toBSON({})),
            _dbVersion);
    }

private:
    const ShardId _toShardId;
    const DatabaseVersion _dbVersion;
};

/**
 * Helper data structure for submitting the remote command associated to a BalancerCommandsScheduler
 * Request.
 */
struct CommandSubmissionParameters {
    CommandSubmissionParameters(UUID id, const std::shared_ptr<CommandInfo>& commandInfo)
        : id(id), commandInfo(commandInfo) {}

    CommandSubmissionParameters(CommandSubmissionParameters&& rhs) noexcept
        : id(rhs.id), commandInfo(std::move(rhs.commandInfo)) {}

    const UUID id;
    std::shared_ptr<CommandInfo> commandInfo;
};

/**
 * Helper data structure for storing the outcome of a Command submission.
 */
struct CommandSubmissionResult {
    CommandSubmissionResult(UUID id, const Status& outcome) : id(id), outcome(outcome) {}
    CommandSubmissionResult(CommandSubmissionResult&& rhs) = default;
    CommandSubmissionResult(const CommandSubmissionResult& rhs) = delete;
    UUID id;
    Status outcome;
};

/**
 * The class encapsulating all the properties supporting a request to BalancerCommandsSchedulerImpl
 * as it gets created, executed and completed/cancelled.
 */
class RequestData {
public:
    RequestData(UUID id, std::shared_ptr<CommandInfo>&& commandInfo)
        : _id(id),
          _completedOrAborted(false),
          _commandInfo(std::move(commandInfo)),
          _responsePromise{NonNullPromiseTag{}} {
        tassert(8245210, "CommandInfo is be empty", _commandInfo);
    }

    RequestData(RequestData&& rhs)
        : _id(rhs._id),
          _completedOrAborted(rhs._completedOrAborted),
          _commandInfo(std::move(rhs._commandInfo)),
          _responsePromise(std::move(rhs._responsePromise)) {}

    ~RequestData() = default;

    UUID getId() const {
        return _id;
    }

    CommandSubmissionParameters getSubmissionParameters() const {
        return CommandSubmissionParameters(_id, _commandInfo);
    }

    Status applySubmissionResult(CommandSubmissionResult&& submissionResult) {
        tassert(8245211, "Result ID does not match request ID", _id == submissionResult.id);
        if (_completedOrAborted) {
            // A remote response was already received by the time the submission gets processed.
            // Keep the original outcome and continue the workflow.
            return Status::OK();
        }
        const auto& submissionStatus = submissionResult.outcome;
        if (!submissionStatus.isOK()) {
            // cascade the submission failure
            setOutcome(submissionStatus);
        }
        return submissionStatus;
    }

    const CommandInfo& getCommandInfo() const {
        return *_commandInfo;
    }

    const NamespaceString& getNamespace() const {
        return _commandInfo->getNameSpace();
    }

    bool requiresRecoveryCleanupOnCompletion() const {
        return _commandInfo->requiresRecoveryCleanupOnCompletion();
    }

    Future<executor::RemoteCommandResponse> getOutcomeFuture() {
        return _responsePromise.getFuture();
    }

    void setOutcome(const StatusWith<executor::RemoteCommandResponse>& response) {
        _responsePromise.setFrom(response);
        _completedOrAborted = true;
    }

private:
    RequestData& operator=(const RequestData& rhs) = delete;

    RequestData(const RequestData& rhs) = delete;

    const UUID _id;

    bool _completedOrAborted;

    std::shared_ptr<CommandInfo> _commandInfo;

    Promise<executor::RemoteCommandResponse> _responsePromise;
};

/**
 *  Implementation of BalancerCommandsScheduler, relying on the Notification library
 *  for the management of deferred response to remote commands.
 */
class BalancerCommandsSchedulerImpl : public BalancerCommandsScheduler {
public:
    BalancerCommandsSchedulerImpl();

    ~BalancerCommandsSchedulerImpl();

    void start(OperationContext* opCtx) override;

    void stop() override;

    void disableBalancerForCollection(OperationContext* opCtx, const NamespaceString& nss) override;

    SemiFuture<void> requestMoveRange(OperationContext* opCtx,
                                      const ShardsvrMoveRange& request,
                                      const WriteConcernOptions& secondaryThrottleWC,
                                      bool issuedByRemoteUser) override;

    SemiFuture<void> requestMergeChunks(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardId& shardId,
                                        const ChunkRange& chunkRange,
                                        const ChunkVersion& version) override;

    SemiFuture<DataSizeResponse> requestDataSize(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const ShardId& shardId,
                                                 const ChunkRange& chunkRange,
                                                 const ShardVersion& version,
                                                 const KeyPattern& keyPattern,
                                                 bool estimatedValue,
                                                 int64_t maxSize) override;

    SemiFuture<NumMergedChunks> requestMergeAllChunksOnShard(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const ShardId& shardId) override;

    SemiFuture<void> requestMoveCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const ShardId& toShardId,
                                           const ShardId& dbPrimaryShardId,
                                           const DatabaseVersion& dbVersion) override;

private:
    enum class SchedulerState { Recovering, Running, Stopping, Stopped };

    std::unique_ptr<executor::ScopedTaskExecutor> _executor{nullptr};

    // Protects the in-memory state of the Scheduler
    // (_state, _requests, _unsubmittedRequestIds, _recentlyCompletedRequests).
    Mutex _mutex = MONGO_MAKE_LATCH("BalancerCommandsSchedulerImpl::_mutex");

    SchedulerState _state{SchedulerState::Stopped};

    stdx::condition_variable _stateUpdatedCV;

    stdx::thread _workerThreadHandle;

    /**
     * List of all unsubmitted + submitted + completed, but not cleaned up yet requests managed by
     * BalancerCommandsSchedulerImpl, organized by ID.
     */
    stdx::unordered_map<UUID, RequestData, UUID::Hash> _requests;

    /**
     * List of request IDs that have not been yet submitted for remote execution.
     */
    std::vector<UUID> _unsubmittedRequestIds;

    /**
     * List of completed/cancelled requests IDs that may still hold synchronisation resources or
     * persisted state that the scheduler needs to release/clean up.
     */
    std::vector<UUID> _recentlyCompletedRequestIds;

    /*
     * Counter of oustanding requests that were interrupted by a prior step-down/crash event,
     * and that the scheduler is currently submitting as part of its initial recovery phase.
     */
    size_t _numRequestsToRecover{0};

    Future<executor::RemoteCommandResponse> _buildAndEnqueueNewRequest(
        OperationContext* opCtx, std::shared_ptr<CommandInfo>&& commandInfo);

    void _enqueueRequest(WithLock, RequestData&& request);

    CommandSubmissionResult _submit(OperationContext* opCtx,
                                    const CommandSubmissionParameters& data);

    void _applySubmissionResult(WithLock, CommandSubmissionResult&& submissionResult);

    void _applyCommandResponse(UUID requestId, const executor::RemoteCommandResponse& response);

    void _workerThread();
};

}  // namespace mongo
