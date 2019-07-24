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

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class ReplicaSetMonitor;
class ReplicaSetMonitorTest;
struct ReadPreferenceSetting;
typedef std::shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

/**
 * Holds state about a replica set and provides a means to refresh the local view.
 * All methods perform the required synchronization to allow callers from multiple threads.
 */
class ReplicaSetMonitor {
    ReplicaSetMonitor(const ReplicaSetMonitor&) = delete;
    ReplicaSetMonitor& operator=(const ReplicaSetMonitor&) = delete;

public:
    class Refresher;

    /**
     * Initializes local state from a MongoURI.
     */
    ReplicaSetMonitor(const MongoURI& uri);

    /**
     * Schedules the initial refresh task into task executor.
     */
    void init();

    /**
     * Ends any ongoing refreshes.
     */
    void drop();

    /**
     * Returns a host matching the given read preference or an error, if no host matches.
     *
     * @param readPref Read preference to match against
     * @param maxWait If no host is readily available, which matches the specified read preference,
     *   wait for one to become available for up to the specified time and periodically refresh
     *   the view of the set. The call may return with an error earlier than the specified value,
     *   if none of the known hosts for the set are reachable within some number of attempts.
     *   Note that if a maxWait of 0ms is specified, this method may still attempt to contact
     *   every host in the replica set up to one time.
     *
     * Known errors are:
     *  FailedToSatisfyReadPreference, if node cannot be found, which matches the read preference.
     */
    SemiFuture<HostAndPort> getHostOrRefresh(const ReadPreferenceSetting& readPref,
                                             Milliseconds maxWait = kDefaultFindHostTimeout);

    SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref, Milliseconds maxWait = kDefaultFindHostTimeout);

    /**
     * Returns the host we think is the current master or uasserts.
     *
     * This is a thin wrapper around getHostOrRefresh so this will also refresh our view if we
     * don't think there is a master at first. The main difference is that this will uassert
     * rather than returning an empty HostAndPort.
     */
    HostAndPort getMasterOrUassert();

    /**
     * Notifies this Monitor that a host has failed because of the specified error 'status' and
     * should be considered down.
     *
     * Call this when you get a connection error. If you get an error while trying to refresh our
     * view of a host, call Refresher::failedHost instead because it bypasses taking the monitor's
     * mutex.
     */
    void failedHost(const HostAndPort& host, const Status& status);

    /**
     * Returns true if this node is the master based ONLY on local data. Be careful, return may
     * be stale.
     */
    bool isPrimary(const HostAndPort& host) const;

    /**
     * Returns true if host is part of this set and is considered up (meaning it can accept
     * queries).
     */
    bool isHostUp(const HostAndPort& host) const;

    /**
     * Returns the minimum wire version supported across the replica set.
     */
    int getMinWireVersion() const;

    /**
     * Returns the maximum wire version supported across the replica set.
     */
    int getMaxWireVersion() const;

    /**
     * The name of the set.
     */
    std::string getName() const;

    /**
     * Returns a std::string with the format name/server1,server2.
     * If name is empty, returns just comma-separated list of servers.
     * It IS updated to reflect the current members of the set.
     */
    std::string getServerAddress() const;

    /**
     * Returns the URI that was used to construct this monitor.
     * It IS NOT updated to reflect the current members of the set.
     */
    const MongoURI& getOriginalUri() const;

    /**
     * Is server part of this set? Uses only cached information.
     */
    bool contains(const HostAndPort& server) const;

    /**
     * Writes information about our cached view of the set to a BSONObjBuilder. If
     * forFTDC, trim to minimize its size for full-time diagnostic data capture.
     */
    void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const;

    /**
     * Returns true if the monitor knows a usable primary from it's interal view.
     */
    bool isKnownToHaveGoodPrimary() const;

    /**
     * Marks the instance as removed to exit refresh sooner.
     */
    void markAsRemoved();

    /**
     * Creates a new ReplicaSetMonitor, if it doesn't already exist.
     */
    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const std::string& name,
                                                             const std::set<HostAndPort>& servers);

    static std::shared_ptr<ReplicaSetMonitor> createIfNeeded(const MongoURI& uri);

    /**
     * gets a cached Monitor per name. If the monitor is not found and createFromSeed is false,
     * it will return none. If createFromSeed is true, it will try to look up the last known
     * servers list for this set and will create a new monitor using that as the seed list.
     */
    static std::shared_ptr<ReplicaSetMonitor> get(const std::string& name);

    /**
     * Removes the ReplicaSetMonitor for the given set name from _sets, which will delete it.
     * If clearSeedCache is true, then the cached seed std::string for this Replica Set will be
     * removed from _seedServers.
     */
    static void remove(const std::string& name);

    /**
     * Returns the change notifier for the underlying ReplicaMonitorManager
     */
    static ReplicaSetChangeNotifier& getNotifier();

    /**
     * Permanently stops all monitoring on replica sets and clears all cached information
     * as well. As a consequence, NEVER call this if you have other threads that have a
     * DBClientReplicaSet instance. This method should be used for unit test only.
     */
    static void cleanup();

    /**
     * Use these to speed up tests by disabling the sleep-and-retry loops and cause errors to be
     * reported immediately.
     */
    static void disableRefreshRetries_forTest();

    /**
     * Permanently stops all monitoring on replica sets.
     */
    static void shutdown();

    /**
     * Returns the refresh period that is given to all new SetStates.
     */
    static Seconds getDefaultRefreshPeriod();

    //
    // internal types (defined in replica_set_monitor_internal.h)
    //

    struct IsMasterReply;
    struct ScanState;
    struct SetState;
    typedef std::shared_ptr<ScanState> ScanStatePtr;
    typedef std::shared_ptr<SetState> SetStatePtr;

    /**
     * Allows tests to set initial conditions and introspect the current state.
     */
    explicit ReplicaSetMonitor(const SetStatePtr& initialState);
    ~ReplicaSetMonitor();

    /**
     * The default timeout, which will be used for finding a replica set host if the caller does
     * not explicitly specify it.
     */
    static const Seconds kDefaultFindHostTimeout;

    /**
     * Defaults to false, meaning that if multiple hosts meet a criteria we pick one at random.
     * This is required by the replica set driver spec. Set this to true in tests that need host
     * selection to be deterministic.
     *
     * NOTE: Used by unit-tests only.
     */
    static bool useDeterministicHostSelection;

    /**
     * This is for use in tests using MockReplicaSet to ensure that a full scan completes before
     * continuing.
     */
    void runScanForMockReplicaSet();

private:
    Future<std::vector<HostAndPort>> _getHostsOrRefresh(const ReadPreferenceSetting& readPref,
                                                        Milliseconds maxWait);
    /**
     * If no scan is in-progress, this function is responsible for setting up a new scan. Otherwise,
     * does nothing.
     */
    static void _ensureScanInProgress(const SetStatePtr&);

    const SetStatePtr _state;
};


/**
 * Refreshes the local view of a replica set.
 *
 * All logic related to choosing the hosts to contact and updating the SetState based on replies
 * lives in this class. Use of this class should always be guarded by SetState::mutex unless in
 * single-threaded use by ReplicaSetMonitorTest.
 */
class ReplicaSetMonitor::Refresher {
public:
    explicit Refresher(const SetStatePtr& setState);

    struct NextStep {
        enum StepKind {
            CONTACT_HOST,  /// Contact the returned host
            WAIT,          /// Wait on condition variable and try again.
            DONE,          /// No more hosts to contact in this Refresh round
        };

        explicit NextStep(StepKind step, const HostAndPort& host = HostAndPort())
            : step(step), host(host) {}

        StepKind step;
        HostAndPort host;
    };

    /**
     * Returns the next step to take.
     *
     * By calling this, you promise to call receivedIsMaster or failedHost if the NextStep is
     * CONTACT_HOST.
     */
    NextStep getNextStep();

    /**
     * Call this if a host returned from getNextStep successfully replied to an isMaster call.
     * Negative latencyMicros are ignored.
     */
    void receivedIsMaster(const HostAndPort& from, int64_t latencyMicros, const BSONObj& reply);

    /**
     * Call this if a host returned from getNextStep failed to reply to an isMaster call.
     */
    void failedHost(const HostAndPort& host, const Status& status);

    /**
     * Starts a new scan over the hosts in set.
     */
    void startNewScan();

    /**
     * First, checks that the "reply" is not from a stale primary by comparing the electionId of
     * "reply" to the maxElectionId recorded by the SetState and returns OK status if "reply"
     * belongs to a non-stale primary. Otherwise returns a failed status.
     *
     * The 'from' parameter specifies the node from which the response is received.
     *
     * Updates _set and _scan based on set-membership information from a master.
     * Applies _scan->unconfirmedReplies to confirmed nodes.
     * Does not update this host's node in _set->nodes.
     */
    Status receivedIsMasterFromMaster(const HostAndPort& from, const IsMasterReply& reply);

    /**
     * Schedules isMaster requests to all hosts that currently need to be contacted.
     * Does nothing if requests have already been sent to all known hosts.
     */
    void scheduleNetworkRequests();

    void scheduleIsMaster(const HostAndPort& host);

private:
    // Both pointers are never NULL
    SetStatePtr _set;
    ScanStatePtr _scan;  // May differ from _set->currentScan if a new scan has started.
};

}  // namespace mongo
