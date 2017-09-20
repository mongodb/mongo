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

#include <boost/none_t.hpp>

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_local.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

ShardLocal::ShardLocal(const ShardId& id) : Shard(id), _rsLocalClient() {
    // Currently ShardLocal only works for config servers. If we ever start using ShardLocal on
    // shards we'll need to consider how to handle shards.
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
}

const ConnectionString ShardLocal::getConnString() const {
    return repl::getGlobalReplicationCoordinator()->getConfig().getConnectionString();
}

std::shared_ptr<RemoteCommandTargeter> ShardLocal::getTargeter() const {
    MONGO_UNREACHABLE;
};

const ConnectionString ShardLocal::originalConnString() const {
    // Return the local connection string here as this method is only used for updating the
    // ShardRegistry and we don't need a mapping from hosts in the replica set config to the shard
    // for local shards.
    return ConnectionString::forLocal();
}

void ShardLocal::updateReplSetMonitor(const HostAndPort& remoteHost,
                                      const Status& remoteCommandStatus) {
    MONGO_UNREACHABLE;
}

std::string ShardLocal::toString() const {
    return getId().toString() + ":<local>";
}

bool ShardLocal::isRetriableError(ErrorCodes::Error code, RetryPolicy options) {
    if (options == RetryPolicy::kNoRetry) {
        return false;
    }

    if (options == RetryPolicy::kIdempotent) {
        return code == ErrorCodes::WriteConcernFailed;
    } else {
        invariant(options == RetryPolicy::kNotIdempotent);
        return false;
    }
}

StatusWith<Shard::CommandResponse> ShardLocal::_runCommand(OperationContext* opCtx,
                                                           const ReadPreferenceSetting& unused,
                                                           const std::string& dbName,
                                                           Milliseconds maxTimeMSOverrideUnused,
                                                           const BSONObj& cmdObj) {
    return _rsLocalClient.runCommandOnce(opCtx, dbName, cmdObj);
}

StatusWith<Shard::QueryResponse> ShardLocal::_exhaustiveFindOnConfig(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    return _rsLocalClient.queryOnce(opCtx, readPref, readConcernLevel, nss, query, sort, limit);
}

Status ShardLocal::createIndexOnConfig(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const BSONObj& keys,
                                       bool unique) {
    invariant(ns.db() == "config" || ns.db() == "admin");

    try {
        DBDirectClient client(opCtx);
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);
        client.createIndex(ns.toString(), index);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
