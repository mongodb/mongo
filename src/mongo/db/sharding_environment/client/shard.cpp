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

#include "mongo/db/sharding_environment/client/shard.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <limits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

auto makeRetryCriteriaForShard(const Shard& shard, Shard::RetryPolicy retryPolicy) {
    return [retryPolicy, shard = &shard](Status s, std::span<const std::string> errorLabels) {
        return shard->isRetriableError(s.code(), errorLabels, retryPolicy);
    };
}

auto backoffFromMaxRetryAttempts(std::int32_t maxRetryAttempts) {
    const auto parameters = DefaultRetryStrategy::getRetryParametersFromServerParameters();
    return DefaultRetryStrategy::RetryParameters{
        .maxRetryAttempts = maxRetryAttempts,
        .baseBackoff = parameters.baseBackoff,
        .maxBackoff = parameters.maxBackoff,
    };
}

struct RunCommandResult {
    StatusWith<Shard::CommandResponse> swResponse;

    // The effective status represents the combined status of three possible failure sources:
    // - A local error,
    // - An error code in the command response,
    // - A write concern error code.
    Status effectiveStatus;
};

template <std::invocable<const TargetingMetadata&> F>
requires(std::same_as<std::invoke_result_t<F, const TargetingMetadata&>, RunCommandResult>)
StatusWith<Shard::CommandResponse> runCommandWithRetryStrategy(Interruptible* interruptible,
                                                               RetryStrategy& strategy,
                                                               F runOperation) {
    boost::optional<RunCommandResult> lastResult;

    auto effectiveStatus = runWithRetryStrategy(
        interruptible, strategy, [&](const TargetingMetadata& targetingMetadata) {
            lastResult.reset();

            auto [swResponse, status] = runOperation(targetingMetadata);

            lastResult.emplace(swResponse, status);

            if (!status.isOK()) {
                return RetryStrategy::Result<Shard::CommandResponse>{
                    status,
                    Shard::CommandResponse::getErrorLabels(swResponse),
                    swResponse.isOK() ? swResponse.getValue().hostAndPort : boost::none};
            }

            const auto& response = swResponse.getValue();
            return RetryStrategy::Result{response, response.hostAndPort};
        });

    bool exceptionThrownDuringRetry = !lastResult || effectiveStatus != lastResult->effectiveStatus;
    return exceptionThrownDuringRetry ? effectiveStatus : StatusWith{lastResult->swResponse};
}

}  // namespace

Shard::RetryStrategy::RetryStrategy(const Shard& shard, Shard::RetryPolicy retryPolicy)
    : RetryStrategy{shard,
                    retryPolicy,
                    DefaultRetryStrategy::getRetryParametersFromServerParameters(),
                    *shard._sharedState} {}

Shard::RetryStrategy::RetryStrategy(const Shard& shard,
                                    Shard::RetryPolicy retryPolicy,
                                    std::int32_t maxRetryAttempts)
    : RetryStrategy{
          shard, retryPolicy, backoffFromMaxRetryAttempts(maxRetryAttempts), *shard._sharedState} {}

Shard::RetryStrategy::RetryStrategy(const Shard& shard,
                                    Shard::RetryPolicy retryPolicy,
                                    AdaptiveRetryStrategy::RetryParameters parameters,
                                    ShardSharedStateCache::State& sharedState)
    : _underlyingStrategy{sharedState.retryBudget,
                          makeRetryCriteriaForShard(shard, retryPolicy),
                          parameters},
      _stats{&sharedState.stats} {}

bool Shard::RetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& target,
    std::span<const std::string> errorLabels) {
    const bool willRetry =
        _underlyingStrategy.recordFailureAndEvaluateShouldRetry(s, target, errorLabels);

    _recordOperationAttempted();

    if (containsSystemOverloadedLabels(errorLabels)) {
        _stats->numOverloadErrorsReceived.addAndFetch(1);

        if (willRetry) {
            _stats->numRetriesDueToOverloadAttempted.addAndFetch(1);
        }

        if (!_previousAttemptOverloaded) {
            _stats->numOperationsRetriedAtLeastOnceDueToOverload.addAndFetch(1);
        }

        _previousAttemptOverloaded = true;
    } else {
        _recordOperationNotOverloaded();
    }

    return willRetry;
}

void Shard::RetryStrategy::recordSuccess(const boost::optional<HostAndPort>& target) {
    _underlyingStrategy.recordSuccess(target);

    _recordOperationAttempted();
    _recordOperationNotOverloaded();
}

void Shard::RetryStrategy::recordBackoff(Milliseconds backoff) {
    _underlyingStrategy.recordBackoff(backoff);
    _stats->totalBackoffTimeMillis.addAndFetch(backoff.count());
}

void Shard::RetryStrategy::_recordOperationAttempted() {
    if (!_recordedAttempted) {
        _recordedAttempted = true;
        _stats->numOperationsAttempted.addAndFetch(1);
    }
}

void Shard::RetryStrategy::_recordOperationNotOverloaded() {
    if (_previousAttemptOverloaded) {
        _stats->numOperationsRetriedAtLeastOnceDueToOverloadAndSucceeded.addAndFetch(1);
        _previousAttemptOverloaded = false;
    }
}

Status Shard::CommandResponse::getEffectiveStatus(
    const StatusWith<Shard::CommandResponse>& swResponse) {
    // Check if the request even reached the shard.
    if (!swResponse.isOK()) {
        return swResponse.getStatus();
    }

    auto& response = swResponse.getValue();

    // If the request reached the shard, check if the command failed.
    if (!response.commandStatus.isOK()) {
        return response.commandStatus;
    }

    // Finally check if the write concern failed.
    if (!response.writeConcernStatus.isOK()) {
        return response.writeConcernStatus;
    }

    return Status::OK();
}

std::vector<std::string> Shard::CommandResponse::getErrorLabels(
    const StatusWith<Shard::CommandResponse>& swResponse) {
    // Check if the request even reached the shard.
    if (!swResponse.isOK()) {
        return {};
    }

    auto& response = swResponse.getValue();

    if (response.commandStatus.isOK()) {
        return {};
    }

    if (BSONElement errorLabelsElement = response.response[kErrorLabelsFieldName];
        !errorLabelsElement.eoo()) {
        auto errorLabelsArray = errorLabelsElement.Array();

        std::vector<std::string> errorLabels{};
        errorLabels.resize(errorLabelsArray.size());

        std::ranges::transform(errorLabelsArray, errorLabels.begin(), [](const BSONElement& data) {
            return data.String();
        });

        return errorLabels;
    }

    return {};
}

Status Shard::CommandResponse::processBatchWriteResponse(
    StatusWith<Shard::CommandResponse> swResponse, BatchedCommandResponse* batchResponse) {
    auto status = getEffectiveStatus(swResponse);
    if (status.isOK()) {
        std::string errmsg;
        if (!batchResponse->parseBSON(swResponse.getValue().response, &errmsg)) {
            status = Status(ErrorCodes::FailedToParse,
                            str::stream() << "Failed to parse write response: " << errmsg);
        } else {
            status = batchResponse->toStatus();
        }
    }

    if (!status.isOK()) {
        batchResponse->clear();
        batchResponse->setStatus(status);
    }

    return status;
}

bool Shard::shouldErrorBePropagated(ErrorCodes::Error code) {
    return !isMongosRetriableError(code) && (code != ErrorCodes::NetworkInterfaceExceededTimeLimit);
}

Shard::Shard(const ShardId& id, std::shared_ptr<ShardSharedStateCache::State> sharedState)
    : _id(id), _sharedState{std::move(sharedState)} {}

std::shared_ptr<ShardSharedStateCache::State> Shard::getSharedState() const {
    return _sharedState;
}

AdaptiveRetryStrategy::RetryBudget& Shard::getRetryBudget_forTest() const {
    return _sharedState->retryBudget;
}

bool Shard::isConfig() const {
    return _id == ShardId::kConfigServerId;
}

bool Shard::localIsRetriableError(ErrorCodes::Error code,
                                  std::span<const std::string> errorLabels,
                                  RetryPolicy options) {
    switch (options) {
        case Shard::RetryPolicy::kNoRetry: {
            return false;
        } break;

        case Shard::RetryPolicy::kIdempotent: {
            return code == ErrorCodes::WriteConcernTimeout || containsRetryableLabels(errorLabels);
        } break;

        case Shard::RetryPolicy::kIdempotentOrCursorInvalidated: {
            return localIsRetriableError(code, errorLabels, Shard::RetryPolicy::kIdempotent) ||
                ErrorCodes::isCursorInvalidatedError(code);
        } break;

        case Shard::RetryPolicy::kNotIdempotent: {
            return false;
        } break;
    }

    MONGO_UNREACHABLE;
}

bool Shard::remoteIsRetriableError(ErrorCodes::Error code,
                                   std::span<const std::string> errorLabels,
                                   RetryPolicy options) {
    if (gInternalProhibitShardOperationRetry.loadRelaxed()) {
        return false;
    }

    switch (options) {
        case RetryPolicy::kNoRetry: {
            return false;
        } break;

        case RetryPolicy::kIdempotent: {
            return isMongosRetriableError(code) || containsRetryableLabels(errorLabels);
        } break;

        case RetryPolicy::kIdempotentOrCursorInvalidated: {
            return remoteIsRetriableError(code, errorLabels, Shard::RetryPolicy::kIdempotent) ||
                ErrorCodes::isCursorInvalidatedError(code);
        } break;

        case RetryPolicy::kNotIdempotent: {
            return ErrorCodes::isNotPrimaryError(code);
        } break;
    }

    MONGO_UNREACHABLE;
}

StatusWith<Shard::CommandResponse> Shard::runCommandWithIndefiniteRetries(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    RetryPolicy retryPolicy) {
    return runCommandWithIndefiniteRetries(
        opCtx, readPref, dbName, cmdObj, Milliseconds::max(), retryPolicy);
}

StatusWith<Shard::CommandResponse> Shard::runCommandWithIndefiniteRetries(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    Milliseconds maxTimeMSOverride,
    RetryPolicy retryPolicy) {
    return _runCommandImpl(opCtx,
                           readPref,
                           dbName,
                           cmdObj,
                           maxTimeMSOverride,
                           retryPolicy,
                           std::numeric_limits<std::int32_t>::max());
}

StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* opCtx,
                                                     const ReadPreferenceSetting& readPref,
                                                     const DatabaseName& dbName,
                                                     const BSONObj& cmdObj,
                                                     RetryPolicy retryPolicy) {
    return runCommand(opCtx, readPref, dbName, cmdObj, Milliseconds::max(), retryPolicy);
}

StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* opCtx,
                                                     const ReadPreferenceSetting& readPref,
                                                     const DatabaseName& dbName,
                                                     const BSONObj& cmdObj,
                                                     Milliseconds maxTimeMSOverride,
                                                     RetryPolicy retryPolicy) {
    return _runCommandImpl(opCtx,
                           readPref,
                           dbName,
                           cmdObj,
                           maxTimeMSOverride,
                           retryPolicy,
                           gDefaultClientMaxRetryAttempts.load());
}

StatusWith<Shard::CommandResponse> Shard::_runCommandImpl(OperationContext* opCtx,
                                                          const ReadPreferenceSetting& readPref,
                                                          const DatabaseName& dbName,
                                                          const BSONObj& cmdObj,
                                                          Milliseconds maxTimeMSOverride,
                                                          RetryPolicy retryPolicy,
                                                          std::int32_t maxRetryAttempt) {
    auto retryStrategy = RetryStrategyWithFailureRetryHook{
        RetryStrategy{*this, retryPolicy, maxRetryAttempt}, [&](Status status) {
            LOGV2(22720,
                  "Command failed with a retryable error and will be retried",
                  "command"_attr = redact(cmdObj),
                  "error"_attr = redact(status));
        }};
    return runCommandWithRetryStrategy(
        opCtx, retryStrategy, [&](const TargetingMetadata& targetingMetadata) {
            auto swResponse =
                _runCommand(opCtx, readPref, targetingMetadata, dbName, maxTimeMSOverride, cmdObj);
            auto effectiveStatus = CommandResponse::getEffectiveStatus(swResponse);
            return RunCommandResult{swResponse, effectiveStatus};
        });
}

StatusWith<Shard::QueryResponse> Shard::runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    Milliseconds maxTimeMSOverride) {
    RetryStrategy retryStrategy{*this, RetryPolicy::kIdempotent};

    return runWithRetryStrategy(
        opCtx, retryStrategy, [&](const TargetingMetadata& targetingMetadata) {
            return _runExhaustiveCursorCommand(
                opCtx, readPref, targetingMetadata, dbName, maxTimeMSOverride, cmdObj);
        });
}

StatusWith<Shard::QueryResponse> Shard::exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    const boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {
    // Do not allow exhaustive finds to be run against regular shards.
    invariant(isConfig());

    RetryStrategy retryStrategy{*this, RetryPolicy::kIdempotent};
    return runWithRetryStrategy(
        opCtx, retryStrategy, [&](const TargetingMetadata& targetingMetadata) {
            return _exhaustiveFindOnConfig(opCtx,
                                           readPref,
                                           targetingMetadata,
                                           readConcernLevel,
                                           nss,
                                           query,
                                           sort,
                                           limit,
                                           hint);
        });
}

Status Shard::runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    RetryStrategy retryStrategy{*this, RetryPolicy::kNoRetry};

    auto status =
        runWithRetryStrategy(opCtx, retryStrategy, [&](const TargetingMetadata& targetingMetadata) {
            return _runAggregation(opCtx, targetingMetadata, aggRequest, callback);
        });

    return status.getStatus();
}

BatchedCommandResponse Shard::_submitBatchWriteCommand(OperationContext* opCtx,
                                                       const BSONObj& serialisedBatchRequest,
                                                       const DatabaseName& dbName,
                                                       Milliseconds maxTimeMS,
                                                       RetryPolicy retryPolicy) {
    auto retryStrategy = RetryStrategyWithFailureRetryHook{
        RetryStrategy{*this, retryPolicy}, [&](Status status) {
            LOGV2_DEBUG(22721,
                        2,
                        "Batch write command failed with retryable error and will be retried",
                        "shardId"_attr = getId(),
                        "error"_attr = redact(status));
        }};
    BatchedCommandResponse batchResponse;
    Status lastWriteStatus = Status::OK();

    auto status =
        runCommandWithRetryStrategy(
            opCtx,
            retryStrategy,
            [&](const TargetingMetadata& targetingMetadata) {
                auto swResponse = _runCommand(opCtx,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              targetingMetadata,
                                              dbName,
                                              maxTimeMS,
                                              serialisedBatchRequest);
                auto writeStatus =
                    CommandResponse::processBatchWriteResponse(swResponse, &batchResponse);
                lastWriteStatus = writeStatus;
                return RunCommandResult{
                    .swResponse = swResponse,
                    .effectiveStatus = writeStatus,
                };
            })
            .getStatus();

    // The last status will be different than the status of runCommandWithRetryStrategy if the
    // operation was interrupted.
    if (lastWriteStatus != status) {
        uassertStatusOK(status);
    }

    return batchResponse;
}

Milliseconds Shard::getConfiguredTimeoutForOperationOnNamespace(const NamespaceString& nss) {
    if (nss == NamespaceString::kConfigsvrChunksNamespace) {
        return Milliseconds(gFindChunksOnConfigTimeoutMS.load());
    }
    if (nss == NamespaceString::kConfigsvrShardsNamespace) {
        return Milliseconds(gFindShardsOnConfigTimeoutMS.load());
    }

    return Milliseconds(defaultConfigCommandTimeoutMS.load());
}


StatusWith<std::vector<BSONObj>> Shard::runAggregationWithResult(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    Shard::RetryPolicy retryPolicy) {
    std::vector<BSONObj> aggResult;
    auto callback = [&aggResult](const std::vector<BSONObj>& batch,
                                 const boost::optional<BSONObj>& postBatchResumeToken) {
        aggResult.insert(aggResult.end(),
                         std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
        return true;
    };

    RetryStrategyWithFailureRetryHook retryStrategy{RetryStrategy{*this, retryPolicy},
                                                    [&](Status s) {
                                                        aggResult.clear();
                                                    }};

    auto status =
        runWithRetryStrategy(opCtx, retryStrategy, [&](const TargetingMetadata& targetingMetadata) {
            return _runAggregation(opCtx, targetingMetadata, aggRequest, callback);
        });

    if (status.isOK()) {
        return aggResult;
    }

    return status.getStatus();
}

}  // namespace mongo
