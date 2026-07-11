// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class ReplicaSetMonitor;

/**
 * Implements a replica-set backed remote command targeter, which monitors the specified
 * replica set and responds to state changes.
 */
class RemoteCommandTargeterRS final : public RemoteCommandTargeter {
public:
    /**
     * Instantiates a new targeter for the specified replica set and seed hosts. The RS name
     * and the seed hosts must match.
     */
    RemoteCommandTargeterRS(const std::string& rsName, const std::vector<HostAndPort>& seedHosts);

    ConnectionString connectionString() override;

    StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                     const ReadPreferenceSetting& readPref,
                                     const TargetingMetadata& targetingMetadata) override;

    SemiFuture<std::vector<HostAndPort>> findHosts(const ReadPreferenceSetting& readPref,
                                                   const TargetingMetadata& targetingMetadata,
                                                   const CancellationToken& cancelToken) override;

    SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     const CancellationToken& cancelToken,
                                     const TargetingMetadata& targetingMetadata) override;

    void markHostNotPrimary(const HostAndPort& host, const Status& status) override;

    void markHostUnreachable(const HostAndPort& host, const Status& status) override;

    void markHostShuttingDown(const HostAndPort& host, const Status& status) override;

private:
    /**
     * Returns true if the local host must be targeted, i.e. this is a shardsvr mongod targeting the
     * replica set it is in and the readPreference has been pre-targeted by the client connected to
     * it.
     */
    bool _mustTargetLocalHost(const ReadPreferenceSetting& readPref) const;

    ServiceContext* const _serviceContext;

    // Name of the replica set which this targeter maintains
    const std::string _rsName;

    // Monitor for this replica set
    std::shared_ptr<ReplicaSetMonitor> _rsMonitor;

    // Set to true if this is shardsvr mongod targeting the replica set it is in.
    bool _isTargetingLocalRS = false;
};

}  // namespace mongo
