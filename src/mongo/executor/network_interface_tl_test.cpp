// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Unit tests for the parts of NetworkInterfaceTL that resolve before a command is sent to a
 * remote: alarms, the request-deadline pre-check in getClient(), and the mapping of
 * connection-acquisition errors. They run against a MockReactor with a frozen clock and a
 * MockAsyncClientFactory that never produces connections, so every deadline and counter is exact.
 *
 * Anything that needs an AsyncDBClient (the per-command timer, killOperations, shutdown while a
 * command is in flight) is out of scope here and remains in network_interface_integration_test.
 */

#include "mongo/executor/network_interface_tl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/executor/mock_async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_test_common.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/transport/mock_reactor.h"
#include "mongo/transport/mock_transport_layer_with_mock_reactor.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace executor {

namespace {

const HostAndPort kTarget{"mock-remote", 27017};
constexpr auto kTimedOutBeforeAcquisitionMetric =
    "numRequestsTimedOutBeforeConnectionAcquisitionAttempt";
constexpr Milliseconds kNoTimeout = RemoteCommandRequest::kNoTimeout;

/**
 * Reads a process-global metric registered by network_interface_tl.cpp; other tests in this binary
 * may bump it, so compare deltas. Not in network_interface_test_common.h: MetricTree is private to
 * observability, and the module lint scans that header but not unit tests.
 */
long long getMetric(const std::string& name) {
    BSONObjBuilder bob;
    globalMetricTreeSet()[ClusterRole::None].appendTo(bob);
    auto obj = bob.obj();
    return obj["metrics"]["operation"][name].numberLong();
}

/**
 * The service context's clocks and the MockReactor's clock are all ClockSourceMocks, which share
 * one process-global time, so request deadlines, net().now() and reactor().advanceTime() agree.
 */
class NetworkInterfaceTLTest : public ClockSourceMockServiceContextTest {
public:
    void setUp() override {
        ClockSourceMockServiceContextTest::setUp();

        auto tl = std::make_unique<transport::MockTransportLayerWithMockReactor>();
        _tl = tl.get();
        getServiceContext()->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
        ASSERT_OK(getServiceContext()->getTransportLayerManager()->setup());
        ASSERT_OK(getServiceContext()->getTransportLayerManager()->start());

        startNet();
    }

    void tearDown() override {
        if (_net) {
            _net->shutdown();
            _net.reset();
        }
        getServiceContext()->getTransportLayerManager()->shutdown();
        ClockSourceMockServiceContextTest::tearDown();
    }

    /**
     * (Re)creates the NetworkInterfaceTL under test with a fresh factory. Each startup() asks the
     * transport layer for a new reactor, so reactor() is rebound too.
     */
    void startNet() {
        _factory = std::make_shared<MockAsyncClientFactory>();
        _net = std::make_unique<NetworkInterfaceTL>(
            "NetworkInterfaceTLTest", _factory, nullptr, /*trackShardRequestCounts=*/false);
        _net->startup();
        _reactor = _tl->latestReactor();
        ASSERT(_reactor);
    }

    NetworkInterface& net() {
        return *_net;
    }

    MockAsyncClientFactory& factory() {
        return *_factory;
    }

    transport::MockReactor& reactor() {
        return *_reactor;
    }

    virtual BatonHandle baton() {
        return nullptr;
    }

    virtual Interruptible* interruptible() {
        return Interruptible::notInterruptible();
    }

    SemiFuture<RemoteCommandResponse> runCommand(
        RemoteCommandRequest request,
        const CancellationToken& token = CancellationToken::uncancelable()) {
        return net().startCommand(makeCallbackHandle(), request, baton(), token);
    }

    // The command is never sent, so its contents do not matter.
    RemoteCommandRequest makeTestCommand(Milliseconds timeout,
                                         boost::optional<ErrorCodes::Error> timeoutCode = {}) {
        RemoteCommandRequest request(
            kTarget, DatabaseName::kAdmin, BSON("ping" << 1), BSONObj(), nullptr, timeout);
        request.timeoutCode = timeoutCode;
        return request;
    }

    void assertNumOps(const ExpectedCounters& expected) {
        ASSERT_EQ(net().getCounters(), expected);
    }

    void runAcquireConnectionTimeoutTest(boost::optional<ErrorCodes::Error> customCode);

    // Test case definitions, run with and without a baton.
    void testNoCustomCodeRequestTimeoutHit();
    void testCustomCodeRequestTimeoutHit();
    void testCancelBeforeConnection();
    void testTimeoutWaitingToAcquireConnection();
    void testCustomCodeTimeoutWaitingToAcquireConnection();
    void testNonTimeoutAcquisitionErrorIsPassedThrough();

private:
    transport::MockTransportLayerWithMockReactor* _tl = nullptr;
    std::shared_ptr<MockAsyncClientFactory> _factory;
    std::unique_ptr<NetworkInterfaceTL> _net;
    std::shared_ptr<transport::MockReactor> _reactor;
};

/** Same tests, but every command carries a DefaultBaton pumped via SemiFuture::get(opCtx). */
using NetworkInterfaceTLTestWithBaton = WithBatonFixture<NetworkInterfaceTLTest>;

using NetworkInterfaceTLTestWithoutBaton = NetworkInterfaceTLTest;

// Alarms use only the reactor; there is no per-command baton involved.
TEST_F(NetworkInterfaceTLTest, SetAlarm) {
    // Set alarm, advance to just before the deadline, then onto it.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        auto future = net().setAlarm(expiration);
        ASSERT_FALSE(future.isReady());
        ASSERT_EQ(reactor().pendingTimerCount(), 1);

        reactor().advanceTime(Milliseconds(99));
        ASSERT_FALSE(future.isReady());

        reactor().advanceTime(Milliseconds(1));
        ASSERT_TRUE(future.isReady());
        future.get();
        ASSERT_EQ(net().now(), expiration);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm with passed deadline: resolved without touching the reactor.
    {
        Date_t before = net().now();
        auto future = net().setAlarm(before - Milliseconds(100));
        ASSERT_TRUE(future.isReady());
        future.get();
        ASSERT_EQ(net().now(), before);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm, shutdown, then check that future finishes as though it were cancelled.
    {
        Date_t expiration = net().now() + Hours(24);
        auto future = net().setAlarm(expiration);
        ASSERT_FALSE(future.isReady());
        ASSERT_EQ(reactor().pendingTimerCount(), 1);

        net().shutdown();
        ASSERT_TRUE(future.isReady());
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
        ASSERT_TRUE(reactor().isStopped());
        ASSERT_NO_FATAL_FAILURE(startNet());
        ASSERT_FALSE(reactor().isStopped());
    }

    // Set alarm with cancelled token.
    {
        Date_t expiration = net().now() + Hours(24);
        CancellationSource source;
        source.cancel();
        auto future = net().setAlarm(expiration, source.token());
        ASSERT_TRUE(future.isReady());
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        ASSERT_LT(net().now(), expiration);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm, then cancel.
    {
        Date_t expiration = net().now() + Hours(24);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        ASSERT_FALSE(future.isReady());
        ASSERT_EQ(reactor().pendingTimerCount(), 1);

        source.cancel();
        ASSERT_TRUE(future.isReady());
        ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
        ASSERT_LT(net().now(), expiration);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm, then dismiss source, and check that deadline is respected.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        ASSERT_FALSE(future.isReady());

        source = {};
        ASSERT_FALSE(future.isReady());
        ASSERT_EQ(reactor().pendingTimerCount(), 1);
        ASSERT_LT(net().now(), expiration);

        reactor().advanceTime(Milliseconds(100));
        ASSERT_TRUE(future.isReady());
        future.get();
        ASSERT_EQ(net().now(), expiration);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm, cancel after deadline.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        reactor().advanceTime(Milliseconds(100));
        future.get();
        ASSERT_EQ(net().now(), expiration);

        source.cancel();
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm, cancel after destruction of the interface.
    {
        Date_t expiration = net().now() + Milliseconds(100);
        CancellationSource source;
        auto future = net().setAlarm(expiration, source.token());
        reactor().advanceTime(Milliseconds(100));
        future.get();
        ASSERT_EQ(net().now(), expiration);

        net().shutdown();
        ASSERT_NO_FATAL_FAILURE(startNet());
        source.cancel();
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }

    // Set alarm after shutdown.
    {
        Date_t expiration = net().now() + Seconds(60);
        net().shutdown();
        ASSERT_THROWS_CODE(
            net().setAlarm(expiration).get(), DBException, ErrorCodes::ShutdownInProgress);
        ASSERT_EQ(reactor().pendingTimerCount(), 0);
    }
}

// A request whose deadline has already passed is failed by getClient() with
// NetworkInterfaceExceededTimeLimit, before the factory is ever consulted.
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, NoCustomCodeRequestTimeoutHit) {
    auto metricBefore = getMetric(kTimedOutBeforeAcquisitionMetric);

    // With a frozen clock, a zero timeout is a deadline of exactly now, which is already expired.
    auto result = runCommand(makeTestCommand(Milliseconds(0))).get(interruptible());

    ASSERT_EQ(result.status, ErrorCodes::NetworkInterfaceExceededTimeLimit);
    ASSERT_EQ(result.target, kTarget);
    ASSERT_EQ(factory().requestCount(), 0);
    assertNumOps({.sent = 0u, .canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
    ASSERT_EQ(getMetric(kTimedOutBeforeAcquisitionMetric), metricBefore + 1);
}

// Same, but the request's custom timeout code replaces the default.
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, CustomCodeRequestTimeoutHit) {
    auto metricBefore = getMetric(kTimedOutBeforeAcquisitionMetric);

    auto result = runCommand(makeTestCommand(Milliseconds(0), ErrorCodes::MaxTimeMSExpired))
                      .get(interruptible());

    ASSERT_EQ(result.status, ErrorCodes::MaxTimeMSExpired);
    ASSERT_EQ(result.target, kTarget);
    ASSERT_EQ(factory().requestCount(), 0);
    assertNumOps({.sent = 0u, .canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
    ASSERT_EQ(getMetric(kTimedOutBeforeAcquisitionMetric), metricBefore + 1);
}

// Cancelling the caller's token while the factory is still working propagates to the token the
// interface handed to the factory, and the command completes as CallbackCanceled.
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, CancelBeforeConnection) {
    CancellationSource source;
    auto request = makeTestCommand(Minutes(1));
    auto future = runCommand(request, source.token());

    // startCommand() runs synchronously through getClient() into the factory.
    ASSERT_EQ(factory().requestCount(), 1);
    auto acquisition = factory().request(0);
    ASSERT_EQ(acquisition.target, kTarget);
    ASSERT_EQ(acquisition.sslMode, request.sslMode);
    ASSERT_EQ(acquisition.timeout, Minutes(1));
    ASSERT_FALSE(acquisition.isLease);
    ASSERT_FALSE(acquisition.token.isCanceled());
    ASSERT_FALSE(acquisition.settled());

    source.cancel();
    ASSERT_TRUE(acquisition.token.isCanceled());
    ASSERT_TRUE(acquisition.settled());
    ASSERT_EQ(net().getCounters().sent, 0);

    auto result = std::move(future).get(interruptible());
    ASSERT_EQ(result.status, ErrorCodes::CallbackCanceled);
    ASSERT_EQ(result.target, kTarget);
    ASSERT(result.elapsed);
    ASSERT_EQ(factory().requestCount(), 1);
    assertNumOps({.sent = 0u, .canceled = 1u, .timedOut = 0u, .failed = 0u, .succeeded = 0u});
}

void NetworkInterfaceTLTest::runAcquireConnectionTimeoutTest(
    boost::optional<ErrorCodes::Error> customCode) {
    auto future = runCommand(makeTestCommand(Milliseconds(100), customCode));

    ASSERT_EQ(factory().requestCount(), 1);
    // poolTimeout is deadline - now(); with a frozen clock that is the full request timeout.
    ASSERT_EQ(factory().request(0).timeout, Milliseconds(100));
    ASSERT_FALSE(factory().request(0).settled());

    // Time passes up to the deadline without anything else in the interface reacting: the
    // per-command timer is only armed once a connection has been acquired.
    reactor().advanceTime(Milliseconds(100));
    ASSERT_EQ(reactor().pendingTimerCount(), 0);
    ASSERT_FALSE(factory().request(0).settled());

    // The pool's own timeout fires.
    factory().request(0).fail(Status(ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
                                     "timed out waiting for a connection"));

    auto result = std::move(future).get(interruptible());
    ASSERT_EQ(result.status,
              customCode.value_or(ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit));
    ASSERT_EQ(result.target, kTarget);
    assertNumOps({.sent = 0u, .canceled = 0u, .timedOut = 1u, .failed = 0u, .succeeded = 0u});
}

TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, TimeoutWaitingToAcquireConnection) {
    runAcquireConnectionTimeoutTest({});
}

// The request's custom timeout code replaces the pool's ExceededTimeLimit-class error.
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, CustomCodeTimeoutWaitingToAcquireConnection) {
    runAcquireConnectionTimeoutTest(ErrorCodes::MaxTimeMSExpired);
}

// Acquisition errors that are not ExceededTimeLimit errors (a connection-setup timeout, a socket
// error, rejection by an overloaded pool) are passed through unchanged and counted as failed.
TEST_WITH_AND_WITHOUT_BATON_F(NetworkInterfaceTLTest, NonTimeoutAcquisitionErrorIsPassedThrough) {
    size_t n = 0;
    for (auto code : {ErrorCodes::ConnectionEstablishmentTimeout,
                      ErrorCodes::SocketException,
                      ErrorCodes::PooledConnectionAcquisitionRejected}) {
        auto future = runCommand(makeTestCommand(kNoTimeout));

        ASSERT_EQ(factory().requestCount(), n + 1);
        // No request deadline means no pool timeout is derived; kNoTimeout is passed through.
        ASSERT_EQ(factory().request(n).timeout, kNoTimeout);
        factory().request(n).fail(Status(code, "test"));
        ++n;

        auto result = std::move(future).get(interruptible());
        ASSERT_EQ(result.status, code);
        ASSERT_EQ(result.target, kTarget);
        assertNumOps({.sent = 0u, .canceled = 0u, .timedOut = 0u, .failed = n, .succeeded = 0u});
    }
}

}  // namespace
}  // namespace executor
}  // namespace mongo
