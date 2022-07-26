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

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Interface encapsulating the targeting logic for a given replica set or a standalone host.
 */
class RemoteCommandTargeter {
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
                                             const ReadPreferenceSetting& readPref) = 0;


    /**
     * Finds a host that matches the read preference specified by readPref, blocking for up to
     * gDefaultFindReplicaSetHostTimeoutMS milliseconds, if a match cannot be found immediately.
     *
     * DEPRECATED. Prefer findHost(OperationContext*, const ReadPreferenceSetting&), whenever an
     * OperationContext is available.
     */
    virtual SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                             const CancellationToken& cancelToken) = 0;

    virtual SemiFuture<std::vector<HostAndPort>> findHosts(
        const ReadPreferenceSetting& readPref, const CancellationToken& cancelToken) = 0;


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
        } else if (status == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
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
