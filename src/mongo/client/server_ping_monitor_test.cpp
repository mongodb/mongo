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


#include <algorithm>
#include <list>
#include <memory>
#include <ratio>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener_mock.h"
#include "mongo/client/server_ping_monitor.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

const sdam::HelloRTT initialRTT = duration_cast<Milliseconds>(Milliseconds(100));
using executor::NetworkInterfaceMock;
using executor::RemoteCommandResponse;
using executor::ThreadPoolExecutorTest;
using InNetworkGuard = NetworkInterfaceMock::InNetworkGuard;

class ServerPingMonitorTestFixture : public unittest::Test {
protected:
    /**
     * Sets up the task executor as well as a TopologyListenerMock for each unit test.
     */
    void setUp() override {
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeSharedThreadPoolTestExecutor(std::move(network));
        _executor->startup();
        _startDate = _net->now();
        _topologyListener.reset(new sdam::TopologyListenerMock());
    }

    void tearDown() override {
        _topologyListener.reset();
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }

    sdam::TopologyListenerMock* getTopologyListener() {
        return _topologyListener.get();
    }

    executor::NetworkInterfaceMock* getNet() {
        return _net;
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        return _executor;
    }

    Date_t getStartDate() {
        return _startDate;
    }

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(_net);
        return _net->hasReadyRequests();
    }

    Milliseconds elapsed() {
        return _net->now() - _startDate;
    }

    /**
     * Checks that a ping has been made to the server at hostAndPort and schedules a response.
     */
    void processPingRequest(const HostAndPort& hostAndPort, MockReplicaSet* replSet) {
        ASSERT(hasReadyRequests());

        InNetworkGuard guard(_net);
        _net->runReadyNetworkOperations();
        auto noi = _net->getNextReadyRequest();
        auto request = noi->getRequest();

        // Check that it is a ping request from the expected hostAndPort.
        executor::TaskExecutorTest::assertRemoteCommandNameEquals("ping", request);
        ASSERT_EQ(request.target, hostAndPort);
        LOGV2(23925,
              "Got mock network operation",
              "elapsed"_attr = elapsed(),
              "request"_attr = request.toString());

        const auto node = replSet->getNode(hostAndPort.toString());
        node->setCommandReply("ping", BSON("ok" << 1));

        if (node->isRunning()) {
            const auto opmsg = static_cast<OpMsgRequest>(request);
            const auto reply = node->runCommand(request.id, opmsg)->getCommandReply();
            _net->scheduleSuccessfulResponse(noi, RemoteCommandResponse(reply, Milliseconds(0)));
        } else {
            _net->scheduleErrorResponse(noi, Status(ErrorCodes::HostUnreachable, ""));
        }
    }

    template <typename Duration>
    void advanceTime(Duration d) {
        InNetworkGuard guard(_net);

        // Operations can happen inline with advanceTime(), so log before and after the call.
        LOGV2_DEBUG(23926,
                    1,
                    "About to advance time",
                    "elapsedStart"_attr = elapsed(),
                    "elapsedEnd"_attr = (elapsed() + d));
        _net->advanceTime(_net->now() + d);
        LOGV2_DEBUG(23927, 1, "Advanced time", "elapsed"_attr = elapsed());
    }

    /**
     * Checks that exactly one successful ping occurs at the time the method is called and ensures
     * no additional pings are issued for at least pingFrequency.
     */
    void checkSinglePing(Milliseconds pingFrequency,
                         const HostAndPort& hostAndPort,
                         MockReplicaSet* replSet) {
        processPingRequest(hostAndPort, replSet);
        auto deadline = elapsed() + pingFrequency;
        while (elapsed() < deadline && !_topologyListener->hasPingResponse(hostAndPort)) {
            advanceTime(Milliseconds(100));
        }

        ASSERT_TRUE(_topologyListener->hasPingResponse(hostAndPort));
        ASSERT_LT(elapsed(), deadline);
        auto pingResponse = _topologyListener->getPingResponse(hostAndPort);

        // There should only be one "hello" response queued up.
        ASSERT_EQ(pingResponse.size(), 1);
        ASSERT(pingResponse[0].isOK());

        // The latency is from the ping monitor's local timer; not from the mocked clock.
        // Just assert that we receive a signal.
        ASSERT_GTE(durationCount<Microseconds>(pingResponse[0].getValue()), 1);

        checkNoActivityBefore(deadline, hostAndPort);
    }

    /**
     * Confirms no more ping requests are sent between elapsed() and deadline. Confirms no more ping
     * responses are received between elapsed() and deadline.
     */
    void checkNoActivityBefore(Milliseconds deadline, const HostAndPort& hostAndPort) {
        while (elapsed() < deadline) {
            ASSERT_FALSE(hasReadyRequests());
            ASSERT_FALSE(_topologyListener->hasPingResponse(hostAndPort));
            advanceTime(Milliseconds(100));
        }
    }

    /**
     * Since the SingleServerPingMonitor is removed upon an onTopologyDescriptionChangedEvent,
     * prompt the event with a new TopologyDescription that does not include hostToDrop.
     */
    void closeMonitor(MockReplicaSet* replSet,
                      HostAndPort hostToDrop,
                      ServerPingMonitor* pingMonitor) {
        auto hosts = replSet->getHosts();
        auto sdamConfigOld = sdam::SdamConfiguration(hosts);
        auto topologyDescriptionOld = std::make_shared<sdam::TopologyDescription>(sdamConfigOld);

        std::vector<HostAndPort> hostsNew(hosts.begin(), hosts.end());
        hostsNew.erase(std::remove_if(hostsNew.begin(),
                                      hostsNew.end(),
                                      [&](auto host) { return host == hostToDrop; }),
                       hostsNew.end());
        // Since the seedlist cannot be empty, the new TopologyDescription contains an empty
        // HostAndPort.
        if (hostsNew.size() == 0) {
            hostsNew.emplace_back(HostAndPort());
        }
        auto sdamConfigNew = sdam::SdamConfiguration(hostsNew);
        auto topologyDescriptionNew = std::make_shared<sdam::TopologyDescription>(sdamConfigNew);
        pingMonitor->onTopologyDescriptionChangedEvent(topologyDescriptionOld,
                                                       topologyDescriptionNew);
    }

private:
    Date_t _startDate;
    std::unique_ptr<sdam::TopologyListenerMock> _topologyListener;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _net;
};

class SingleServerPingMonitorTest : public ServerPingMonitorTestFixture {
protected:
    void setUp() {
        ServerPingMonitorTestFixture::setUp();
        _replSet.reset(new MockReplicaSet(
            "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false));
        _hostAndPort = HostAndPort(_replSet->getSecondaries()[0]);
    }

    void tearDown() {
        _replSet.reset();
        ServerPingMonitorTestFixture::tearDown();
    }

    MockReplicaSet* getReplSet() {
        return _replSet.get();
    }

    HostAndPort getHostAndPort() {
        return _hostAndPort;
    }

    void processPingRequest() {
        ServerPingMonitorTestFixture::processPingRequest(_hostAndPort, getReplSet());
    }

    /**
     * Sets up a SingleServerPingMonitor that starts pinging the server.
     */
    std::shared_ptr<SingleServerPingMonitor> initSingleServerPingMonitor(Seconds pingFrequency) {
        auto ssPingMonitor = std::make_shared<SingleServerPingMonitor>(getReplSet()->getURI(),
                                                                       _hostAndPort,
                                                                       getTopologyListener(),
                                                                       pingFrequency,
                                                                       getExecutor());
        ssPingMonitor->init();

        // Ensure that the clock has not advanced since setUp() and _startDate is representative
        // of when the first ping request was sent.
        ASSERT_EQ(getStartDate(), getNet()->now());
        return ssPingMonitor;
    }

    void checkSinglePing(Milliseconds pingFrequency) {
        ServerPingMonitorTestFixture::checkSinglePing(pingFrequency, _hostAndPort, getReplSet());
    }

    void checkNoActivityBefore(Milliseconds deadline) {
        ServerPingMonitorTestFixture::checkNoActivityBefore(deadline, _hostAndPort);
    }

private:
    std::unique_ptr<MockReplicaSet> _replSet;

    /**
     * Stores the HostAndPort of the node ping requests are sent to.
     */
    HostAndPort _hostAndPort;
};

TEST_F(SingleServerPingMonitorTest, pingFrequencyCheck) {
    auto pingFrequency = Seconds(10);
    auto ssPingMonitor = initSingleServerPingMonitor(pingFrequency);

    checkSinglePing(pingFrequency);
    checkSinglePing(pingFrequency);
    checkSinglePing(pingFrequency);
    checkSinglePing(pingFrequency);
}

/**
 * Confirms that the SingleServerPingMonitor continues to try and ping a dead server at
 * pingFrequency and successfully does so once the server is restored.
 */
TEST_F(SingleServerPingMonitorTest, pingDeadServer) {
    // Kill the server before starting up the SingleServerPingMonitor.
    auto hostAndPort = getHostAndPort();
    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        getReplSet()->kill(hostAndPort.toString());
    }

    auto pingFrequency = Seconds(10);
    auto ssPingMonitor = initSingleServerPingMonitor(pingFrequency);
    auto topologyListener = getTopologyListener();
    auto checkSinglePingDeadServer = [&]() {
        Milliseconds deadline = elapsed() + pingFrequency;
        processPingRequest();

        while (elapsed() < deadline && !topologyListener->hasPingResponse(hostAndPort)) {
            advanceTime(Milliseconds(100));
        }

        ASSERT_TRUE(topologyListener->hasPingResponse(hostAndPort));
        auto pingResponse = topologyListener->getPingResponse(hostAndPort);
        ASSERT_EQ(pingResponse.size(), 1);
        ASSERT_EQ(ErrorCodes::HostUnreachable, pingResponse[0].getStatus());

        checkNoActivityBefore(deadline);
    };
    checkSinglePingDeadServer();
    checkSinglePingDeadServer();
    {

        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        getReplSet()->restore(hostAndPort.toString());
    }
    checkSinglePing(pingFrequency);
    checkSinglePing(pingFrequency);
}

/**
 * Checks that no more events are published to the TopologyListener and no more pings are issued to
 * the server after the SingleServerPingMonitor is closed.
 */
TEST_F(SingleServerPingMonitorTest, noPingAfterSingleServerPingMonitorClosed) {
    auto pingFrequency = Seconds(10);
    auto ssPingMonitor = initSingleServerPingMonitor(pingFrequency);

    // Drop the SingleServerMonitor before the second ping is sent.
    checkSinglePing(pingFrequency - Seconds(2));
    ssPingMonitor->drop();

    checkNoActivityBefore(pingFrequency * 3);
}

class ServerPingMonitorTest : public ServerPingMonitorTestFixture {
protected:
    std::unique_ptr<ServerPingMonitor> makeServerPingMonitor(const MongoURI& setUri,
                                                             Seconds pingFrequency) {
        return std::make_unique<ServerPingMonitor>(
            setUri, getTopologyListener(), pingFrequency, getExecutor());
    }
};

/**
 * Adds and removes a SingleServerPingMonitor from the ServerPingMonitor.
 */
TEST_F(ServerPingMonitorTest, singleNodeServerPingMonitorCycle) {
    auto pingFrequency = Seconds(10);
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    auto serverPingMonitor = makeServerPingMonitor(replSet->getURI(), pingFrequency);
    auto hostAndPort = HostAndPort(replSet->getSecondaries()[0]);

    // Add a SingleServerPingMonitor to the ServerPingMonitor. Confirm pings are sent to the server
    // at pingFrequency.
    serverPingMonitor->onServerHandshakeCompleteEvent(initialRTT, hostAndPort);
    checkSinglePing(pingFrequency, hostAndPort, replSet.get());
    checkSinglePing(pingFrequency - Seconds(2), hostAndPort, replSet.get());

    // Close the SingleServerMonitor before the third ping and confirm ping activity to the server
    // is stopped.
    closeMonitor(replSet.get(), hostAndPort, serverPingMonitor.get());
    checkNoActivityBefore(elapsed() + pingFrequency * 2, hostAndPort);
}

/**
 * Adds two SingleServerPingMonitors to the ServerPingMonitor, removes one SingleServerPingMonitor
 * but not the other.
 */
TEST_F(ServerPingMonitorTest, twoNodeServerPingMonitorOneClosed) {
    auto pingFrequency = Seconds(10);
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    auto serverPingMonitor = makeServerPingMonitor(replSet->getURI(), pingFrequency);

    auto hosts = replSet->getHosts();
    auto host0 = hosts[0];
    auto host1 = hosts[1];

    // Add SingleServerPingMonitors for host0 and host1 where host1 is added host1Delay seconds
    // after host0.
    auto host1Delay = Seconds(2);
    serverPingMonitor->onServerHandshakeCompleteEvent(initialRTT, host0);
    checkSinglePing(host1Delay, host0, replSet.get());
    ASSERT_EQ(elapsed(), host1Delay);
    serverPingMonitor->onServerHandshakeCompleteEvent(initialRTT, host1);
    checkSinglePing(pingFrequency - Seconds(2), host1, replSet.get());

    closeMonitor(replSet.get(), host0, serverPingMonitor.get());
    checkNoActivityBefore(pingFrequency + host1Delay, host0);

    // Confirm that host1's SingleServerPingMonitor continues ping activity.
    checkSinglePing(pingFrequency, host1, replSet.get());
}

/**
 * Starts up a ServerPingMonitor monitoring two servers. Confirms ServerPingMonitor::shutdown()
 * is safe to call multiple times - once explicitly and a second time implicitly through its
 * destructor.
 */
TEST_F(ServerPingMonitorTest, twoNodeServerPingMonitorMutlipleShutdown) {
    auto pingFrequency = Seconds(10);
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    auto serverPingMonitor = makeServerPingMonitor(replSet->getURI(), pingFrequency);

    auto hosts = replSet->getHosts();
    auto host0 = hosts[0];
    auto host1 = hosts[1];

    // Add SingleServerPingMonitors for host0 and host1 where host1 is added host1Delay seconds
    // after host0.
    auto host1Delay = Seconds(2);
    serverPingMonitor->onServerHandshakeCompleteEvent(initialRTT, host0);
    checkSinglePing(host1Delay, host0, replSet.get());
    ASSERT_EQ(elapsed(), host1Delay);
    serverPingMonitor->onServerHandshakeCompleteEvent(initialRTT, host1);
    checkSinglePing(pingFrequency - Seconds(2), host1, replSet.get());

    serverPingMonitor->shutdown();

    // Invoke the second shutdown via the ServerPingMonitor destructor.
    serverPingMonitor.reset();

    // Since ServerPingMonitor::shutdown() shuts down the executor, the clock can no longer be
    // advanced. Still, confirm the TopologyListener stopped receiving ping activity.
    auto topologyListener = getTopologyListener();
    ASSERT_FALSE(topologyListener->hasPingResponse(host0));
    ASSERT_FALSE(topologyListener->hasPingResponse(host1));
}
}  // namespace
}  // namespace mongo
