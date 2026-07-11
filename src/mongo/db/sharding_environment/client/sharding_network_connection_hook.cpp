// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/client/sharding_network_connection_hook.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

Status ShardingNetworkConnectionHook::validateHost(
    const HostAndPort& remoteHost,
    const BSONObj&,
    const executor::RemoteCommandResponse& helloReply) {
    return validateHostImpl(remoteHost, helloReply);
}

Status ShardingNetworkConnectionHook::validateHostImpl(
    const HostAndPort& remoteHost, const executor::RemoteCommandResponse& helloReply) {
    auto shard =
        Grid::get(getGlobalServiceContext())->shardRegistry()->getShardForHostNoReload(remoteHost);
    if (!shard) {
        return {ErrorCodes::ShardNotFound,
                str::stream() << "No shard found for host: " << remoteHost.toString()};
    }

    long long configServerModeNumber;
    auto status = bsonExtractIntegerField(helloReply.data, "configsvr", &configServerModeNumber);

    switch (status.code()) {
        case ErrorCodes::OK: {
            // The hello response indicates remoteHost is a config server.
            if (!shard->isConfig()) {
                // If we are in the process of demoting to a replica set, the secondary may simply
                // have not been restarted with the new options yet.
                if (serverGlobalParams.replicaSetConfigShardMaintenanceMode) {
                    LOGV2_INFO(10848801,
                               "Replica set member is a config server and this node is "
                               "not, but ignoring this since reconfiguration is in process",
                               "host"_attr = remoteHost.toString());
                    return Status::OK();
                }

                return {ErrorCodes::InvalidOptions,
                        str::stream() << "Surprised to discover that " << remoteHost.toString()
                                      << " believes it is a config server"};
            }
            return Status::OK();
        }
        case ErrorCodes::NoSuchKey: {
            // The hello response indicates that remoteHost is not a config server, or that
            // the config server is running a version prior to the 3.1 development series.
            if (!shard->isConfig()) {
                return Status::OK();
            }

            // If we are in the process of promoting to a sharded cluster, the secondary may simply
            // not have been restarted with the new options yet.
            if (serverGlobalParams.replicaSetConfigShardMaintenanceMode) {
                LOGV2_INFO(10848802,
                           "Replica set member is not a config server, but ignoring this since "
                           "reconfiguration is in process",
                           "host"_attr = remoteHost.toString());
                return Status::OK();
            }

            // In a standby cluster, the config server is not started with --configsvr, so we allow
            // the mongoS to ignore this case if it is started with the --configOnly option.
            if (serverGlobalParams.configOnly) {
                LOGV2_DEBUG(12296001,
                            2,
                            "Replica set member is not a config server, but ignoring this since "
                            "the mongoS is started with --configOnly",
                            "host"_attr = remoteHost.toString());
                return Status::OK();
            }

            return {ErrorCodes::InvalidOptions,
                    str::stream() << "Surprised to discover that " << remoteHost.toString()
                                  << " does not believe it is a config server"};
        }
        default:
            // The hello response was malformed.
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
}  // namespace mongo
