/*    Copyright 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <atomic>
#include <memory>
#include <memory>
#include <set>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class ReplicaSetMonitor;
struct ReadPreferenceSetting;
typedef std::shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

/**
 * Holds state about a replica set and provides a means to refresh the local view.
 * All methods perform the required synchronization to allow callers from multiple threads.
 */
class ReplicaSetMonitor : public std::enable_shared_from_this<ReplicaSetMonitor> {
    MONGO_DISALLOW_COPYING(ReplicaSetMonitor);

public:
    class Refresher;

    typedef stdx::function<void(const std::string& setName, const std::string& newConnectionString)>
        ConfigChangeHook;

    /**
     * Initializes local state.
     *
     * seeds must not be empty.
     */
    ReplicaSetMonitor(StringData name, const std::set<HostAndPort>& seeds);

    /**
     * Schedules the initial refresh task into task executor.
     */
    void init();

    /**
     * Returns a host matching the given read preference or an error, if no host matches.
     *
     * @param readPref Read preference to match against
     * @param maxWait If no host is readily available, which matches the specified read preference,
     *   wait for one to become available for up to the specified time and periodically refresh
     *   the view of the set. The call may return with an error earlier than the specified value,
     *   if none of the known hosts for the set are reachable within some number of attempts.
     *
     * Known errors are:
     *  FailedToSatisfyReadPreference, if node cannot be found, which matches the read preference.
     *  ReplicaSetNotFound, if none of the known hosts for the replica set are reachable within
     *      maxConsecutiveFailedChecks number of attempts.
     */
    StatusWith<HostAndPort> getHostOrRefresh(const ReadPreferenceSetting& readPref,
                                             Milliseconds maxWait = kDefaultFindHostTimeout);

    /**
     * Returns the host we think is the current master or uasserts.
     *
     * This is a thin wrapper around getHostOrRefresh so this will also refresh our view if we
     * don't think there is a master at first. The main difference is that this will uassert
     * rather than returning an empty HostAndPort.
     */
    HostAndPort getMasterOrUassert();

    /**
     * Returns a refresher object that can be used to update our view of the set.
     * If a refresh is currently in-progress, the returned Refresher will participate in the
     * current refresh round.
     */
    Refresher startOrContinueRefresh();

    /**
     * Notifies this Monitor that a host has failed and should be considered down.
     *
     * Call this when you get a connection error. If you get an error while trying to refresh
     * our view of a host, call Refresher::hostFailed() instead.
     */
    void failedHost(const HostAndPort& host);

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
     * This call will return false if this monitor ever enters a state where none of the nodes could
     * be contacted for some amount of attempts. Such monitors will be removed from the periodic
     * refresh thread.
     */
    bool isSetUsable() const;

    /**
     * The name of the set.
     */
    std::string getName() const;

    /**
     * Returns a std::string with the format name/server1,server2.
     * If name is empty, returns just comma-separated list of servers.
     */
    std::string getServerAddress() const;

    /**
     * Is server part of this set? Uses only cached information.
     */
    bool contains(const HostAndPort& server) const;

    /**
     * Writes information about our cached view of the set to a BSONObjBuilder.
     */
    void appendInfo(BSONObjBuilder& b) const;

    /**
     * Returns true if the monitor knows a usable primary from it's interal view.
     */
    bool isKnownToHaveGoodPrimary() const;

    /**
     * Creates a new ReplicaSetMonitor, if it doesn't already exist.
     */
    static void createIfNeeded(const std::string& name, const std::set<HostAndPort>& servers);

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
     * Sets the hook to be called whenever the config of any replica set changes.
     * Currently only 1 globally, so this asserts if one already exists.
     *
     * The hook will be called from a fresh thread. It is responsible for initializing any
     * thread-local state and ensuring that no exceptions escape.
     *
     * The hook must not be changed while the program has multiple threads.
     */
    static void setAsynchronousConfigChangeHook(ConfigChangeHook hook);

    /**
     * Sets the hook to be called whenever the config of any replica set changes.
     * Currently only 1 globally, so this asserts if one already exists.
     *
     * The hook will be called inline while refreshing the ReplicaSetMonitor's view of the set
     * membership.  It is important that the hook not block for long as it will be running under
     * the ReplicaSetMonitor's mutex.
     *
     * The hook must not be changed while the program has multiple threads.
     */
    static void setSynchronousConfigChangeHook(ConfigChangeHook hook);

    /**
     * Permanently stops all monitoring on replica sets and clears all cached information
     * as well. As a consequence, NEVER call this if you have other threads that have a
     * DBClientReplicaSet instance.
     */
    static void cleanup();

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
    explicit ReplicaSetMonitor(const SetStatePtr& initialState) : _state(initialState) {}
    ~ReplicaSetMonitor();

    /**
     * If a ReplicaSetMonitor has been refreshed more than this many times in a row without
     * finding any live nodes claiming to be in the set, the ReplicaSetMonitor will stop
     * periodic background refreshes of this set.
     */
    static std::atomic<int> maxConsecutiveFailedChecks;  // NOLINT

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

private:
    /**
     * A callback passed to a task executor to refresh the replica set. It reschedules itself until
     * its canceled in d-tor.
     */
    void _refresh(const executor::TaskExecutor::CallbackArgs&);

    // Serializes refresh and protects _refresherHandle
    stdx::mutex _mutex;
    executor::TaskExecutor::CallbackHandle _refresherHandle;

    const SetStatePtr _state;
    executor::TaskExecutor* _executor;
};


/**
 * Refreshes the local view of a replica set.
 *
 * Use ReplicaSetMonitor::startOrContinueRefresh() to obtain a Refresher.
 *
 * Multiple threads can refresh a single set without any additional synchronization, however
 * they must each use their own Refresher object.
 *
 * All logic related to choosing the hosts to contact and updating the SetState based on replies
 * lives in this class.
 */
class ReplicaSetMonitor::Refresher {
public:
    /**
     * Contact hosts in the set to refresh our view, but stop once a host matches criteria.
     * Returns the matching host or empty if none match after a refresh.
     *
     * This is called by ReplicaSetMonitor::getHostWithRefresh()
     */
    HostAndPort refreshUntilMatches(const ReadPreferenceSetting& criteria) {
        return _refreshUntilMatches(&criteria);
    };

    /**
     * Refresh all hosts. Equivalent to refreshUntilMatches with a criteria that never
     * matches.
     *
     * This is intended to be called periodically, possibly from a background thread.
     */
    void refreshAll() {
        _refreshUntilMatches(NULL);
    }

    //
    // Remaining methods are only for testing and internal use.
    // Callers are responsible for holding SetState::mutex before calling any of these methods.
    //

    /**
     * Any passed-in pointers are shared with caller.
     *
     * If no scan is in-progress, this function is responsible for setting up a new scan.
     */
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
    void failedHost(const HostAndPort& host);

    /**
     * True if this Refresher started a new full scan rather than joining an existing one.
     */
    bool startedNewScan() const {
        return _startedNewScan;
    }

    /**
     * Starts a new scan over the hosts in set.
     */
    static ScanStatePtr startNewScan(const SetState* set);

private:
    /**
     * First, checks that the "reply" is not from a stale primary by
     * comparing the electionId of "reply" to the maxElectionId recorded by the SetState.
     * Returns true if "reply" belongs to a non-stale primary.
     *
     * Updates _set and _scan based on set-membership information from a master.
     * Applies _scan->unconfirmedReplies to confirmed nodes.
     * Does not update this host's node in _set->nodes.
     */
    bool receivedIsMasterFromMaster(const IsMasterReply& reply);

    /**
     * Adjusts the _scan work queue based on information from this host.
     * This should only be called with replies from non-masters.
     * Does not update _set at all.
     */
    void receivedIsMasterBeforeFoundMaster(const IsMasterReply& reply);

    /**
     * Shared implementation of refreshUntilMatches and refreshAll.
     * NULL criteria means refresh every host.
     * Handles own locking.
     */
    HostAndPort _refreshUntilMatches(const ReadPreferenceSetting* criteria);

    // Both pointers are never NULL
    SetStatePtr _set;
    ScanStatePtr _scan;  // May differ from _set->currentScan if a new scan has started.
    bool _startedNewScan;
};

}  // namespace mongo
