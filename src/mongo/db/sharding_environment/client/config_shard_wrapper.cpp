// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/sharding_environment/client/config_shard_wrapper.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

ConfigShardWrapper::ConfigShardWrapper(std::shared_ptr<Shard> configShard)
    : Shard(configShard->getHandle(), configShard->getSharedState()),
      _configShard(std::move(configShard)) {
    invariant(_configShard->isConfig());
}

const ConnectionString& ConfigShardWrapper::getConnString() const {
    return _configShard->getConnString();
}

std::shared_ptr<RemoteCommandTargeter> ConfigShardWrapper::getTargeter() const {
    return _configShard->getTargeter();
};

void ConfigShardWrapper::updateReplSetMonitor(const HostAndPort& remoteHost,
                                              const Status& remoteCommandStatus) {
    return _configShard->updateReplSetMonitor(remoteHost, remoteCommandStatus);
}

std::string ConfigShardWrapper::toString() const {
    return _configShard->toString();
}

bool ConfigShardWrapper::isRetriableError(const Status& status,
                                          std::span<const std::string> errorLabels,
                                          RetryPolicy options) const {
    return _configShard->isRetriableError(status, errorLabels, options);
}

void ConfigShardWrapper::runFireAndForgetCommand(OperationContext* opCtx,
                                                 const ReadPreferenceSetting& readPref,
                                                 const DatabaseName& dbName,
                                                 const BSONObj& cmdObj) {
    const auto readPrefWithConfigTime = _attachConfigTimeToMinClusterTime(opCtx, readPref);
    _configShard->runFireAndForgetCommand(opCtx, readPrefWithConfigTime, dbName, cmdObj);
}

RetryStrategy::Result<std::monostate> ConfigShardWrapper::_runAggregation(
    OperationContext* opCtx,
    const TargetingMetadata& targetingMetadata,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    return _configShard->_runAggregation(opCtx, targetingMetadata, aggRequest, std::move(callback));
}

BatchedCommandResponse ConfigShardWrapper::runBatchWriteCommand(
    OperationContext* opCtx,
    Milliseconds maxTimeMS,
    const BatchedCommandRequest& batchRequest,
    const WriteConcernOptions& writeConcern,
    RetryPolicy retryPolicy) {
    return _configShard->runBatchWriteCommand(
        opCtx, maxTimeMS, batchRequest, writeConcern, retryPolicy);
}


StatusWith<Shard::CommandResponse> ConfigShardWrapper::_runCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverrideUnused,
    const BSONObj& cmdObj) {
    const auto readPrefWithConfigTime = _attachConfigTimeToMinClusterTime(opCtx, readPref);
    return _configShard->_runCommand(
        opCtx, readPrefWithConfigTime, targetingMetadata, dbName, maxTimeMSOverrideUnused, cmdObj);
}

RetryStrategy::Result<Shard::QueryResponse> ConfigShardWrapper::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    return _configShard->_runExhaustiveCursorCommand(
        opCtx, readPref, targetingMetadata, dbName, maxTimeMSOverride, cmdObj);
}

RetryStrategy::Result<Shard::QueryResponse> ConfigShardWrapper::_exhaustiveFindOnConfig(
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
    return _configShard->_exhaustiveFindOnConfig(
        opCtx, readPref, targetingMetadata, readConcern, nss, query, sort, limit, hint, projection);
}

ReadPreferenceSetting ConfigShardWrapper::_attachConfigTimeToMinClusterTime(
    OperationContext* opCtx, const ReadPreferenceSetting& readPref) {
    const auto vcTime = VectorClock::get(opCtx)->getTime();
    ReadPreferenceSetting readPrefToReturn{readPref};
    readPrefToReturn.minClusterTime = vcTime.configTime().asTimestamp();
    return readPrefToReturn;
}
}  // namespace mongo
