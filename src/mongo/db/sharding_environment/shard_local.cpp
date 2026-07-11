// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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

ShardLocal::ShardLocal(const ShardHandle& handle,
                       std::shared_ptr<ShardSharedStateCache::State> sharedState)
    : Shard(handle, std::move(sharedState)) {
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

bool ShardLocal::isRetriableError(const Status& status,
                                  std::span<const std::string> errorLabels,
                                  RetryPolicy options) const {
    return localIsRetriableError(status, errorLabels, options);
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& unused,
    const TargetingMetadata& targetingMetadata,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverrideUnused,
    const BSONObj& cmdObj) {
    return _rsLocalClient.runCommandOnce(opCtx, dbName, cmdObj);
}

RetryStrategy::Result<Shard::QueryResponse> ShardLocal::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

RetryStrategy::Result<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const repl::ReadConcernArgs& readConcern,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint,
    const boost::optional<BSONObj>& projection) {
    return _rsLocalClient.queryOnce(
        opCtx, readPref, readConcern, nss, query, sort, limit, hint, projection);
}

void ShardLocal::runFireAndForgetCommand(OperationContext* opCtx,
                                         const ReadPreferenceSetting& readPref,
                                         const DatabaseName& dbName,
                                         const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

RetryStrategy::Result<std::monostate> ShardLocal::_runAggregation(
    OperationContext* opCtx,
    const TargetingMetadata& targetingMetadata,
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
