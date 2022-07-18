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


#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const int kOnErrorNumRetries = 3;

}  // namespace

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

const Milliseconds Shard::kDefaultConfigCommandTimeout = Seconds{30};

bool Shard::shouldErrorBePropagated(ErrorCodes::Error code) {
    return !isMongosRetriableError(code) && (code != ErrorCodes::NetworkInterfaceExceededTimeLimit);
}

Shard::Shard(const ShardId& id) : _id(id) {}

bool Shard::isConfig() const {
    return _id == ShardId::kConfigServerId;
}

StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* opCtx,
                                                     const ReadPreferenceSetting& readPref,
                                                     const std::string& dbName,
                                                     const BSONObj& cmdObj,
                                                     RetryPolicy retryPolicy) {
    return runCommand(opCtx, readPref, dbName, cmdObj, Milliseconds::max(), retryPolicy);
}

StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* opCtx,
                                                     const ReadPreferenceSetting& readPref,
                                                     const std::string& dbName,
                                                     const BSONObj& cmdObj,
                                                     Milliseconds maxTimeMSOverride,
                                                     RetryPolicy retryPolicy) {
    while (true) {
        auto interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }

        auto swResponse = _runCommand(opCtx, readPref, dbName, maxTimeMSOverride, cmdObj);
        auto status = CommandResponse::getEffectiveStatus(swResponse);
        if (isRetriableError(status.code(), retryPolicy)) {
            LOGV2_DEBUG(22719,
                        2,
                        "Command {command} failed with retryable error and will be retried. Caused "
                        "by {error}",
                        "Command failed with retryable error and will be retried",
                        "command"_attr = redact(cmdObj),
                        "error"_attr = redact(status));
            continue;
        }

        return swResponse;
    }
    MONGO_UNREACHABLE;
}

StatusWith<Shard::CommandResponse> Shard::runCommandWithFixedRetryAttempts(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj,
    RetryPolicy retryPolicy) {
    return runCommandWithFixedRetryAttempts(
        opCtx, readPref, dbName, cmdObj, Milliseconds::max(), retryPolicy);
}

StatusWith<Shard::CommandResponse> Shard::runCommandWithFixedRetryAttempts(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj,
    Milliseconds maxTimeMSOverride,
    RetryPolicy retryPolicy) {
    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        auto interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }

        auto swResponse = _runCommand(opCtx, readPref, dbName, maxTimeMSOverride, cmdObj);
        auto status = CommandResponse::getEffectiveStatus(swResponse);
        if (retry < kOnErrorNumRetries && isRetriableError(status.code(), retryPolicy)) {
            LOGV2(22720,
                  "Command failed with a retryable error and will be retried",
                  "command"_attr = redact(cmdObj),
                  "error"_attr = redact(status));
            continue;
        }

        return swResponse;
    }
    MONGO_UNREACHABLE;
}

StatusWith<Shard::QueryResponse> Shard::runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj,
    Milliseconds maxTimeMSOverride) {
    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        auto result =
            _runExhaustiveCursorCommand(opCtx, readPref, dbName, maxTimeMSOverride, cmdObj);

        if (retry < kOnErrorNumRetries &&
            isRetriableError(result.getStatus().code(), RetryPolicy::kIdempotent)) {
            continue;
        }
        return result;
    }
    MONGO_UNREACHABLE;
}

BatchedCommandResponse Shard::runBatchWriteCommand(OperationContext* opCtx,
                                                   const Milliseconds maxTimeMS,
                                                   const BatchedCommandRequest& batchRequest,
                                                   RetryPolicy retryPolicy) {
    const StringData dbname = batchRequest.getNS().db();
    const BSONObj cmdObj = batchRequest.toBSON();

    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        // Note: write commands can only be issued against a primary.
        auto swResponse = _runCommand(
            opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, dbname, maxTimeMS, cmdObj);

        BatchedCommandResponse batchResponse;
        auto writeStatus = CommandResponse::processBatchWriteResponse(swResponse, &batchResponse);
        if (retry < kOnErrorNumRetries && isRetriableError(writeStatus.code(), retryPolicy)) {
            LOGV2_DEBUG(22721,
                        2,
                        "Batch write command to shard {shardId} failed with retryable error "
                        "and will be retried. Caused by {error}",
                        "Batch write command failed with retryable error and will be retried",
                        "shardId"_attr = getId(),
                        "error"_attr = redact(writeStatus));
            continue;
        }

        return batchResponse;
    }
    MONGO_UNREACHABLE;
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

    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        auto result = _exhaustiveFindOnConfig(
            opCtx, readPref, readConcernLevel, nss, query, sort, limit, hint);

        if (retry < kOnErrorNumRetries &&
            isRetriableError(result.getStatus().code(), RetryPolicy::kIdempotent)) {
            continue;
        }

        return result;
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
