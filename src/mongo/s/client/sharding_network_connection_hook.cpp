/**
*    Copyright (C) 2015 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/s/client/sharding_network_connection_hook.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

Status ShardingNetworkConnectionHook::validateHost(
    const HostAndPort& remoteHost, const executor::RemoteCommandResponse& isMasterReply) {
    return validateHostImpl(remoteHost, isMasterReply);
}

Status ShardingNetworkConnectionHook::validateHostImpl(
    const HostAndPort& remoteHost, const executor::RemoteCommandResponse& isMasterReply) {
    long long configServerModeNumber;
    Status status =
        bsonExtractIntegerField(isMasterReply.data, "configsvr", &configServerModeNumber);

    if (status == ErrorCodes::NoSuchKey) {
        // This isn't a config server we're talking to.
        return Status::OK();
    } else if (!status.isOK()) {
        return status;
    }

    BSONElement setName = isMasterReply.data["setName"];
    return grid.catalogManager()->scheduleReplaceCatalogManagerIfNeeded(
        configServerModeNumber == 0 ? CatalogManager::ConfigServerMode::SCCC
                                    : CatalogManager::ConfigServerMode::CSRS,
        setName.type() == String ? setName.valueStringData() : StringData(),
        remoteHost);
}

StatusWith<boost::optional<executor::RemoteCommandRequest>>
ShardingNetworkConnectionHook::makeRequest(const HostAndPort& remoteHost) {
    auto shard = grid.shardRegistry()->getShardNoReload(remoteHost.toString());
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "No shard found for host: " << remoteHost.toString());
    }
    if (shard->isConfig()) {
        // No need to initialize sharding metadata if talking to a config server
        return {boost::none};
    }

    SetShardVersionRequest ssv = SetShardVersionRequest::makeForInitNoPersist(
        grid.shardRegistry()->getConfigServerConnectionString(),
        shard->getId(),
        shard->getConnString());
    executor::RemoteCommandRequest request;
    request.dbname = "admin";
    request.target = remoteHost;
    request.timeout = stdx::chrono::seconds{30};
    request.cmdObj = ssv.toBSON();

    return {request};
}

Status ShardingNetworkConnectionHook::handleReply(const HostAndPort& remoteHost,
                                                  executor::RemoteCommandResponse&& response) {
    return getStatusFromCommandResult(response.data);
}
}  // namespace mongo
