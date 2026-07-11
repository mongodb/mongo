// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * An implementation of NetworkConnectionHook for handling sharding-specific operations such
 * as sending sharding initialization information to shards and maintaining this process' notion of
 * the config server optime.
 */
class ShardingNetworkConnectionHook final : public executor::NetworkConnectionHook {
public:
    ShardingNetworkConnectionHook() = default;
    ~ShardingNetworkConnectionHook() override = default;

    /**
     * Checks that the given host is valid to be used in this sharded cluster, based on its
     * "hello" response.
     */
    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& request,
                        const executor::RemoteCommandResponse& helloReply) override;

    /**
     * Implementation of validateHost can be called without a ShardingNetworkConnectionHook
     * instance.
     */
    static Status validateHostImpl(const HostAndPort& remoteHost,
                                   const executor::RemoteCommandResponse& helloReply);

    /**
     * Makes a SetShardVersion request for initializing sharding information on the new connection.
     */
    StatusWith<boost::optional<executor::RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) override;

    /**
     * Confirms that the SetShardVersion request made in makeRequest ran successfully.
     */
    Status handleReply(const HostAndPort& remoteHost,
                       executor::RemoteCommandResponse&& response) override;
};

}  // namespace mongo
