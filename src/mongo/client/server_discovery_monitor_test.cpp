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


#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/client/sdam/sdam_configuration_parameters_gen.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener_mock.h"
#include "mongo/client/server_discovery_monitor.h"
#include "mongo/dbtests/mock/mock_replica_set.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandResponse;
using executor::ThreadPoolExecutorTest;
using InNetworkGuard = NetworkInterfaceMock::InNetworkGuard;

class ServerDiscoveryMonitorTestFixture : public unittest::Test {
protected:
    /**
     * Sets up the task executor as well as a TopologyListenerMock for each unit test.
     */
    void setUp() override {
        auto serviceContext = ServiceContext::make();
        setGlobalServiceContext(std::move(serviceContext));
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kSdam);
        ReplicaSetMonitor::cleanup();

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeSharedThreadPoolTestExecutor(std::move(network));
        _executor->startup();
        _startDate = _net->now();
        _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
        _topologyListener.reset(new sdam::TopologyListenerMock());
        _eventsPublisher->registerListener(_topologyListener);
    }

    void tearDown() override {
        _eventsPublisher.reset();
        _topologyListener.reset();
        _executor->shutdown();
        _executor->join();
        _executor.reset();
        ReplicaSetMonitor::cleanup();
        ReplicaSetMonitorProtocolTestUtil::resetRSMProtocol();
    }

    sdam::TopologyListenerMock* getTopologyListener() {
        return _topologyListener.get();
    }

    std::shared_ptr<sdam::TopologyEventsPublisher> getEventsPublisher() {
        return _eventsPublisher;
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
     * Sets up a SingleServerDiscoveryMonitor that starts sending isMasters to the server.
     */
    std::shared_ptr<SingleServerDiscoveryMonitor> initSingleServerDiscoveryMonitor(
        const sdam::SdamConfiguration& sdamConfiguration,
        const HostAndPort& hostAndPort,
        MockReplicaSet* replSet) {
        auto ssIsMasterMonitor = std::make_shared<SingleServerDiscoveryMonitor>(replSet->getURI(),
                                                                                hostAndPort,
                                                                                boost::none,
                                                                                sdamConfiguration,
                                                                                _eventsPublisher,
                                                                                _executor,
                                                                                _stats);
        ssIsMasterMonitor->init();

        // Ensure that the clock has not advanced since setUp() and _startDate is representative
        // of when the first isMaster request was sent.
        ASSERT_EQ(getStartDate(), getNet()->now());
        return ssIsMasterMonitor;
    }

    std::shared_ptr<ServerDiscoveryMonitor> initServerDiscoveryMonitor(
        const MongoURI& setUri,
        const sdam::SdamConfiguration& sdamConfiguration,
        const sdam::TopologyDescriptionPtr topologyDescription) {
        auto serverIsMasterMonitor = std::make_shared<ServerDiscoveryMonitor>(
            setUri, sdamConfiguration, _eventsPublisher, topologyDescription, _stats, _executor);

        // Ensure that the clock has not advanced since setUp() and _startDate is representative
        // of when the first isMaster request was sent.
        ASSERT_EQ(getStartDate(), getNet()->now());
        return serverIsMasterMonitor;
    }

    /**
     * Checks that an isMaster request has been sent to some server and schedules a response. If
     * assertHostCheck is true, asserts that the isMaster was sent to the server at hostAndPort.
     */
    void processIsMasterRequest(MockReplicaSet* replSet,
                                boost::optional<HostAndPort> hostAndPort = boost::none) {
        ASSERT(hasReadyRequests());
        InNetworkGuard guard(_net);
        _net->runReadyNetworkOperations();
        auto noi = _net->getNextReadyRequest();
        auto request = noi->getRequest();

        executor::TaskExecutorTest::assertRemoteCommandNameEquals("isMaster", request);
        auto requestHost = request.target.toString();
        if (hostAndPort) {
            ASSERT_EQ(request.target, hostAndPort);
        }

        LOGV2(457991,
              "Got mock network operation",
              "elapsed"_attr = elapsed(),
              "request"_attr = request.toString());

        const auto node = replSet->getNode(requestHost);
        if (node->isRunning()) {
            const auto opmsg = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
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
        LOGV2_DEBUG(457992,
                    1,
                    "Advancing time",
                    "elpasedStart"_attr = elapsed(),
                    "elapsedEnd"_attr = (elapsed() + d));
        _net->advanceTime(_net->now() + d);
        LOGV2_DEBUG(457993, 1, "Advanced time", "timeElapsed"_attr = elapsed());
    }

    /**
     * Checks that exactly one successful isMaster occurs within a time interval of
     * heartbeatFrequency.
     */
    void checkSingleIsMaster(Milliseconds heartbeatFrequency,
                             const HostAndPort& hostAndPort,
                             MockReplicaSet* replSet) {
        auto deadline = elapsed() + heartbeatFrequency;
        processIsMasterRequest(replSet, hostAndPort);

        while (elapsed() < deadline && !_topologyListener->hasIsMasterResponse(hostAndPort)) {
            advanceTime(Milliseconds(1));
        }
        validateIsMasterResponse(hostAndPort, deadline);
        checkNoActivityBefore(deadline, hostAndPort);
    }

    void validateIsMasterResponse(const HostAndPort& hostAndPort, Milliseconds deadline) {
        ASSERT_TRUE(_topologyListener->hasIsMasterResponse(hostAndPort));
        ASSERT_LT(elapsed(), deadline);
        auto isMasterResponse = _topologyListener->getIsMasterResponse(hostAndPort);

        // There should only be one isMaster response queued up.
        ASSERT_EQ(isMasterResponse.size(), 1);
        ASSERT(isMasterResponse[0].isOK());
    }

    /**
     * Confirms no more isMaster requests are sent between elapsed() and deadline. Confirms no more
     * isMaster responses are received between elapsed() and deadline when hostAndPort is specified.
     */
    void checkNoActivityBefore(Milliseconds deadline,
                               boost::optional<HostAndPort> hostAndPort = boost::none) {
        while (elapsed() < deadline) {
            ASSERT_FALSE(hasReadyRequests());
            if (hostAndPort) {
                ASSERT_FALSE(_topologyListener->hasIsMasterResponse(hostAndPort.value()));
            }
            advanceTime(Milliseconds(1));
        }
    }

    /**
     * Waits up to timeoutMS for the next isMaster request to go out.
     * Causes the test to fail if timeoutMS time passes and no request is ready.
     *
     * NOTE: The time between each isMaster request is the heartbeatFrequency compounded by response
     * time.
     */
    void waitForNextIsMaster(Milliseconds timeoutMS) {
        auto deadline = elapsed() + timeoutMS;
        while (!hasReadyRequests() && elapsed() < deadline) {
            advanceTime(Milliseconds(1));
        }

        ASSERT_LT(elapsed(), deadline);
    }

private:
    Date_t _startDate;
    std::shared_ptr<sdam::TopologyEventsPublisher> _eventsPublisher;
    std::shared_ptr<sdam::TopologyListenerMock> _topologyListener;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _net;

    std::shared_ptr<ReplicaSetMonitorManagerStats> _managerStats =
        std::make_shared<ReplicaSetMonitorManagerStats>();
    std::shared_ptr<ReplicaSetMonitorStats> _stats =
        std::make_shared<ReplicaSetMonitorStats>(_managerStats);
};

/**
 * Checks that a SingleServerDiscoveryMonitor sends isMaster requests at least heartbeatFrequency
 * apart.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, heartbeatFrequencyCheck) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    auto hostAndPort = HostAndPort(replSet->getSecondaries()[0]);

    const auto config = SdamConfiguration(std::vector<HostAndPort>{hostAndPort});
    auto ssIsMasterMonitor = initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssIsMasterMonitor->disableExpeditedChecking();

    // An isMaster command fails if it takes as long or longer than timeoutMS.
    auto timeoutMS = config.getConnectionTimeout();
    auto heartbeatFrequency = config.getHeartBeatFrequency();

    checkSingleIsMaster(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextIsMaster(timeoutMS);

    checkSingleIsMaster(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextIsMaster(timeoutMS);

    checkSingleIsMaster(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextIsMaster(timeoutMS);

    checkSingleIsMaster(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextIsMaster(timeoutMS);
}

/**
 * Confirms that a SingleServerDiscoveryMonitor reports to the TopologyListener when an isMaster
 * command generates an error.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, singleServerDiscoveryMonitorReportsFailure) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    // Kill the server before starting up the SingleServerDiscoveryMonitor.
    auto hostAndPort = HostAndPort(replSet->getSecondaries()[0]);
    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        replSet->kill(hostAndPort.toString());
    }

    const auto config = SdamConfiguration(std::vector<HostAndPort>{hostAndPort});
    auto ssIsMasterMonitor = initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssIsMasterMonitor->disableExpeditedChecking();

    processIsMasterRequest(replSet.get(), hostAndPort);
    auto topologyListener = getTopologyListener();
    auto timeoutMS = config.getConnectionTimeout();
    while (elapsed() < timeoutMS && !topologyListener->hasIsMasterResponse(hostAndPort)) {
        // Advance time in small increments to ensure we stop before another isMaster is sent.
        advanceTime(Milliseconds(1));
    }
    ASSERT_TRUE(topologyListener->hasIsMasterResponse(hostAndPort));
    auto response = topologyListener->getIsMasterResponse(hostAndPort);
    ASSERT_EQ(response.size(), 1);
    ASSERT_EQ(response[0], ErrorCodes::HostUnreachable);
}

TEST_F(ServerDiscoveryMonitorTestFixture, serverIsMasterMonitorOnTopologyDescriptionChangeAddHost) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    auto hostAndPortList = replSet->getHosts();
    auto host0 = hostAndPortList[0];
    std::vector<HostAndPort> host0Vec{host0};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(host0Vec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto isMasterMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);
    isMasterMonitor->disableExpeditedChecking();

    auto host1Delay = Milliseconds(100);
    checkSingleIsMaster(host1Delay, host0, replSet.get());
    ASSERT_FALSE(hasReadyRequests());

    // Start monitoring host1.
    auto host1 = hostAndPortList[1];
    std::vector<HostAndPort> allHostsVec{host0, host1};
    auto sdamConfigAllHosts = sdam::SdamConfiguration(allHostsVec);
    auto topologyDescriptionAllHosts =
        std::make_shared<sdam::TopologyDescription>(sdamConfigAllHosts);
    isMasterMonitor->onTopologyDescriptionChangedEvent(topologyDescription0,
                                                       topologyDescriptionAllHosts);
    // Ensure expedited checking is disabled for the SingleServerDiscoveryMonitor corresponding to
    // host1 as well.
    isMasterMonitor->disableExpeditedChecking();

    // Confirm host0 and host1 are monitored.
    auto heartbeatFrequency = sdamConfigAllHosts.getHeartBeatFrequency();
    checkSingleIsMaster(heartbeatFrequency - host1Delay, host1, replSet.get());
    waitForNextIsMaster(sdamConfigAllHosts.getConnectionTimeout());
    checkSingleIsMaster(host1Delay, host0, replSet.get());
}

TEST_F(ServerDiscoveryMonitorTestFixture,
       serverIsMasterMonitorOnTopologyDescriptionChangeRemoveHost) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    auto hostAndPortList = replSet->getHosts();
    auto host0 = hostAndPortList[0];
    auto host1 = hostAndPortList[1];
    std::vector<HostAndPort> allHostsVec{host0, host1};

    // Start up the ServerDiscoveryMonitor to monitor both hosts.
    auto sdamConfigAllHosts = sdam::SdamConfiguration(allHostsVec);
    auto topologyDescriptionAllHosts =
        std::make_shared<sdam::TopologyDescription>(sdamConfigAllHosts);
    auto uri = replSet->getURI();
    auto isMasterMonitor =
        initServerDiscoveryMonitor(uri, sdamConfigAllHosts, topologyDescriptionAllHosts);
    isMasterMonitor->disableExpeditedChecking();

    // Confirm that both hosts are monitored.
    auto heartbeatFrequency = sdamConfigAllHosts.getHeartBeatFrequency();
    while (hasReadyRequests()) {
        processIsMasterRequest(replSet.get());
    }
    auto deadline = elapsed() + heartbeatFrequency;
    auto topologyListener = getTopologyListener();
    auto hasResponses = [&]() {
        return topologyListener->hasIsMasterResponse(host0) &&
            topologyListener->hasIsMasterResponse(host1);
    };
    while (elapsed() < heartbeatFrequency && !hasResponses()) {
        advanceTime(Milliseconds(1));
    }
    validateIsMasterResponse(host0, deadline);
    validateIsMasterResponse(host1, deadline);

    // Remove host1 from the TopologyDescription to stop monitoring it.
    std::vector<HostAndPort> host0Vec{host0};
    auto sdamConfig0 = sdam::SdamConfiguration(host0Vec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    isMasterMonitor->onTopologyDescriptionChangedEvent(topologyDescriptionAllHosts,
                                                       topologyDescription0);

    checkNoActivityBefore(deadline);
    waitForNextIsMaster(sdamConfig0.getConnectionTimeout());

    checkSingleIsMaster(heartbeatFrequency, host0, replSet.get());
    waitForNextIsMaster(sdamConfig0.getConnectionTimeout());

    // Confirm the next isMaster request is sent to host0 and not host1.
    checkSingleIsMaster(heartbeatFrequency, host0, replSet.get());
}

TEST_F(ServerDiscoveryMonitorTestFixture, serverIsMasterMonitorShutdownStopsIsMasterRequests) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};
    auto sdamConfig = sdam::SdamConfiguration(hostVec);
    auto topologyDescription = std::make_shared<sdam::TopologyDescription>(sdamConfig);
    auto uri = replSet->getURI();
    auto isMasterMonitor = initServerDiscoveryMonitor(uri, sdamConfig, topologyDescription);
    isMasterMonitor->disableExpeditedChecking();

    auto heartbeatFrequency = sdamConfig.getHeartBeatFrequency();
    checkSingleIsMaster(heartbeatFrequency - Milliseconds(200), hostVec[0], replSet.get());

    isMasterMonitor->shutdown();

    // After the ServerDiscoveryMonitor shuts down, the TopologyListener may have responses until
    // heartbeatFrequency has passed, but none of them should indicate Status::OK.
    auto deadline = elapsed() + heartbeatFrequency;
    auto topologyListener = getTopologyListener();

    // Drain any requests already scheduled.
    while (elapsed() < deadline) {
        while (hasReadyRequests()) {
            processIsMasterRequest(replSet.get(), hostVec[0]);
        }
        if (topologyListener->hasIsMasterResponse(hostVec[0])) {
            auto isMasterResponses = topologyListener->getIsMasterResponse(hostVec[0]);
            for (auto& response : isMasterResponses) {
                ASSERT_FALSE(response.isOK());
            }
        }
        advanceTime(Milliseconds(1));
    }

    ASSERT_FALSE(topologyListener->hasIsMasterResponse(hostVec[0]));
}

/**
 * Tests that the ServerDiscoveryMonitor waits until SdamConfiguration::kMinHeartbeatFrequency has
 * passed since the last isMaster was received if requestImmediateCheck() is called before enough
 * time has passed.
 */
TEST_F(ServerDiscoveryMonitorTestFixture,
       serverIsMasterMonitorRequestImmediateCheckWaitMinHeartbeat) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(hostVec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto isMasterMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);

    // Ensure the server is not in expedited mode *before* requestImmediateCheck().
    isMasterMonitor->disableExpeditedChecking();

    // Check that there is only one isMaster request at time t=0 up until
    // timeAdvanceFromFirstIsMaster.
    auto minHeartbeatFrequency = SdamConfiguration::kMinHeartbeatFrequency;
    auto timeAdvanceFromFirstIsMaster = Milliseconds(10);
    ASSERT_LT(timeAdvanceFromFirstIsMaster, minHeartbeatFrequency);
    checkSingleIsMaster(timeAdvanceFromFirstIsMaster, hostVec[0], replSet.get());

    // It's been less than SdamConfiguration::kMinHeartbeatFrequency since the last isMaster was
    // received. The next isMaster should be sent SdamConfiguration::kMinHeartbeatFrequency since
    // the last isMaster was received rather than immediately.
    auto timeRequestImmediateSent = elapsed();
    isMasterMonitor->requestImmediateCheck();
    waitForNextIsMaster(minHeartbeatFrequency);

    auto timeIsMasterSent = elapsed();
    ASSERT_LT(timeRequestImmediateSent, timeIsMasterSent);
    ASSERT_LT(timeIsMasterSent, timeRequestImmediateSent + minHeartbeatFrequency);
    checkSingleIsMaster(minHeartbeatFrequency, hostVec[0], replSet.get());

    // Confirm expedited requests continue since there is no primary.
    waitForNextIsMaster(sdamConfig0.getConnectionTimeout());
    checkSingleIsMaster(minHeartbeatFrequency, hostVec[0], replSet.get());
}

/**
 * Tests that if more than SdamConfiguration::kMinHeartbeatFrequency has passed since the last
 * isMaster response was received, the ServerDiscoveryMonitor sends an isMaster immediately after
 * requestImmediateCheck() is called.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, serverIsMasterMonitorRequestImmediateCheckNoWait) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(hostVec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto isMasterMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);

    // Ensure the server is not in expedited mode *before* requestImmediateCheck().
    isMasterMonitor->disableExpeditedChecking();

    // No less than SdamConfiguration::kMinHeartbeatFrequency must pass before
    // requestImmediateCheck() is called in order to ensure the server reschedules for an immediate
    // check.
    auto minHeartbeatFrequency = SdamConfiguration::kMinHeartbeatFrequency;
    checkSingleIsMaster(minHeartbeatFrequency + Milliseconds(10), hostVec[0], replSet.get());

    isMasterMonitor->requestImmediateCheck();
    checkSingleIsMaster(minHeartbeatFrequency, hostVec[0], replSet.get());

    // Confirm expedited requests continue since there is no primary.
    waitForNextIsMaster(sdamConfig0.getConnectionTimeout());
    checkSingleIsMaster(minHeartbeatFrequency, hostVec[0], replSet.get());
}

}  // namespace
}  // namespace mongo
