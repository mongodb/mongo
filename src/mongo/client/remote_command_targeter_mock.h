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
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] RemoteCommandTargeterMock final : public RemoteCommandTargeter {
public:
    RemoteCommandTargeterMock();
    ~RemoteCommandTargeterMock() override;

    /**
     * Shortcut for unit-tests.
     */
    static std::shared_ptr<RemoteCommandTargeterMock> get(
        std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Returns the value last set by setConnectionStringReturnValue.
     */
    ConnectionString connectionString() override;

    /**
     * Returns the return value last set by setFindHostReturnValue.
     * Returns ErrorCodes::InternalError if setFindHostReturnValue was never called.
     */
    SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     const CancellationToken& cancelToken,
                                     const TargetingMetadata& targetingMetadata) override;

    SemiFuture<std::vector<HostAndPort>> findHosts(const ReadPreferenceSetting& readPref,
                                                   const TargetingMetadata& targetingMetadata,
                                                   const CancellationToken& cancelToken) override;

    StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                     const ReadPreferenceSetting& readPref,
                                     const TargetingMetadata& targetingMetadata) override;

    /**
     * Adds host to a set of hosts marked down, otherwise a no-op.
     */
    void markHostNotPrimary(const HostAndPort& host, const Status& status) override;

    /**
     * Adds host to a set of hosts marked down, otherwise a no-op.
     */
    void markHostUnreachable(const HostAndPort& host, const Status& status) override;

    /**
     * Adds host to a set of hosts marked down, otherwise a no-op.
     */
    void markHostShuttingDown(const HostAndPort& host, const Status& status) override;

    /**
     * Sets the return value for the next call to connectionString.
     */
    void setConnectionStringReturnValue(ConnectionString returnValue);

    /**
     * Sets the return value for the next call to findHost.
     */
    void setFindHostReturnValue(StatusWith<HostAndPort> returnValue);

    void setFindHostsReturnValue(StatusWith<std::vector<HostAndPort>> returnValue);

    /**
     * Returns the current set of hosts marked down and resets the mock's internal list of marked
     * down hosts.
     */
    std::set<HostAndPort> getAndClearMarkedDownHosts();

private:
    ConnectionString _connectionStringReturnValue;
    StatusWith<std::vector<HostAndPort>> _findHostReturnValue;

    // Protects _hostsMarkedDown.
    mutable std::mutex _mutex;

    // HostAndPorts marked not primary or unreachable. Meant to verify a code path updates the
    // RemoteCommandTargeterMock.
    std::set<HostAndPort> _hostsMarkedDown;
};

}  // namespace mongo
