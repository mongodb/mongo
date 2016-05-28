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

/**
 * This is an internal header.
 * This should only be included by replica_set_monitor.cpp and replica_set_monitor_test.cpp.
 * This should never be included by any header.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

struct ReplicaSetMonitor::IsMasterReply {
    IsMasterReply() : ok(false) {}
    IsMasterReply(const HostAndPort& host, int64_t latencyMicros, const BSONObj& reply)
        : ok(false), host(host), latencyMicros(latencyMicros) {
        parse(reply);
    }

    /**
     * Never throws. If parsing fails for any reason, sets ok to false.
     */
    void parse(const BSONObj& obj);

    bool ok;      // if false, ignore all other fields
    BSONObj raw;  // Always owned. Other fields are allowed to be a view into this.
    std::string setName;
    bool isMaster;
    bool secondary;
    bool hidden;
    int configVersion{0};
    OID electionId;                     // Set if this isMaster reply is from the primary
    HostAndPort primary;                // empty if not present
    std::set<HostAndPort> normalHosts;  // both "hosts" and "passives"
    BSONObj tags;
    int minWireVersion{0};
    int maxWireVersion{0};

    // remaining fields aren't in isMaster reply, but are known to caller.
    HostAndPort host;
    int64_t latencyMicros;  // ignored if negative
};

struct ReplicaSetMonitor::SetState {
    MONGO_DISALLOW_COPYING(SetState);

public:
    /**
     * Holds the state of a single node in the replicaSet
     */
    struct Node {
        explicit Node(const HostAndPort& host);

        void markFailed();

        bool matches(const ReadPreference pref) const;

        /**
         * Checks if the given tag matches the tag attached to this node.
         *
         * Example:
         *
         * Tag of this node: { "dc": "nyc", "region": "na", "rack": "4" }
         *
         * match: {}
         * match: { "dc": "nyc", "rack": 4 }
         * match: { "region": "na", "dc": "nyc" }
         * not match: { "dc": "nyc", "rack": 2 }
         * not match: { "dc": "sf" }
         */
        bool matches(const BSONObj& tag) const;

        /**
         * Updates this Node based on information in reply. The reply must be from this host.
         */
        void update(const IsMasterReply& reply);

        HostAndPort host;
        bool isUp{false};
        bool isMaster{false};   // implies isUp
        int64_t latencyMicros;  // unknownLatency if unknown
        BSONObj tags;           // owned
        int minWireVersion{0};
        int maxWireVersion{0};
    };

    typedef std::vector<Node> Nodes;

    /**
     * seedNodes must not be empty
     */
    SetState(StringData name, const std::set<HostAndPort>& seedNodes);

    bool isUsable() const;

    /**
     * Returns a host matching criteria or an empty host if no known host matches.
     *
     * Note: Uses only local data and does not go over the network.
     */
    HostAndPort getMatchingHost(const ReadPreferenceSetting& criteria) const;

    /**
     * Returns the Node with the given host, or NULL if no Node has that host.
     */
    Node* findNode(const HostAndPort& host);

    /**
     * Returns the Node with the given host, or creates one if no Node has that host.
     * Maintains the sorted order of nodes.
     */
    Node* findOrCreateNode(const HostAndPort& host);

    void updateNodeIfInNodes(const IsMasterReply& reply);

    /**
     * Returns the connection string of the nodes that are known the be in the set because we've
     * seen them in the isMaster reply of a PRIMARY.
     */
    std::string getConfirmedServerAddress() const;

    /**
     * Returns the connection string of the nodes that are believed to be in the set because we've
     * seen them in the isMaster reply of non-PRIMARY nodes in our seed list.
     */
    std::string getUnconfirmedServerAddress() const;

    /**
     * Before unlocking, do DEV checkInvariants();
     */
    void checkInvariants() const;

    stdx::mutex mutex;  // must hold this to access any other member or method (except name).

    // If Refresher::getNextStep returns WAIT, you should wait on the condition_variable,
    // releasing mutex. It will be notified when either getNextStep will return something other
    // than WAIT, or a new host is available for consideration by getMatchingHost. Essentially,
    // this will be hit whenever the _refreshUntilMatches loop has the potential to make
    // progress.
    // TODO consider splitting cv into two: one for when looking for a master, one for all other
    // cases.
    stdx::condition_variable cv;

    const std::string name;  // safe to read outside lock since it is const
    int consecutiveFailedScans;
    std::set<HostAndPort> seedNodes;  // updated whenever a master reports set membership changes
    OID maxElectionId;                // largest election id observed by this ReplicaSetMonitor
    int configVersion{0};             // version number of the replica set config.
    HostAndPort lastSeenMaster;  // empty if we have never seen a master. can be same as current
    Nodes nodes;                 // maintained sorted and unique by host
    ScanStatePtr currentScan;    // NULL if no scan in progress
    int64_t latencyThresholdMicros;
    mutable PseudoRandom rand;  // only used for host selection to balance load
    mutable int roundRobin;     // used when useDeterministicHostSelection is true
};

struct ReplicaSetMonitor::ScanState {
    MONGO_DISALLOW_COPYING(ScanState);

public:
    ScanState() : foundUpMaster(false), foundAnyUpNodes(false) {}

    /**
     * Adds all hosts in container that aren't in triedHosts to hostsToScan, then shuffles the
     * queue.
     */
    template <typename Container>
    void enqueAllUntriedHosts(const Container& container, PseudoRandom& rand);

    // Access to fields is guarded by associated SetState's mutex.
    bool foundUpMaster;
    bool foundAnyUpNodes;
    std::deque<HostAndPort> hostsToScan;  // Work queue.
    std::set<HostAndPort> possibleNodes;  // Nodes reported by non-primary hosts.
    std::set<HostAndPort> waitingFor;     // Hosts we have dispatched but haven't replied yet.
    std::set<HostAndPort> triedHosts;     // Hosts that have been returned from getNextStep.

    // All responses go here until we find a master.
    typedef std::vector<IsMasterReply> UnconfirmedReplies;
    UnconfirmedReplies unconfirmedReplies;
};

}  // namespace mongo
