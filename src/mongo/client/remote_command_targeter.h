// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Interface encapsulating the targeting logic for a given replica set or a standalone host.
 */
class [[MONGO_MOD_PUBLIC]] RemoteCommandTargeter {
    RemoteCommandTargeter(const RemoteCommandTargeter&) = delete;
    RemoteCommandTargeter& operator=(const RemoteCommandTargeter&) = delete;

public:
    virtual ~RemoteCommandTargeter() = default;

    /**
     * Retrieves the full connection string for the replica set or standalone host which are
     * represented by this targeter. This value is always constant for a standalone host and may
     * vary for replica sets as hosts are added, discovered and removed during the lifetime of the
     * set.
     */
    virtual ConnectionString connectionString() = 0;

    /**
     * Finds a host matching readPref blocking up to gDefaultFindReplicaSetHostTimeoutMS
     * milliseconds or until the given operation is interrupted or its deadline expires.
     */
    virtual StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                             const ReadPreferenceSetting& readPref,
                                             const TargetingMetadata& targetingMetadata) = 0;


    /**
     * Finds a host that matches the read preference specified by readPref, blocking for up to
     * gDefaultFindReplicaSetHostTimeoutMS milliseconds, if a match cannot be found immediately.
     *
     * DEPRECATED. Prefer findHost(OperationContext*, const ReadPreferenceSetting&), whenever an
     * OperationContext is available.
     */
    virtual SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                             const CancellationToken& cancelToken,
                                             const TargetingMetadata& targetingMetadata) = 0;

    virtual SemiFuture<std::vector<HostAndPort>> findHosts(
        const ReadPreferenceSetting& readPref,
        const TargetingMetadata& targetingMetadata,
        const CancellationToken& cancelToken) = 0;

    /**
     * Checks the given status and updates the host bookkeeping accordingly.
     */
    void updateHostWithStatus(const HostAndPort& host, const Status& status) {
        if (status.isOK())
            return;

        if (ErrorCodes::isNotPrimaryError(status.code())) {
            markHostNotPrimary(host, status);
        } else if (ErrorCodes::isNetworkError(status.code())) {
            markHostUnreachable(host, status);
        } else if (ErrorCodes::isShutdownError(status.code())) {
            markHostShuttingDown(host, status);
        }
    };

    /**
     * Reports to the targeter that a 'status' indicating a not primary error was received when
     * communicating with 'host', and so it should update its bookkeeping to avoid giving out the
     * host again on a subsequent request for the primary.
     */
    virtual void markHostNotPrimary(const HostAndPort& host, const Status& status) = 0;

    /**
     * Reports to the targeter that a 'status' indicating a network error was received when trying
     * to communicate with 'host', and so it should update its bookkeeping to avoid giving out the
     * host again on a subsequent request for the primary.
     */
    virtual void markHostUnreachable(const HostAndPort& host, const Status& status) = 0;

    /**
     * Reports to the targeter that a 'status' indicating a shutdown error was received when trying
     * to communicate with 'host', and so it should update its bookkeeping to avoid giving out the
     * host again on a subsequent request for the primary.
     */
    virtual void markHostShuttingDown(const HostAndPort& host, const Status& status) = 0;

protected:
    RemoteCommandTargeter() = default;
};

}  // namespace mongo
