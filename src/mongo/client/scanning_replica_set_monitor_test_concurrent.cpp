/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/scanning_replica_set_monitor_internal.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandResponse;
using executor::ThreadPoolExecutorTest;
using InNetworkGuard = NetworkInterfaceMock::InNetworkGuard;
using NetworkOperationIterator = NetworkInterfaceMock::NetworkOperationIterator;
using StepKind = ScanningReplicaSetMonitor::Refresher::NextStep::StepKind;

class ScanningReplicaSetMonitorConcurrentTest : public ThreadPoolExecutorTest {
protected:
    void setUp() {
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kScanning);
        auto serviceContext = ServiceContext::make();
        ThreadPoolExecutorTest::setUp();
        launchExecutorThread();
        _startTime = getNet()->now();
        setGlobalServiceContext(std::move(serviceContext));
    }

    void tearDown() {
        shutdownExecutorThread();
        joinExecutorThread();
        ReplicaSetMonitor::cleanup();
        ThreadPoolExecutorTest::tearDown();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
    }

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        return getNet()->hasReadyRequests();
    }

    Milliseconds elapsed() {
        return getNet()->now() - _startTime;
    }

    long long elapsedMS() {
        return durationCount<Milliseconds>(elapsed());
    }

    void processReadyRequests(MockReplicaSet& replSet) {
        while (hasReadyRequests()) {
            InNetworkGuard guard(getNet());
            auto noi = getNet()->getNextReadyRequest();
            respondToCommand(guard, replSet, noi);
        }
    }

    // Provide the MockReplicaSet's default response to a command such as "isMaster". Pass an
    // InNetworkGuard to prove you have called NetworkInterfaceMock::enterNetwork().
    void respondToCommand(const InNetworkGuard& guard,
                          MockReplicaSet& replSet,
                          NetworkOperationIterator& noi) {
        const auto net = getNet();
        const auto request = noi->getRequest();
        _numChecks[request.target]++;
        LOGV2_DEBUG(20192,
                    2,
                    "at {elapsed} got mock net operation {request}",
                    "elapsed"_attr = elapsed(),
                    "request"_attr = request.toString());
        const auto node = replSet.getNode(request.target.toString());
        if (node->isRunning()) {
            const auto opmsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
            const auto reply = node->runCommand(request.id, opmsg)->getCommandReply();
            LOGV2_DEBUG(20193, 2, "replying {reply}", "reply"_attr = reply);
            net->scheduleSuccessfulResponse(noi, RemoteCommandResponse(reply, Milliseconds(0)));
        } else {
            LOGV2_DEBUG(20194, 2, "black hole");
            net->blackHole(noi);
        }
        net->runReadyNetworkOperations();
    }

    template <typename Duration>
    void advanceTime(Duration d) {
        const auto net = getNet();
        InNetworkGuard guard(net);

        // Operations can happen inline with advanceTime(), so log before and after the call.
        LOGV2_DEBUG(20195,
                    3,
                    "Advancing time from {elapsed} to {elapsed_d}",
                    "elapsed"_attr = elapsed(),
                    "elapsed_d"_attr = (elapsed() + d));
        net->advanceTime(net->now() + d);
        LOGV2_DEBUG(20196, 3, "Advanced to {elapsed}", "elapsed"_attr = elapsed());
    }

    int getNumChecks(HostAndPort host) {
        return _numChecks[host];
    }

    const ReadPreferenceSetting primaryOnly =
        ReadPreferenceSetting(mongo::ReadPreference::PrimaryOnly, TagSet());
    const ReadPreferenceSetting secondaryOnly =
        ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly, TagSet());
    const ReadPreferenceSetting secondaryWithTags =
        ReadPreferenceSetting(mongo::ReadPreference::SecondaryOnly,
                              TagSet(BSON_ARRAY(BSON("nonexistent"
                                                     << "tag"))));
    ReplicaSetChangeNotifier& getNotifier() {
        return _notifier;
    }

private:
    unittest::MinimumLoggedSeverityGuard _networkSeverityGuard{logv2::LogComponent::kNetwork,
                                                               logv2::LogSeverity::Debug(2)};

    std::map<HostAndPort, int> _numChecks;
    Date_t _startTime;

    ReplicaSetChangeNotifier _notifier;
};

// If one node is unresponsive and the available node doesn't satisfy our read preference, the
// replica set monitor should re-check the available node every kExpeditedRefreshPeriod = 500ms
// until expiration.
//
// 1. Create a replica set with two secondaries, Node 0 and Node 1
// 2. Begin two ReplicaSetMonitor::getHostOrRefresh calls with primaryOnly and secondaryOnly prefs
// 3. Node 0 responds but Node 1 does not
// 4. Assert the ReplicaSetMonitor rechecks Node 0 periodically while waiting for Node 1
// 6. At 2 seconds, Node 0 becomes primary
// 7. At 2.5 seconds the monitor rechecks Node 0 and discovers it's primary. Assert that
//    getHostOrRefresh(primaryOnly) succeeds.
// 8. At 5 seconds the Node 1 check times out, assert getHostOrRefresh(secondaryOnly) fails
#if 0
TEST_F(ScanningReplicaSetMonitorConcurrentTest, RechecksAvailableNodesUntilExpiration) {
    MockReplicaSet replSet("test", 2, false /* hasPrimary */, false /* dollarPrefixHosts */);
    const auto node0 = HostAndPort(replSet.getSecondaries()[0]);
    const auto node1 = HostAndPort(replSet.getSecondaries()[1]);
    auto state = std::make_shared<ScanningReplicaSetMonitor::SetState>(
        replSet.getURI(), &getNotifier(), &getExecutor());
    auto monitor = std::make_shared<ScanningReplicaSetMonitor>(state);

    // Node 1 is unresponsive.
    replSet.kill(replSet.getSecondaries()[1]);
    monitor->init();

    // ReplicaSetMonitor deliberately completes a future when a primary is discovered or the scan
    // ends, even if the future expires sooner. Thus timeouts don't matter so long as they're less
    // than the scan's duration, which is socketTimeoutSecs = 5 seconds.
    const auto primaryFuture = monitor->getHostOrRefresh(primaryOnly, Seconds(4));
    const auto secondaryFuture = monitor->getHostOrRefresh(secondaryOnly, Milliseconds(4800));

    // Monitor rechecks Node 0 every 500ms, Node 1 times out after 5 seconds.
    while (elapsed() <= Seconds(20)) {
        processReadyRequests(replSet);

        // After 2 seconds, make Node 0 primary.
        if (elapsed() == Milliseconds(2100)) {
            replSet.setPrimary(node0.toString());
        }

        if (elapsed() < Milliseconds(2500)) {
            ASSERT(!primaryFuture.isReady());
        }

        // The monitor discovers Node 0 is primary, getHostOrRefresh(primaryOnly) succeeds.
        if (elapsed() == Milliseconds(2500)) {
            ASSERT(primaryFuture.isReady());
            ASSERT_EQ(primaryFuture.get(), node0);
        }

        // After 5 seconds, getHostOrRefresh(secondaryOnly) receives its timeout.
        if (elapsed() < Milliseconds(5000)) {
            ASSERT(!secondaryFuture.isReady());
            ASSERT_EQ(getNumChecks(node0), 1 + (elapsedMS() / 500));
            ASSERT_EQ(getNumChecks(node1), 1);
        } else {
            ASSERT(secondaryFuture.isReady());
            ASSERT_NOT_OK(secondaryFuture.getNoThrow());
        }

        // Once Node 1 times out, monitoring slows to the usual 30-second interval.
        if (elapsed() >= Milliseconds(5000)) {
            ASSERT_EQ(getNumChecks(node0), 11);
            ASSERT_EQ(getNumChecks(node1), 1);
        }

        advanceTime(Milliseconds(100));
    }
}
#endif  // 0

// Like previous test, but simulate a stepdown and election while waiting for unresponsive node.
//
// 1. Create a replica set with three secondaries, Node 0, Node 1, and Node 2
// 2. Begin two ReplicaSetMonitor::getHostOrRefresh calls with primaryOnly and secondaryOnly prefs
// 3. Node 0 and 1 respond as secondaries, Node 2 does not respond
// 4. After 1 second, Node 0 becomes primary
// 5. After 2 seconds, Node 0 steps down
// 6. After 3 seconds, Node 1 becomes primary
TEST_F(ScanningReplicaSetMonitorConcurrentTest, StepdownAndElection) {
    MockReplicaSet replSet("test", 3, false /* hasPrimary */, false /* dollarPrefixHosts */);
    const auto node0 = HostAndPort(replSet.getSecondaries()[0]);
    const auto node1 = HostAndPort(replSet.getSecondaries()[1]);
    const auto node2 = HostAndPort(replSet.getSecondaries()[2]);
    auto state = std::make_shared<ScanningReplicaSetMonitor::SetState>(
        replSet.getURI(), &getNotifier(), &getExecutor());
    auto monitor = std::make_shared<ScanningReplicaSetMonitor>(state);

    // Node 2 is unresponsive.
    replSet.kill(replSet.getSecondaries()[2]);
    monitor->init();

    auto primaryFuture = monitor->getHostOrRefresh(primaryOnly, Seconds(4));
    auto unsatisfiableFuture = monitor->getHostOrRefresh(secondaryWithTags, Milliseconds(4250));

    // Receive first isMasters in any order. Nodes 0 and 1 respond, Node 2 doesn't.
    // Then monitor rechecks Node 0 and 1 every 500ms, Node 2 times out after 5 seconds.
    while (elapsed() <= Seconds(5)) {
        processReadyRequests(replSet);

        // After 1 second, make Node 0 primary.
        if (elapsed() == Milliseconds(1100)) {
            replSet.setPrimary(node0.toString());
        }

        if (elapsed() < Milliseconds(1500)) {
            ASSERT(!primaryFuture.isReady());
        }

        // The monitor discovers Node 0 is primary, getHostOrRefresh(primaryOnly) succeeds.
        if (elapsed() == Milliseconds(1500)) {
            ASSERT(primaryFuture.isReady());
            ASSERT_EQ(primaryFuture.get(), node0);
        }

        // After 2 seconds, Node 0 steps down.
        if (elapsed() == Milliseconds(2100)) {
            replSet.setPrimary("");
        }

        // The monitor discovers Node 0 is secondary.
        if (elapsed() == Milliseconds(2500)) {
            primaryFuture = monitor->getHostOrRefresh(primaryOnly, Seconds(4));
            ASSERT(!primaryFuture.isReady());
        }

        // After 3 seconds, make Node 1 primary.
        if (elapsed() == Milliseconds(3100)) {
            replSet.setPrimary(node1.toString());
        }

        // The monitor discovers Node 1 is primary.
        if (elapsed() == Milliseconds(3500)) {
            ASSERT(primaryFuture.isReady());
            ASSERT_EQ(primaryFuture.get(), node1);
        }

        // At 4.5 seconds, getHostOrRefresh(secondaryWithTags) receives its timeout.
        if (elapsed() < Milliseconds(4500)) {
            ASSERT(!unsatisfiableFuture.isReady());
        } else {
            ASSERT(unsatisfiableFuture.isReady());
            ASSERT_NOT_OK(unsatisfiableFuture.getNoThrow());
        }

        if (elapsed() < Milliseconds(5000)) {
            // New getHostOrRefresh calls can trigger checks faster than every 500ms, so "+ 2".
            ASSERT_LTE(getNumChecks(node0), elapsedMS() / 500 + 2);
            ASSERT_LTE(getNumChecks(node1), elapsedMS() / 500 + 2);
            ASSERT_EQ(getNumChecks(node2), 1);
        }

        advanceTime(Milliseconds(100));
    }
}

// Check that isMaster is being called at most every 500ms.
//
// 1. Create a replica set with two secondaries, Node 0 and Node 1
// 2. Begin a ReplicaSetMonitor::getHostOrRefresh call with primaryOnly
// 3. Node 0 responds but Node 1 does not
// 4. After 0.5s call ReplicaSetMonitor::getHostOrRefresh again
TEST_F(ScanningReplicaSetMonitorConcurrentTest, IsMasterFrequency) {
    MockReplicaSet replSet("test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    const auto node0 = HostAndPort(replSet.getSecondaries()[0]);
    const auto node1 = HostAndPort(replSet.getSecondaries()[1]);

    auto state = std::make_shared<ScanningReplicaSetMonitor::SetState>(
        replSet.getURI(), &getNotifier(), &getExecutor());
    auto monitor = std::make_shared<ScanningReplicaSetMonitor>(state);

    // Node 1 is unresponsive.
    replSet.kill(replSet.getSecondaries()[1]);
    monitor->init();

    std::vector<SemiFuture<HostAndPort>> primaryFutures;
    primaryFutures.push_back(monitor->getHostOrRefresh(primaryOnly, Seconds(4)));

    // Because Node 1 is unresponsive, the monitor rechecks Node 0 every 500ms
    // until Node 1 times out after 5 seconds.
    auto checkUntil = [&](auto timeToStop, auto func) {
        while (elapsed() < timeToStop) {
            processReadyRequests(replSet);
            func();
            // all primaryFutures are not ready, because the monitor cannot find the primary
            for (SemiFuture<HostAndPort>& future : primaryFutures) {
                ASSERT(!future.isReady());
            }
            advanceTime(Milliseconds(100));
        }
    };

    // Up until 500ms there is only one isMaster call.
    // At 500ms, the monitor rechecks node 0.
    checkUntil(Milliseconds(500), [&]() {
        ASSERT_EQ(getNumChecks(node0), 1);
        ASSERT_EQ(getNumChecks(node1), 1);
    });

    // Triggers isMaster calls that will be delayed until 1000ms
    checkUntil(Milliseconds(1000), [&]() {
        // this should schedule a new isMaster call at 1000ms and cancel the
        // previous job scheduled for the same time
        primaryFutures.push_back(monitor->getHostOrRefresh(primaryOnly, Seconds(4)));
        ASSERT_EQ(getNumChecks(node0), 2);
        ASSERT_EQ(getNumChecks(node1), 1);
    });

    // At 1000ms, only one scheduled isMaster call runs, because all others have been canceled
    checkUntil(Milliseconds(1500), [&]() {
        ASSERT_EQ(getNumChecks(node0), 3);
        ASSERT_EQ(getNumChecks(node1), 1);
    });

    advanceTime(Seconds(5));
    processReadyRequests(replSet);

    for (auto& future : primaryFutures) {
        ASSERT(future.isReady());
    }
}

// Check that requests actually experience timeout despite in-flight isMasters
TEST_F(ScanningReplicaSetMonitorConcurrentTest, RecheckUntilTimeout) {
    MockReplicaSet replSet("test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    const auto node0 = HostAndPort(replSet.getSecondaries()[0]);
    const auto node1 = HostAndPort(replSet.getSecondaries()[1]);

    auto state = std::make_shared<ScanningReplicaSetMonitor::SetState>(
        replSet.getURI(), &getNotifier(), &getExecutor());
    auto monitor = std::make_shared<ScanningReplicaSetMonitor>(state);

    // Node 1 is unresponsive.
    replSet.kill(replSet.getSecondaries()[1]);
    monitor->init();

    // Set the timeout to be more than 500ms before the third timeout on node 1. This way the
    // isMaster to node 1 is in flight and the timeout occurs at 14500 when the isMaster to node
    // 0 succeeds.
    auto hostFuture = monitor->getHostOrRefresh(primaryOnly, Milliseconds(14250));

    auto checkUntil = [&](auto timeToStop, auto func) {
        while (elapsed() < timeToStop) {
            processReadyRequests(replSet);
            func();
            advanceTime(Milliseconds(100));
        }

        processReadyRequests(replSet);
    };

    // Every 500ms, the monitor rechecks node 0 after the previous successful isMaster.
    // Every 5s, the monitor rechecks node 1 after the previous isMaster experiences timeout.
    constexpr auto kTimeoutPeriodMS =
        Milliseconds(ScanningReplicaSetMonitor::kCheckTimeout).count() +
        ScanningReplicaSetMonitor::kExpeditedRefreshPeriod.count();
    checkUntil(Milliseconds(14500), [&]() {
        ASSERT_EQ(getNumChecks(node0),
                  elapsedMS() / ScanningReplicaSetMonitor::kExpeditedRefreshPeriod.count() + 1);
        ASSERT_EQ(getNumChecks(node1), elapsedMS() / kTimeoutPeriodMS + 1);
        ASSERT(!hostFuture.isReady());
    });


    ASSERT(hostFuture.isReady());
}
}  // namespace
}  // namespace mongo
