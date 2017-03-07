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
#include "mongo/util/net/hostandport.h"
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
     * Finds a host matching readPref blocking up to 20 seconds or until the given operation is
     * interrupted or its deadline expires.
     *
     * TODO(schwerin): Once operation max-time behavior is more uniformly integrated into sharding,
     * remove the 20-second ceiling on wait time.
     */
    virtual StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                             const ReadPreferenceSetting& readPref) = 0;


    /**
     * Finds a host that matches the read preference specified by readPref, blocking for up to
     * specified maxWait milliseconds, if a match cannot be found immediately.
     *
     * DEPRECATED. Prefer findHost(OperationContext*, const ReadPreferenceSetting&), whenever
     * an OperationContext is available.
     */
    virtual StatusWith<HostAndPort> findHostWithMaxWait(const ReadPreferenceSetting& readPref,
                                                        Milliseconds maxWait) = 0;

    /**
     * Finds a host matching the given read preference, giving up if a match is not found promptly.
     *
     * This method may still engage in blocking networking calls, but will attempt contact every
     * member of the replica set at most one time.
     *
     * TODO(schwerin): Change this implementation to not perform any networking, once existing
     * callers have been shown to be safe with this behavior or changed to call findHost.
     */
    StatusWith<HostAndPort> findHostNoWait(const ReadPreferenceSetting& readPref) {
        return findHostWithMaxWait(readPref, Milliseconds::zero());
    }

    /**
     * Reports to the targeter that a 'status' indicating a not master error was received when
     * communicating with 'host', and so it should update its bookkeeping to avoid giving out the
     * host again on a subsequent request for the primary.
     */
    virtual void markHostNotMaster(const HostAndPort& host, const Status& status) = 0;

    /**
     * Reports to the targeter that a 'status' indicating a network error was received when trying
     * to communicate with 'host', and so it should update its bookkeeping to avoid giving out the
     * host again on a subsequent request for the primary.
     */
    virtual void markHostUnreachable(const HostAndPort& host, const Status& status) = 0;

protected:
    RemoteCommandTargeter() = default;
};

}  // namespace mongo
