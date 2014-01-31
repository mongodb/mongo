/*    Copyright 2014 MongoDB Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <string>
#include <set>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
    class ReplicaSetMonitor;
    class TagSet;
    struct ReadPreferenceSetting;
    typedef shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;

    /**
     * Holds state about a replica set and provides a means to refresh the local view.
     * All methods perform the required synchronization to allow callers from multiple threads.
     */
    class MONGO_CLIENT_API ReplicaSetMonitor {
        MONGO_DISALLOW_COPYING(ReplicaSetMonitor);
    public:
        class Refresher;

        typedef boost::function<void(const std::string& setName,
                                     const std::string& newConnectionString)>
                ConfigChangeHook;

        /**
         * Initializes local state.
         *
         * seeds must not be empty.
         */
        ReplicaSetMonitor(StringData name, const std::set<HostAndPort>& seeds);

        /**
         * Returns a host matching criteria or an empty HostAndPort if no host matches.
         *
         * If no host matches initially, will then attempt to refresh our view of the set by
         * contacting other hosts. May still return no result if no host matches following a
         * refresh.
         */
        HostAndPort getHostOrRefresh(const ReadPreferenceSetting& criteria);

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
         * How may times in a row have we tried to refresh without successfully contacting any hosts
         * who claim to be members of this set?
         */
        int getConsecutiveFailedScans() const;

        /**
         * The name of the set.
         */
        std::string getName() const;

        /**
         * Returns a string with the format name/server1,server2.
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
         * Creates a new ReplicaSetMonitor, if it doesn't already exist.
         */
        static void createIfNeeded(const std::string& name , const std::set<HostAndPort>& servers);

        /**
         * gets a cached Monitor per name. If the monitor is not found and createFromSeed is false,
         * it will return none. If createFromSeed is true, it will try to look up the last known
         * servers list for this set and will create a new monitor using that as the seed list.
         */
        static ReplicaSetMonitorPtr get(const std::string& name, bool createFromSeed = false);

        /**
         * Returns all the currently tracked replica set names.
         */
        static std::set<std::string> getAllTrackedSets();

        /**
         * Removes the ReplicaSetMonitor for the given set name from _sets, which will delete it.
         * If clearSeedCache is true, then the cached seed string for this Replica Set will be
         * removed from _seedServers.
         */
        static void remove(const std::string& name, bool clearSeedCache = false);

        /**
         * Sets the hook to be called whenever the config of any replica set changes.
         * Currently only 1 globally, so this asserts if one already exists.
         *
         * The hook will be called from a fresh thread. It is responsible for initializing any
         * thread-local state and ensuring that no exceptions escape.
         *
         * The hook must not be changed while the program has multiple threads.
         */
        static void setConfigChangeHook(ConfigChangeHook hook);

        /**
         * Permanently stops all monitoring on replica sets and clears all cached information
         * as well. As a consequence, NEVER call this if you have other threads that have a
         * DBClientReplicaSet instance.
         */
        static void cleanup();

        /**
         * If a ReplicaSetMonitor has been refreshed more than this many times in a row without
         * finding any live nodes claiming to be in the set, the ReplicaSetMonitorWatcher will stop
         * periodic background refreshes of this set.
         */
        static int maxConsecutiveFailedChecks;

        //
        // internal types (defined in replica_set_monitor_internal.h)
        //

        struct IsMasterReply;
        struct ScanState;
        struct SetState;
        typedef boost::shared_ptr<ScanState> ScanStatePtr;
        typedef boost::shared_ptr<SetState> SetStatePtr;

        //
        // FOR TESTING ONLY
        //

        /**
         * Allows tests to set initial conditions and introspect the current state.
         */
        explicit ReplicaSetMonitor(const SetStatePtr& initialState)
            : _state(initialState)
        {}

        /**
         * Defaults to false, meaning that if multiple hosts meet a criteria we pick one at random.
         * This is required by the replica set driver spec. Set this to true in tests that need host
         * selection to be deterministic.
         */
        static bool useDeterministicHostSelection;

    private:
        const SetStatePtr _state; // never NULL
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
    class MONGO_CLIENT_API ReplicaSetMonitor::Refresher {
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
        void refreshAll() { _refreshUntilMatches(NULL); }

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
                CONTACT_HOST, /// Contact the returned host
                WAIT, /// Wait on condition variable and try again.
                DONE, /// No more hosts to contact in this Refresh round
            };

            explicit NextStep(StepKind step, const HostAndPort& host=HostAndPort())
                : step(step)
                , host(host)
            {}

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
        bool startedNewScan() const { return _startedNewScan; }

        /**
         * Starts a new scan over the hosts in set.
         */
        static ScanStatePtr startNewScan(const SetState* set);

    private:
        /**
         * Updates _set and _scan based on set-membership information from a master.
         * Applies _scan->unconfirmedReplies to confirmed nodes.
         * Does not update this host's node in _set->nodes.
         */
        void receivedIsMasterFromMaster(const IsMasterReply& reply);

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
        ScanStatePtr _scan; // May differ from _set->currentScan if a new scan has started.
        bool _startedNewScan;
    };
}
