// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/targeting_metadata.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

namespace mongo {

struct ReadPreferenceSetting;

class [[MONGO_MOD_OPEN]] ReplicaSetMonitorInterface {
public:
    virtual ~ReplicaSetMonitorInterface() = default;

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
     * @param excludedHosts List of hosts that are not eligible to be chosen.
     *
     * Known errors are:
     *  FailedToSatisfyReadPreference, if node cannot be found, which matches the read preference.
     */
    virtual Future<HostAndPort> getHostOrRefresh(const ReadPreferenceSetting& readPref,
                                                 const TargetingMetadata& targetingMetadata,
                                                 const CancellationToken& cancelToken) = 0;

    virtual Future<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref,
        const TargetingMetadata& targetingMetadata,
        const CancellationToken& cancelToken) = 0;

    /**
     * Returns the host the RSM thinks is the current primary or uasserts.
     *
     * This is a thin wrapper around getHostOrRefresh and will also refresh the view if a primary
     * does not exist. The main difference is that this will uassert rather than returning an empty
     * HostAndPort.
     */
    virtual HostAndPort getPrimaryOrUassert() = 0;

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

    /**
     * Returns the ping time of `server` if available.
     */
    virtual boost::optional<Microseconds> pingTime(const HostAndPort& server) const = 0;
};

}  // namespace mongo
