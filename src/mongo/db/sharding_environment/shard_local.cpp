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


#include "mongo/db/sharding_environment/shard_local.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

const ConnectionString ShardLocal::kLocalConnectionString = ConnectionString::forLocal();

ShardLocal::ShardLocal(const ShardId& id) : Shard(id) {
    // Currently ShardLocal only works for config servers. If we ever start using ShardLocal on
    // shards we'll need to consider how to handle shards.
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
}

const ConnectionString& ShardLocal::getConnString() const {
    return kLocalConnectionString;
}

std::shared_ptr<RemoteCommandTargeter> ShardLocal::getTargeter() const {
    MONGO_UNREACHABLE;
};

void ShardLocal::updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) {
    MONGO_UNREACHABLE;
}

std::string ShardLocal::toString() const {
    return getId().toString() + ":<local>";
}

bool ShardLocal::isRetriableError(ErrorCodes::Error code,
                                  std::span<const std::string> errorLabels,
                                  RetryPolicy options) const {
    return localIsRetriableError(code, errorLabels, options);
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(OperationContext* opCtx,
                                                           const ReadPreferenceSetting& unused,
                                                           const DatabaseName& dbName,
                                                           Milliseconds maxTimeMSOverrideUnused,
                                                           const BSONObj& cmdObj) {
    return _rsLocalClient.runCommandOnce(opCtx, dbName, cmdObj);
}

RetryStrategy::Result<Shard::QueryResponse> ShardLocal::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

RetryStrategy::Result<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {
    return _rsLocalClient.queryOnce(
        opCtx, readPref, readConcernLevel, nss, query, sort, limit, hint);
}

void ShardLocal::runFireAndForgetCommand(OperationContext* opCtx,
                                         const ReadPreferenceSetting& readPref,
                                         const DatabaseName& dbName,
                                         const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

RetryStrategy::Result<std::monostate> ShardLocal::_runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    // TODO: SERVER-104141 return the result of runAggregation directly.
    auto status = _rsLocalClient.runAggregation(opCtx, aggRequest, callback);
    if (status.isOK()) {
        return std::monostate{};
    }

    return status;
}

BatchedCommandResponse ShardLocal::runBatchWriteCommand(OperationContext* opCtx,
                                                        const Milliseconds maxTimeMS,
                                                        const BatchedCommandRequest& batchRequest,
                                                        const WriteConcernOptions& writeConcern,
                                                        RetryPolicy retryPolicy) {
    // A request dispatched through a local client is served within the same thread that submits it
    // (so that the opCtx needs to be used as the vehicle to pass the WC to the ServiceEntryPoint).
    const auto originalWC = opCtx->getWriteConcern();
    ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
    opCtx->setWriteConcern(writeConcern);

    const DatabaseName dbName = batchRequest.getNS().dbName();
    const BSONObj cmdObj = [&] {
        BSONObjBuilder cmdObjBuilder;
        batchRequest.serialize(&cmdObjBuilder);
        return cmdObjBuilder.obj();
    }();

    return _submitBatchWriteCommand(opCtx, cmdObj, dbName, maxTimeMS, retryPolicy);
}
}  // namespace mongo
