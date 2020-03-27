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

#include "mongo/base/string_data.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ScanningReplicaSetMonitor : public ReplicaSetMonitor {
    ScanningReplicaSetMonitor(const ScanningReplicaSetMonitor&) = delete;
    ScanningReplicaSetMonitor& operator=(const ScanningReplicaSetMonitor&) = delete;

public:
    class Refresher;

    static constexpr auto kExpeditedRefreshPeriod = Milliseconds(500);
    static constexpr auto kCheckTimeout = Seconds(5);

    ScanningReplicaSetMonitor(const MongoURI& uri);

    void init() override;

    void drop() override;

    SemiFuture<HostAndPort> getHostOrRefresh(
        const ReadPreferenceSetting& readPref,
        Milliseconds maxWait = kDefaultFindHostTimeout) override;

    SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref,
        Milliseconds maxWait = kDefaultFindHostTimeout) override;

    HostAndPort getMasterOrUassert() override;

    /*
     * For the ScanningReplicaSetMonitor, all the failedHost methods are equivalent.
     */
    void failedHost(const HostAndPort& host, const Status& status) override;

    void failedHostPreHandshake(const HostAndPort& host,
                                const Status& status,
                                BSONObj bson) override;

    void failedHostPostHandshake(const HostAndPort& host,
                                 const Status& status,
                                 BSONObj bson) override;

    bool isPrimary(const HostAndPort& host) const override;

    bool isHostUp(const HostAndPort& host) const override;

    int getMinWireVersion() const override;

    int getMaxWireVersion() const override;

    std::string getName() const override;

    std::string getServerAddress() const override;

    const MongoURI& getOriginalUri() const override;

    bool contains(const HostAndPort& server) const override;

    void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const override;

    bool isKnownToHaveGoodPrimary() const override;

    /**
     * Returns the refresh period that is given to all new SetStates.
     */
    static Seconds getDefaultRefreshPeriod();

    //
    // internal types (defined in scanning_replica_set_monitor_internal.h)
    //

    struct IsMasterReply;
    struct ScanState;
    struct SetState;
    typedef std::shared_ptr<ScanState> ScanStatePtr;
    typedef std::shared_ptr<SetState> SetStatePtr;

    /**
     * Allows tests to set initial conditions and introspect the current state.
     */
    explicit ScanningReplicaSetMonitor(const SetStatePtr& initialState);
    ~ScanningReplicaSetMonitor();

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
 * single-threaded use by ScanningReplicaSetMonitorTest.
 */
class ScanningReplicaSetMonitor::Refresher {
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
