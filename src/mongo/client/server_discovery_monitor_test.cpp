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


#include "mongo/client/server_discovery_monitor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_protocol_test_util.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_listener_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
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
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::sdam {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandResponse;
using InNetworkGuard = NetworkInterfaceMock::InNetworkGuard;

BSONObj makeHelloResponseWithTopologyVersion() {
    return BSON("ok" << 1.0 << "isWritablePrimary" << false << "secondary" << true
                     << "topologyVersion"
                     << BSON("processId" << OID::gen() << "counter" << static_cast<long long>(0)));
}

class ServerDiscoveryMonitorTestFixture : public unittest::Test {
protected:
    /**
     * Sets up the task executor as well as a TopologyListenerMock for each unit test.
     */
    void setUp() override {
        auto serviceContext = ServiceContext::make();
        WireSpec::getWireSpec(serviceContext.get()).initialize(WireSpec::Specification{});
        setGlobalServiceContext(std::move(serviceContext));
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(ReplicaSetMonitorProtocol::kSdam);
        ReplicaSetMonitor::cleanup();

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeThreadPoolTestExecutor(std::move(network));
        _executor->startup();
        _startDate = _net->now();
        _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
        _topologyListener = std::make_shared<sdam::TopologyListenerMock>();
        _eventsPublisher->registerListener(_topologyListener);
    }

    void tearDown() override {
        _eventsPublisher.reset();
        _topologyListener.reset();
        _executor->shutdown();
        executor::NetworkInterfaceMock::InNetworkGuard(_net)->runReadyNetworkOperations();
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
     * Sets up a SingleServerDiscoveryMonitor that starts sending "hello" to the server.
     */
    std::shared_ptr<SingleServerDiscoveryMonitor> initSingleServerDiscoveryMonitor(
        const sdam::SdamConfiguration& sdamConfiguration,
        const HostAndPort& hostAndPort,
        MockReplicaSet* replSet) {
        auto ssHelloMonitor = std::make_shared<SingleServerDiscoveryMonitor>(replSet->getURI(),
                                                                             hostAndPort,
                                                                             boost::none,
                                                                             sdamConfiguration,
                                                                             _eventsPublisher,
                                                                             _executor,
                                                                             _stats);
        ssHelloMonitor->init();

        // Ensure that the clock has not advanced since setUp() and _startDate is representative of
        // when the first "hello" request was sent.
        ASSERT_EQ(getStartDate(), getNet()->now());
        return ssHelloMonitor;
    }

    std::shared_ptr<ServerDiscoveryMonitor> initServerDiscoveryMonitor(
        const MongoURI& setUri,
        const sdam::SdamConfiguration& sdamConfiguration,
        const sdam::TopologyDescriptionPtr topologyDescription) {
        auto serverHelloMonitor = std::make_shared<ServerDiscoveryMonitor>(
            setUri, sdamConfiguration, _eventsPublisher, topologyDescription, _stats, _executor);

        // Ensure that the clock has not advanced since setUp() and _startDate is representative
        // of when the first "hello" request was sent.
        ASSERT_EQ(getStartDate(), getNet()->now());
        return serverHelloMonitor;
    }

    /**
     * Checks that an "hello" request has been sent to some server and schedules a response. If
     * assertHostCheck is true, asserts that the "hello" was sent to the server at hostAndPort.
     */
    void processHelloRequest(MockReplicaSet* replSet,
                             boost::optional<HostAndPort> hostAndPort = boost::none) {
        ASSERT(hasReadyRequests());
        InNetworkGuard guard(_net);
        _net->runReadyNetworkOperations();
        auto noi = _net->getNextReadyRequest();
        auto request = noi->getRequest();

        executor::TaskExecutorTest::assertRemoteCommandNameEquals("hello", request);
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
            const auto opmsg = OpMsgRequestBuilder::create(
                auth::ValidatedTenancyScope::kNotRequired, request.dbname, request.cmdObj);
            const auto reply = node->runCommand(request.id, opmsg)->getCommandReply();
            _net->scheduleSuccessfulResponse(
                noi, RemoteCommandResponse::make_forTest(reply, Milliseconds(0)));
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
     * Checks that exactly one successful "hello" occurs within a time interval of
     * heartbeatFrequency.
     */
    void checkSingleHello(Milliseconds heartbeatFrequency,
                          const HostAndPort& hostAndPort,
                          MockReplicaSet* replSet) {
        auto deadline = elapsed() + heartbeatFrequency;
        processHelloRequest(replSet, hostAndPort);

        while (elapsed() < deadline && !_topologyListener->hasHelloResponse(hostAndPort)) {
            advanceTime(Milliseconds(1));
        }
        validateHelloResponse(hostAndPort, deadline);
        checkNoActivityBefore(deadline, hostAndPort);
    }

    void validateHelloResponse(const HostAndPort& hostAndPort, Milliseconds deadline) {
        ASSERT_TRUE(_topologyListener->hasHelloResponse(hostAndPort));
        ASSERT_LT(elapsed(), deadline);
        const auto helloResponse = _topologyListener->getHelloResponse(hostAndPort);

        ASSERT_EQ(helloResponse.size(), 1);
        ASSERT(helloResponse[0].isOK());
    }

    /**
     * Confirms no more "hello" requests are sent between elapsed() and deadline. Confirms no more
     * "hello" responses are received between elapsed() and deadline when hostAndPort is specified.
     */
    void checkNoActivityBefore(Milliseconds deadline,
                               boost::optional<HostAndPort> hostAndPort = boost::none) {
        while (elapsed() < deadline) {
            ASSERT_FALSE(hasReadyRequests());
            if (hostAndPort) {
                ASSERT_FALSE(_topologyListener->hasHelloResponse(hostAndPort.value()));
            }
            advanceTime(Milliseconds(1));
        }
    }

    /**
     * Waits up to timeoutMS for the next "hello" request to go out. Causes the test to fail if
     * timeoutMS time passes and no request is ready.
     *
     * NOTE: The time between each "hello" request is the heartbeatFrequency compounded by response
     * time.
     */
    void waitForNextHello(Milliseconds timeoutMS) {
        const auto deadline = elapsed() + timeoutMS;
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
 * Parameterized test fixture that runs tests with both kSdam and kStreamable protocols. For
 * kStreamable, tests exercise the ServerDiscoveryMonitor::_scheduleStreamableHello code path by
 * first sending a successful hello with topologyVersion to prime the monitor.
 */
class SingleServerDiscoveryMonitorProtocolTestFixture
    : public ServerDiscoveryMonitorTestFixture,
      public testing::WithParamInterface<ReplicaSetMonitorProtocol> {
protected:
    void setUp() override {
        ServerDiscoveryMonitorTestFixture::setUp();
        ReplicaSetMonitorProtocolTestUtil::setRSMProtocol(GetParam());
    }

    bool isStreamableProtocol() const {
        return GetParam() == ReplicaSetMonitorProtocol::kStreamable;
    }

    /**
     * For kStreamable protocol, sends an initial hello that returns a topologyVersion,
     * which primes the monitor to use _scheduleStreamableHello for subsequent requests.
     * For kSdam, this is a no-op.
     */
    void primeForStreamableIfNeeded(MockReplicaSet* replSet,
                                    const HostAndPort& hostAndPort,
                                    const SdamConfiguration& config) {
        if (!isStreamableProtocol()) {
            return;
        }

        // Process the first hello to establish topologyVersion
        processHelloRequest(replSet, hostAndPort);
        auto* const topologyListener = getTopologyListener();
        const Milliseconds timeout = config.getConnectionTimeout();
        while (elapsed() < timeout && !topologyListener->hasHelloResponse(hostAndPort)) {
            advanceTime(Milliseconds(1));
        }

        ASSERT_TRUE(topologyListener->hasHelloResponse(hostAndPort));
        const auto responses = topologyListener->getHelloResponse(hostAndPort);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_OK(responses[0]);

        waitForNextHello(timeout);
    }
};

INSTANTIATE_TEST_SUITE_P(SingleServerDiscoveryMonitorProtocol,
                         SingleServerDiscoveryMonitorProtocolTestFixture,
                         testing::Values(ReplicaSetMonitorProtocol::kSdam,
                                         ReplicaSetMonitorProtocol::kStreamable),
                         [](const testing::TestParamInfo<ReplicaSetMonitorProtocol>& info) {
                             // The returned string will be added as a suffix to the name of the
                             // generated tests, e.g. "foo.bar/Sdam" and "foo.bar/Streamable".
                             switch (info.param) {
                                 case ReplicaSetMonitorProtocol::kSdam:
                                     return "Sdam";
                                 case ReplicaSetMonitorProtocol::kStreamable:
                                     return "Streamable";
                                 default:
                                     MONGO_UNREACHABLE;
                             }
                         });

/**
 * Checks that a SingleServerDiscoveryMonitor sends "hello" requests at least heartbeatFrequency
 * apart.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, heartbeatFrequencyCheck) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);
    const HostAndPort hostAndPort{replSet->getSecondaries()[0]};

    const auto config = SdamConfiguration(std::vector<HostAndPort>{hostAndPort});
    auto ssHelloMonitor = initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssHelloMonitor->disableExpeditedChecking();

    // A "hello" command fails if it takes as long or longer than timeoutMS.
    auto timeoutMS = config.getConnectionTimeout();
    auto heartbeatFrequency = config.getHeartBeatFrequency();

    checkSingleHello(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextHello(timeoutMS);

    checkSingleHello(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextHello(timeoutMS);

    checkSingleHello(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextHello(timeoutMS);

    checkSingleHello(heartbeatFrequency, hostAndPort, replSet.get());
    waitForNextHello(timeoutMS);
}

/**
 * Confirms that a SingleServerDiscoveryMonitor reports to the TopologyListener when a "hello"
 * command succeeds.
 */
TEST_P(SingleServerDiscoveryMonitorProtocolTestFixture,
       singleServerDiscoveryMonitorReportsSuccess) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    const HostAndPort hostAndPort{replSet->getSecondaries()[0]};

    // For streamable protocol, configure responses to include topologyVersion.
    if (isStreamableProtocol()) {
        auto node = replSet->getNode(hostAndPort.toString());
        node->setCommandReply("hello", makeHelloResponseWithTopologyVersion());
    }

    const SdamConfiguration config{std::vector<HostAndPort>{hostAndPort}};
    auto ssHelloMonitor = initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssHelloMonitor->disableExpeditedChecking();

    // For streamable protocol, prime with first hello to establish topologyVersion.
    primeForStreamableIfNeeded(replSet.get(), hostAndPort, config);

    auto* const topologyListener = getTopologyListener();
    const auto [successesBefore, failuresBefore] = topologyListener->serverHeartbeatCounts();
    processHelloRequest(replSet.get(), hostAndPort);
    const Milliseconds timeout = config.getConnectionTimeout();
    while (elapsed() < timeout && !topologyListener->hasHelloResponse(hostAndPort)) {
        advanceTime(Milliseconds(1));
    }
    ASSERT_TRUE(topologyListener->hasHelloResponse(hostAndPort));
    const auto responses = topologyListener->getHelloResponse(hostAndPort);
    ASSERT_EQ(responses.size(), 1);
    ASSERT_OK(responses[0]);
    const auto [successes, failures] = topologyListener->serverHeartbeatCounts();
    ASSERT_EQ(successes - successesBefore, 1);
    ASSERT_EQ(failures - failuresBefore, 0);
}

/**
 * Confirms that a SingleServerDiscoveryMonitor reports to the TopologyListener when a "hello"
 * command returns a response with ok:0 indicating the remote server is shutting down.
 */
TEST_P(SingleServerDiscoveryMonitorProtocolTestFixture,
       singleServerDiscoveryMonitorReportsRemoteShutdownInProgress) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    const HostAndPort hostAndPort{replSet->getSecondaries()[0]};
    auto node = replSet->getNode(hostAndPort.toString());

    // Configure the mock server to return a ShutdownInProgress error response.
    auto shutdownErrorResponse = BSON("ok" << 0.0 << "errmsg"
                                           << "The server is in quiesce mode and will shut down"
                                           << "code" << ErrorCodes::ShutdownInProgress << "codeName"
                                           << "ShutdownInProgress");

    if (isStreamableProtocol()) {
        // For streamable: first response establishes topologyVersion, second is the error.
        node->setCommandReply("hello",
                              {makeHelloResponseWithTopologyVersion(), shutdownErrorResponse});
    } else {
        node->setCommandReply("hello", shutdownErrorResponse);
    }

    const auto config = SdamConfiguration(std::vector<HostAndPort>{hostAndPort});
    auto ssHelloMonitor = initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssHelloMonitor->disableExpeditedChecking();

    // For streamable protocol, prime with first hello to establish topologyVersion.
    primeForStreamableIfNeeded(replSet.get(), hostAndPort, config);

    auto* const topologyListener = getTopologyListener();
    const auto [successesBefore, failuresBefore] = topologyListener->serverHeartbeatCounts();
    processHelloRequest(replSet.get(), hostAndPort);
    const Milliseconds timeout = config.getConnectionTimeout();
    while (elapsed() < timeout && !topologyListener->hasHelloResponse(hostAndPort)) {
        advanceTime(Milliseconds(1));
    }
    ASSERT_TRUE(topologyListener->hasHelloResponse(hostAndPort));
    const auto responses = topologyListener->getHelloResponse(hostAndPort);
    ASSERT_EQ(responses.size(), 1);
    ASSERT_EQ(responses[0], ErrorCodes::ShutdownInProgress);
    const auto [successes, failures] = topologyListener->serverHeartbeatCounts();
    ASSERT_EQ(successes - successesBefore, 0);
    ASSERT_EQ(failures - failuresBefore, 1);
}

/**
 * Confirms that a SingleServerDiscoveryMonitor reports an IDL parsing error to the TopologyListener
 * when a "hello" command returns ok:0 but is missing required error fields (code, codeName,
 * errmsg).
 */
TEST_P(SingleServerDiscoveryMonitorProtocolTestFixture,
       singleServerDiscoveryMonitorReportsIDLParseErrorForMalformedErrorReply) {
    const auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    const HostAndPort hostAndPort{replSet->getSecondaries()[0]};
    auto* const node = replSet->getNode(hostAndPort.toString());

    // Configure the mock server to return an error response missing required fields.
    // A valid error response requires "code" and "errmsg" fields per the ErrorReply IDL.
    const auto malformedErrorResponse = BSON("ok" << 0.0);

    if (isStreamableProtocol()) {
        // For streamable: first response establishes topologyVersion, second is the error.
        node->setCommandReply("hello",
                              {makeHelloResponseWithTopologyVersion(), malformedErrorResponse});
    } else {
        node->setCommandReply("hello", malformedErrorResponse);
    }

    const SdamConfiguration config{std::vector<HostAndPort>{hostAndPort}};
    const auto ssHelloMonitor =
        initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssHelloMonitor->disableExpeditedChecking();

    // For streamable protocol, prime with first hello to establish topologyVersion.
    primeForStreamableIfNeeded(replSet.get(), hostAndPort, config);

    auto* const topologyListener = getTopologyListener();
    const auto [successesBefore, failuresBefore] = topologyListener->serverHeartbeatCounts();
    processHelloRequest(replSet.get(), hostAndPort);
    const Milliseconds timeout = config.getConnectionTimeout();
    while (elapsed() < timeout && !topologyListener->hasHelloResponse(hostAndPort)) {
        advanceTime(Milliseconds(1));
    }
    ASSERT_TRUE(topologyListener->hasHelloResponse(hostAndPort));
    const auto responses = topologyListener->getHelloResponse(hostAndPort);
    ASSERT_EQ(responses.size(), 1);
    ASSERT_EQ(responses[0], ErrorCodes::IDLFailedToParse);
    const auto [successes, failures] = topologyListener->serverHeartbeatCounts();
    ASSERT_EQ(successes - successesBefore, 0);
    ASSERT_EQ(failures - failuresBefore, 1);
}

/**
 * Confirms that a SingleServerDiscoveryMonitor reports to the TopologyListener when a "hello"
 * command generates a network error.
 */
TEST_P(SingleServerDiscoveryMonitorProtocolTestFixture,
       singleServerDiscoveryMonitorReportsFailure) {
    const auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    const HostAndPort hostAndPort{replSet->getSecondaries()[0]};
    const auto config = SdamConfiguration(std::vector<HostAndPort>{hostAndPort});

    if (isStreamableProtocol()) {
        // For streamable: first response establishes topologyVersion, then server dies.
        auto* const node = replSet->getNode(hostAndPort.toString());
        node->setCommandReply("hello", makeHelloResponseWithTopologyVersion());
    }

    const auto ssHelloMonitor =
        initSingleServerDiscoveryMonitor(config, hostAndPort, replSet.get());
    ssHelloMonitor->disableExpeditedChecking();

    if (isStreamableProtocol()) {
        // Prime with first hello to establish topologyVersion, then kill the server.
        primeForStreamableIfNeeded(replSet.get(), hostAndPort, config);
        {
            NetworkInterfaceMock::InNetworkGuard ing(getNet());
            replSet->kill(hostAndPort.toString());
        }
    } else {
        // Kill the server before starting.
        {
            NetworkInterfaceMock::InNetworkGuard ing(getNet());
            replSet->kill(hostAndPort.toString());
        }
    }

    auto* const topologyListener = getTopologyListener();
    const auto [successesBefore, failuresBefore] = topologyListener->serverHeartbeatCounts();
    processHelloRequest(replSet.get(), hostAndPort);
    const Milliseconds timeout = config.getConnectionTimeout();
    while (elapsed() < timeout && !topologyListener->hasHelloResponse(hostAndPort)) {
        advanceTime(Milliseconds(1));
    }
    ASSERT_TRUE(topologyListener->hasHelloResponse(hostAndPort));
    const auto responses = topologyListener->getHelloResponse(hostAndPort);
    ASSERT_EQ(responses.size(), 1);
    ASSERT_EQ(responses[0], ErrorCodes::HostUnreachable);
    const auto [successes, failures] = topologyListener->serverHeartbeatCounts();
    ASSERT_EQ(successes - successesBefore, 0);
    ASSERT_EQ(failures - failuresBefore, 1);
}

TEST_F(ServerDiscoveryMonitorTestFixture, ServerHelloMonitorOnTopologyDescriptionChangeAddHost) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 2, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    auto hostAndPortList = replSet->getHosts();
    auto host0 = hostAndPortList[0];
    std::vector<HostAndPort> host0Vec{host0};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(host0Vec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto helloMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);
    helloMonitor->disableExpeditedChecking();

    auto host1Delay = Milliseconds(100);
    checkSingleHello(host1Delay, host0, replSet.get());
    ASSERT_FALSE(hasReadyRequests());

    // Start monitoring host1.
    auto host1 = hostAndPortList[1];
    std::vector<HostAndPort> allHostsVec{host0, host1};
    auto sdamConfigAllHosts = sdam::SdamConfiguration(allHostsVec);
    auto topologyDescriptionAllHosts =
        std::make_shared<sdam::TopologyDescription>(sdamConfigAllHosts);
    helloMonitor->onTopologyDescriptionChangedEvent(topologyDescription0,
                                                    topologyDescriptionAllHosts);
    // Ensure expedited checking is disabled for the SingleServerDiscoveryMonitor corresponding to
    // host1 as well.
    helloMonitor->disableExpeditedChecking();

    // Confirm host0 and host1 are monitored.
    auto heartbeatFrequency = sdamConfigAllHosts.getHeartBeatFrequency();
    checkSingleHello(heartbeatFrequency - host1Delay, host1, replSet.get());
    waitForNextHello(sdamConfigAllHosts.getConnectionTimeout());
    checkSingleHello(host1Delay, host0, replSet.get());
}

TEST_F(ServerDiscoveryMonitorTestFixture, ServerHelloMonitorOnTopologyDescriptionChangeRemoveHost) {
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
    auto helloMonitor =
        initServerDiscoveryMonitor(uri, sdamConfigAllHosts, topologyDescriptionAllHosts);
    helloMonitor->disableExpeditedChecking();

    // Confirm that both hosts are monitored.
    auto heartbeatFrequency = sdamConfigAllHosts.getHeartBeatFrequency();
    while (hasReadyRequests()) {
        processHelloRequest(replSet.get());
    }
    auto deadline = elapsed() + heartbeatFrequency;
    auto topologyListener = getTopologyListener();
    auto hasResponses = [&]() {
        return topologyListener->hasHelloResponse(host0) &&
            topologyListener->hasHelloResponse(host1);
    };
    while (elapsed() < heartbeatFrequency && !hasResponses()) {
        advanceTime(Milliseconds(1));
    }
    validateHelloResponse(host0, deadline);
    validateHelloResponse(host1, deadline);

    // Remove host1 from the TopologyDescription to stop monitoring it.
    std::vector<HostAndPort> host0Vec{host0};
    auto sdamConfig0 = sdam::SdamConfiguration(host0Vec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    helloMonitor->onTopologyDescriptionChangedEvent(topologyDescriptionAllHosts,
                                                    topologyDescription0);

    checkNoActivityBefore(deadline);
    waitForNextHello(sdamConfig0.getConnectionTimeout());

    checkSingleHello(heartbeatFrequency, host0, replSet.get());
    waitForNextHello(sdamConfig0.getConnectionTimeout());

    // Confirm the next "hello" request is sent to host0 and not host1.
    checkSingleHello(heartbeatFrequency, host0, replSet.get());
}

TEST_F(ServerDiscoveryMonitorTestFixture, ServerHelloMonitorShutdownStopsHelloRequests) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};
    auto sdamConfig = sdam::SdamConfiguration(hostVec);
    auto topologyDescription = std::make_shared<sdam::TopologyDescription>(sdamConfig);
    auto uri = replSet->getURI();
    auto helloMonitor = initServerDiscoveryMonitor(uri, sdamConfig, topologyDescription);
    helloMonitor->disableExpeditedChecking();

    auto heartbeatFrequency = sdamConfig.getHeartBeatFrequency();
    checkSingleHello(heartbeatFrequency - Milliseconds(200), hostVec[0], replSet.get());

    helloMonitor->shutdown();

    // After the ServerDiscoveryMonitor shuts down, the TopologyListener may have responses until
    // heartbeatFrequency has passed, but none of them should indicate Status::OK.
    auto deadline = elapsed() + heartbeatFrequency;
    auto topologyListener = getTopologyListener();

    // Drain any requests already scheduled.
    while (elapsed() < deadline) {
        while (hasReadyRequests()) {
            processHelloRequest(replSet.get(), hostVec[0]);
        }
        if (topologyListener->hasHelloResponse(hostVec[0])) {
            const auto helloResponses = topologyListener->getHelloResponse(hostVec[0]);
            for (const Status& response : helloResponses) {
                ASSERT_FALSE(response.isOK());
            }
        }
        advanceTime(Milliseconds(1));
    }

    ASSERT_FALSE(topologyListener->hasHelloResponse(hostVec[0]));
}

/**
 * Tests that the ServerDiscoveryMonitor waits until SdamConfiguration::kMinHeartbeatFrequency has
 * passed since the last "hello" was received if requestImmediateCheck() is called before enough
 * time has passed.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, ServerHelloMonitorRequestImmediateCheckWaitMinHeartbeat) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(hostVec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto helloMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);

    // Ensure the server is not in expedited mode *before* requestImmediateCheck().
    helloMonitor->disableExpeditedChecking();

    // Check that there is only one "hello" request at time t=0 up until
    // timeAdvanceFromFirstHello.
    auto minHeartbeatFrequency = SdamConfiguration::kMinHeartbeatFrequency;
    auto timeAdvanceFromFirstHello = Milliseconds(10);
    ASSERT_LT(timeAdvanceFromFirstHello, minHeartbeatFrequency);
    checkSingleHello(timeAdvanceFromFirstHello, hostVec[0], replSet.get());

    // It's been less than SdamConfiguration::kMinHeartbeatFrequency since the last "hello" was
    // received. The next "hello" should be sent SdamConfiguration::kMinHeartbeatFrequency since
    // the last "hello" was received rather than immediately.
    auto timeRequestImmediateSent = elapsed();
    helloMonitor->requestImmediateCheck();
    waitForNextHello(minHeartbeatFrequency);

    auto timeHelloSent = elapsed();
    ASSERT_LT(timeRequestImmediateSent, timeHelloSent);
    ASSERT_LT(timeHelloSent, timeRequestImmediateSent + minHeartbeatFrequency);
    checkSingleHello(minHeartbeatFrequency, hostVec[0], replSet.get());

    // Confirm expedited requests continue since there is no primary.
    waitForNextHello(sdamConfig0.getConnectionTimeout());
    checkSingleHello(minHeartbeatFrequency, hostVec[0], replSet.get());
}

/**
 * Tests that if more than SdamConfiguration::kMinHeartbeatFrequency has passed since the last
 * "hello" response was received, the ServerDiscoveryMonitor sends an "hello" immediately after
 * requestImmediateCheck() is called.
 */
TEST_F(ServerDiscoveryMonitorTestFixture, ServerHelloMonitorRequestImmediateCheckNoWait) {
    auto replSet = std::make_unique<MockReplicaSet>(
        "test", 1, /* hasPrimary = */ false, /* dollarPrefixHosts = */ false);

    std::vector<HostAndPort> hostVec{replSet->getHosts()[0]};

    // Start up the ServerDiscoveryMonitor to monitor host0 only.
    auto sdamConfig0 = sdam::SdamConfiguration(hostVec);
    auto topologyDescription0 = std::make_shared<sdam::TopologyDescription>(sdamConfig0);
    auto uri = replSet->getURI();
    auto helloMonitor = initServerDiscoveryMonitor(uri, sdamConfig0, topologyDescription0);

    // Ensure the server is not in expedited mode *before* requestImmediateCheck().
    helloMonitor->disableExpeditedChecking();

    // No less than SdamConfiguration::kMinHeartbeatFrequency must pass before
    // requestImmediateCheck() is called in order to ensure the server reschedules for an immediate
    // check.
    auto minHeartbeatFrequency = SdamConfiguration::kMinHeartbeatFrequency;
    checkSingleHello(minHeartbeatFrequency + Milliseconds(10), hostVec[0], replSet.get());

    helloMonitor->requestImmediateCheck();
    checkSingleHello(minHeartbeatFrequency, hostVec[0], replSet.get());

    // Confirm expedited requests continue since there is no primary.
    waitForNextHello(sdamConfig0.getConnectionTimeout());
    checkSingleHello(minHeartbeatFrequency, hostVec[0], replSet.get());
}

}  // namespace
}  // namespace mongo::sdam
