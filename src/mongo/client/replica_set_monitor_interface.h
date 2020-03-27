/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

struct ReadPreferenceSetting;

class ReplicaSetMonitorInterface {
public:
    virtual ~ReplicaSetMonitorInterface() = default;

    /**
     * The default timeout, which will be used for finding a replica set host if the caller does
     * not explicitly specify it.
     */
    static constexpr Seconds kDefaultFindHostTimeout{15};

    /**
     * Schedules the initial refresh task into task executor.
     */
    virtual void init() = 0;

    /**
     * Ends any ongoing refreshes.
     */
    virtual void drop() = 0;

    /**
     * Returns a host matching the given read preference or an error, if no host matches.
     *
     * @param readPref Read preference to match against
     * @param maxWait If no host is readily available that matches the specified read preference,
     *   wait for one to become available for up to the specified time and periodically refresh
     *   the view of the set. The call may return with an error earlier than the specified value,
     *   if none of the known hosts for the set are reachable within some number of attempts.
     *   Note that if a maxWait of 0ms is specified, this method may still attempt to contact
     *   every host in the replica set up to one time.
     *
     * Known errors are:
     *  FailedToSatisfyReadPreference, if node cannot be found, which matches the read preference.
     */
    virtual SemiFuture<HostAndPort> getHostOrRefresh(
        const ReadPreferenceSetting& readPref, Milliseconds maxWait = kDefaultFindHostTimeout) = 0;

    virtual SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref, Milliseconds maxWait = kDefaultFindHostTimeout) = 0;

    /**
     * Returns the host the RSM thinks is the current master or uasserts.
     *
     * This is a thin wrapper around getHostOrRefresh and will also refresh the view if a primary
     * does not exist. The main difference is that this will uassert rather than returning an empty
     * HostAndPort.
     */
    virtual HostAndPort getMasterOrUassert() = 0;

    /**
     * Notifies this Monitor that a host has failed because of the specified error 'status' and
     * should be considered down.
     *
     * The sdam version of the Monitor makes a distinction between failures happening before or
     * after the initial handshake for the connection. The failedHost method is kept for backwards
     * compatibility, and is equivalent to failedHostPostHandshake.
     */
    virtual void failedHost(const HostAndPort& host, const Status& status) = 0;
    virtual void failedHostPreHandshake(const HostAndPort& host,
                                        const Status& status,
                                        BSONObj bson) = 0;
    virtual void failedHostPostHandshake(const HostAndPort& host,
                                         const Status& status,
                                         BSONObj bson) = 0;

    /**
     * Returns true if this node is the master based ONLY on local data. Be careful, return may
     * be stale.
     */
    virtual bool isPrimary(const HostAndPort& host) const = 0;

    /**
     * Returns true if host is part of this set and is considered up (meaning it can accept
     * queries).
     */
    virtual bool isHostUp(const HostAndPort& host) const = 0;

    /**
     * Returns the minimum wire version supported across the replica set.
     */
    virtual int getMinWireVersion() const = 0;

    /**
     * Returns the maximum wire version supported across the replica set.
     */
    virtual int getMaxWireVersion() const = 0;

    /**
     * The name of the set.
     */
    virtual std::string getName() const = 0;

    /**
     * Returns a std::string with the format name/server1,server2.
     * If name is empty, returns just comma-separated list of servers.
     * It IS updated to reflect the current members of the set.
     */
    virtual std::string getServerAddress() const = 0;

    /**
     * Returns the URI that was used to construct this monitor.
     * It IS NOT updated to reflect the current members of the set.
     */
    virtual const MongoURI& getOriginalUri() const = 0;

    /**
     * Is server part of this set? Uses only cached information.
     */
    virtual bool contains(const HostAndPort& server) const = 0;

    /**
     * Writes information about our cached view of the set to a BSONObjBuilder. If
     * forFTDC, trim to minimize its size for full-time diagnostic data capture.
     */
    virtual void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const = 0;

    /**
     * Returns true if the monitor knows a usable primary from it's interal view.
     */
    virtual bool isKnownToHaveGoodPrimary() const = 0;

    /**
     * This is for use in tests using MockReplicaSet to ensure that a full scan completes before
     * continuing.
     */
    virtual void runScanForMockReplicaSet() = 0;
};

}  // namespace mongo
