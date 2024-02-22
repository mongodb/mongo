/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/s/client/config_shard_wrapper.h"

#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/vector_clock.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {

ConfigShardWrapper::ConfigShardWrapper(std::shared_ptr<Shard> configShard)
    : Shard(configShard->getId()), _configShard(std::move(configShard)) {
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

bool ConfigShardWrapper::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    return _configShard->isRetriableError(code, options);
}

void ConfigShardWrapper::runFireAndForgetCommand(OperationContext* opCtx,
                                                 const ReadPreferenceSetting& readPref,
                                                 const DatabaseName& dbName,
                                                 const BSONObj& cmdObj) {
    const auto readPrefWithConfigTime = _attachConfigTimeToMinClusterTime(opCtx, readPref);
    _configShard->runFireAndForgetCommand(opCtx, readPrefWithConfigTime, dbName, cmdObj);
}

Status ConfigShardWrapper::runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    return _configShard->runAggregation(opCtx, aggRequest, std::move(callback));
}

StatusWith<Shard::CommandResponse> ConfigShardWrapper::_runCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverrideUnused,
    const BSONObj& cmdObj) {
    const auto readPrefWithConfigTime = _attachConfigTimeToMinClusterTime(opCtx, readPref);
    return _configShard->_runCommand(
        opCtx, readPrefWithConfigTime, dbName, maxTimeMSOverrideUnused, cmdObj);
}

StatusWith<Shard::QueryResponse> ConfigShardWrapper::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    return _configShard->_runExhaustiveCursorCommand(
        opCtx, readPref, dbName, maxTimeMSOverride, cmdObj);
}

StatusWith<Shard::QueryResponse> ConfigShardWrapper::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit,
    const boost::optional<BSONObj>& hint) {
    return _configShard->_exhaustiveFindOnConfig(
        opCtx, readPref, readConcernLevel, nss, query, sort, limit, hint);
}

ReadPreferenceSetting ConfigShardWrapper::_attachConfigTimeToMinClusterTime(
    OperationContext* opCtx, const ReadPreferenceSetting& readPref) {
    const auto vcTime = VectorClock::get(opCtx)->getTime();
    ReadPreferenceSetting readPrefToReturn{readPref};
    readPrefToReturn.minClusterTime = vcTime.configTime().asTimestamp();
    return readPrefToReturn;
}
}  // namespace mongo
