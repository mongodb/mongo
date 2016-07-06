/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

const int kOnErrorNumRetries = 3;

Status _getEffectiveCommandStatus(StatusWith<Shard::CommandResponse> cmdResponse) {
    // Make sure the command even received a valid response
    if (!cmdResponse.isOK()) {
        return cmdResponse.getStatus();
    }

    // If the request reached the shard, check if the command itself failed.
    if (!cmdResponse.getValue().commandStatus.isOK()) {
        return cmdResponse.getValue().commandStatus;
    }

    // Finally check if the write concern failed
    if (!cmdResponse.getValue().writeConcernStatus.isOK()) {
        return cmdResponse.getValue().writeConcernStatus;
    }

    return Status::OK();
}

}  // namespace

Status Shard::CommandResponse::processBatchWriteResponse(
    StatusWith<Shard::CommandResponse> response, BatchedCommandResponse* batchResponse) {
    auto status = _getEffectiveCommandStatus(response);
    if (status.isOK()) {
        string errmsg;
        if (!batchResponse->parseBSON(response.getValue().response, &errmsg)) {
            status = Status(ErrorCodes::FailedToParse,
                            str::stream() << "Failed to parse write response: " << errmsg);
        } else {
            status = batchResponse->toStatus();
        }
    }

    if (!status.isOK()) {
        batchResponse->clear();
        batchResponse->setErrCode(status.code());
        batchResponse->setErrMessage(status.reason());
        batchResponse->setOk(false);
    }

    return status;
}

Shard::Shard(const ShardId& id) : _id(id) {}

const ShardId Shard::getId() const {
    return _id;
}

bool Shard::isConfig() const {
    return _id == "config";
}

StatusWith<Shard::CommandResponse> Shard::runCommand(OperationContext* txn,
                                                     const ReadPreferenceSetting& readPref,
                                                     const std::string& dbName,
                                                     const BSONObj& cmdObj,
                                                     RetryPolicy retryPolicy) {
    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        auto swCmdResponse = _runCommand(txn, readPref, dbName, cmdObj).commandResponse;
        auto commandStatus = _getEffectiveCommandStatus(swCmdResponse);

        if (retry < kOnErrorNumRetries && isRetriableError(commandStatus.code(), retryPolicy)) {
            LOG(2) << "Command " << cmdObj << " failed with retriable error and will be retried"
                   << causedBy(commandStatus);
            continue;
        }

        return swCmdResponse;
    }
    MONGO_UNREACHABLE;
}

BatchedCommandResponse Shard::runBatchWriteCommand(OperationContext* txn,
                                                   const BatchedCommandRequest& batchRequest,
                                                   RetryPolicy retryPolicy) {
    const std::string dbname = batchRequest.getNS().db().toString();
    invariant(batchRequest.sizeWriteOps() == 1);

    const BSONObj cmdObj = batchRequest.toBSON();

    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        auto response =
            _runCommand(txn, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, dbname, cmdObj);

        BatchedCommandResponse batchResponse;
        Status writeStatus =
            CommandResponse::processBatchWriteResponse(response.commandResponse, &batchResponse);

        if (!writeStatus.isOK() && response.host) {
            updateReplSetMonitor(response.host.get(), writeStatus);
        }

        if (retry < kOnErrorNumRetries && isRetriableError(writeStatus.code(), retryPolicy)) {
            LOG(2) << "Batch write command failed with retriable error and will be retried"
                   << causedBy(writeStatus);
            continue;
        }

        return batchResponse;
    }
    MONGO_UNREACHABLE;
}

StatusWith<Shard::QueryResponse> Shard::exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    const boost::optional<long long> limit) {
    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        auto result =
            _exhaustiveFindOnConfig(txn, readPref, readConcernLevel, nss, query, sort, limit);

        if (retry < kOnErrorNumRetries &&
            isRetriableError(result.getStatus().code(), RetryPolicy::kIdempotent)) {
            continue;
        }

        return result;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
