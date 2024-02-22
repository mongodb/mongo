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


#include <boost/optional/optional.hpp>

#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/shard_local.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"

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

bool ShardLocal::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    return localIsRetriableError(code, options);
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(OperationContext* opCtx,
                                                           const ReadPreferenceSetting& unused,
                                                           const DatabaseName& dbName,
                                                           Milliseconds maxTimeMSOverrideUnused,
                                                           const BSONObj& cmdObj) {
    return _rsLocalClient.runCommandOnce(opCtx, dbName, cmdObj);
}

StatusWith<Shard::QueryResponse> ShardLocal::_runExhaustiveCursorCommand(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const DatabaseName& dbName,
    Milliseconds maxTimeMSOverride,
    const BSONObj& cmdObj) {
    MONGO_UNREACHABLE;
}

StatusWith<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
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

Status ShardLocal::runAggregation(
    OperationContext* opCtx,
    const AggregateCommandRequest& aggRequest,
    std::function<bool(const std::vector<BSONObj>& batch,
                       const boost::optional<BSONObj>& postBatchResumeToken)> callback) {
    return _rsLocalClient.runAggregation(opCtx, aggRequest, callback);
}

}  // namespace mongo
