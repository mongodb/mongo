/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/s/client/sharding_network_connection_hook.h"

#include "merizo/base/status.h"
#include "merizo/base/status_with.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/db/server_options.h"
#include "merizo/db/wire_version.h"
#include "merizo/executor/remote_command_request.h"
#include "merizo/executor/remote_command_response.h"
#include "merizo/rpc/get_status_from_command_result.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/grid.h"
#include "merizo/s/request_types/set_shard_version_request.h"
#include "merizo/util/merizoutils/str.h"
#include "merizo/util/net/hostandport.h"

namespace merizo {

Status ShardingNetworkConnectionHook::validateHost(
    const HostAndPort& remoteHost,
    const BSONObj&,
    const executor::RemoteCommandResponse& isMasterReply) {
    return validateHostImpl(remoteHost, isMasterReply);
}

Status ShardingNetworkConnectionHook::validateHostImpl(
    const HostAndPort& remoteHost, const executor::RemoteCommandResponse& isMasterReply) {
    auto shard =
        Grid::get(getGlobalServiceContext())->shardRegistry()->getShardForHostNoReload(remoteHost);
    if (!shard) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "No shard found for host: " << remoteHost.toString()};
    }

    long long configServerModeNumber;
    auto status = bsonExtractIntegerField(isMasterReply.data, "configsvr", &configServerModeNumber);
    // TODO SERVER-22320 fix should collapse the switch to only NoSuchKey handling

    switch (status.code()) {
        case ErrorCodes::OK: {
            // The ismaster response indicates remoteHost is a config server.
            if (!shard->isConfig()) {
                return {ErrorCodes::InvalidOptions,
                        str::stream() << "Surprised to discover that " << remoteHost.toString()
                                      << " believes it is a config server"};
            }
            return Status::OK();
        }
        case ErrorCodes::NoSuchKey: {
            // The ismaster response indicates that remoteHost is not a config server, or that
            // the config server is running a version prior to the 3.1 development series.
            if (!shard->isConfig()) {
                return Status::OK();
            }

            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Surprised to discover that " << remoteHost.toString()
                                  << " does not believe it is a config server"};
        }
        default:
            // The ismaster response was malformed.
            return status;
    }
}

StatusWith<boost::optional<executor::RemoteCommandRequest>>
ShardingNetworkConnectionHook::makeRequest(const HostAndPort& remoteHost) {
    return {boost::none};
}

Status ShardingNetworkConnectionHook::handleReply(const HostAndPort& remoteHost,
                                                  executor::RemoteCommandResponse&& response) {
    MONGO_UNREACHABLE;
}
}  // namespace merizo
