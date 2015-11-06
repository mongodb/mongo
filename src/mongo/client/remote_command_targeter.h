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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ConnectionString;
class OperationContext;
struct ReadPreferenceSetting;
struct HostAndPort;
template <typename T>
class StatusWith;

/**
 * Interface encapsulating the targeting logic for a given replica set or a standalone host.
 */
class RemoteCommandTargeter {
    MONGO_DISALLOW_COPYING(RemoteCommandTargeter);

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
     * Obtains a host, which matches the read preferences specified by readPref, blocking for the
     * specified maxWait milliseconds, if a match cannot be found immediately.
     *
     * Specifying a maxWait of zero means non-blocking. I.e., the call will just check the in-memory
     * cached view of the replica set's host state and won't wait for it to be refreshed if it is
     * found to be stale.
     *
     * Returns OK and a host and port to use for the specified read preference or an ErrorCode.
     * Known error codes are:
     *   All error codes which can be returned by ReplicaSetMonitor::getHostOrRefresh.
     */
    virtual StatusWith<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                             Milliseconds maxWait = Milliseconds(0)) = 0;

    /**
     * Reports to the targeter that a NotMaster response was received when communicating with
     * "host', and so it should update its bookkeeping to avoid giving out the host again on a
     * subsequent request for the primary.
     */
    virtual void markHostNotMaster(const HostAndPort& host) = 0;

    /**
     * Similar to markHostNotMaster(). Reports to the targeter that a HostUnreachable response was
     * received when communicating with "host". The targeter should update its bookkeeping to avoid
     * giving out the same host on a subsequent request.
     */
    virtual void markHostUnreachable(const HostAndPort& host) = 0;

    /**
     * Based on the remaining time of the operation and the default max wait time for findHost,
     * selects an appropriate value to pass to the maxWait argument of the findHost method, so it
     * has high likelyhood in returning on time and also leaving time for the rest of the call to
     * complete.
     */
    static Milliseconds selectFindHostMaxWaitTime(OperationContext* txn);

protected:
    RemoteCommandTargeter() = default;
};

}  // namespace mongo
