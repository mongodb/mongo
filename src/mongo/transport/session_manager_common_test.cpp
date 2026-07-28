// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session_manager_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/egress_response_rate_limiter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/session_workflow_p.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <cerrno>
#include <mutex>

#include <sys/resource.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

class SessionManagerCommonTest : public ServiceContextTest {
public:
    auto makeClient(std::shared_ptr<Session> session) {
        return getServiceContext()->getService()->makeClient("test", std::move(session));
    }
};

TEST_F(SessionManagerCommonTest, VerifyMaxOpenSessionsBasedOnRlimit) {
    struct rlimit originalLimit, newLimit;
    auto rlimitReturnCode = getrlimit(RLIMIT_NOFILE, &originalLimit);
    const auto savedErrno1 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno1;

    ASSERT_GTE(originalLimit.rlim_max, 10);

    newLimit = originalLimit;
    newLimit.rlim_cur = 10;
    rlimitReturnCode = setrlimit(RLIMIT_NOFILE, &newLimit);
    const auto savedErrno2 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno2;

    // 80% of half of 10 is 4, which is the arithmetic we want to verify in the
    // `getSupportedMax` function via the `maxOpenSessions` getter.
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_EQ(sm.maxOpenSessions(), 4);

    rlimitReturnCode = setrlimit(RLIMIT_NOFILE, &originalLimit);
    const auto savedErrno3 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno3;
}

// The three tests below verify that onClientConnect/onClientDisconnect correctly
// updates the number of sessions on the load balancer port and the priority port.

TEST_F(SessionManagerCommonTest, OnClientConnectAndDisconnectLoadBalancedSessions) {
    TransportLayerMock tl;
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);

    FailPointEnableBlock fp("clientIsLoadBalancedPeer");
    auto session = MockSession::create(&tl);
    auto client = makeClient(session);
    sm.onClientConnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 1);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);

    sm.onClientDisconnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);
}

TEST_F(SessionManagerCommonTest, OnClientConnectAndDisconnectPrioritySessions) {
    TransportLayerMock tl;
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);

    auto session = std::make_shared<MockPrioritySession>(&tl);
    auto client = makeClient(session);
    sm.onClientConnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 1);

    sm.onClientDisconnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);
}

TEST_F(SessionManagerCommonTest, OnClientConnectAndDisconnectStandardSessions) {
    TransportLayerMock tl;
    MockSessionManagerCommon sm(getServiceContext());

    auto session = MockSession::create(&tl);
    auto client = makeClient(session);
    sm.onClientConnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);

    sm.onClientDisconnect(client.get());
    ASSERT_EQ(sm.getSessionStats().numLoadBalancedSessions, 0);
    ASSERT_EQ(sm.getSessionStats().numPrioritySessions, 0);
}

TEST_F(SessionManagerCommonTest, ShouldIncludeInConnectionsServerStatusDefaultsToFalse) {
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_FALSE(sm.shouldIncludeInConnectionsServerStatus());
}

TEST_F(SessionManagerCommonTest, GetSessionStatsMaxOpenSessions) {
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_EQ(sm.getSessionStats().maxOpenSessions, static_cast<int64_t>(sm.maxOpenSessions()));
}

TEST_F(SessionManagerCommonTest, GetSessionStatsNumRejectedSessions) {
    TransportLayerMock tl;
    MockSessionManagerCommon sm(getServiceContext());

    FailPointEnableBlock fp("rejectNewNonPriorityConnections");
    sm.startSession(MockSession::create(&tl));
    ASSERT_EQ(sm.getSessionStats().numRejectedSessions, 1);
}

TEST_F(SessionManagerCommonTest, DisconnectShutdownAwareInterruptibleWakesOnShutdown) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        MockSessionManagerCommon sm(getServiceContext());
        TransportLayerMock tl;
        // MockSession reports connected by default, so the tarpit waiter stays parked on the
        // shutdown token rather than bailing on a peer disconnect.
        auto session = std::make_shared<MockSession>(&tl);
        DisconnectShutdownAwareInterruptible interruptible{
            sm.getShutdownToken(), session.get(), getServiceContext()->getPreciseClockSource()};

        // Configure a small refresh rate with a single-token burst.
        // - The first throttle() below consumes the burst and admits immediately.
        // - The second (spawned) call then queues and parks on the Interruptible.
        // The tiny refresh rate gives a ~1000s wait window so the waiter stays parked until
        // shutdown rather than self-waking on a token.
        constexpr double kRefreshRatePerSec = 0.001;
        constexpr double kBurstCapacitySecs =
            1000.0;  // burst = refreshRate * burstCapacitySecs = 1.0
        auto& limiter = admission::EgressResponseRateLimiter::get(getServiceContext());
        limiter.updateRateParameters(kRefreshRatePerSec, kBurstCapacitySecs);
        const auto baselineAddedToQueue = limiter.stats().addedToQueue();

        // Consume the single burst token so the next throttle() cannot admit immediately.
        ASSERT_OK(limiter.throttle(&interruptible, getServiceContext()->getPreciseClockSource()));

        Notification<void> woke;
        std::vector<unittest::JoinThread> threads;
        threads.emplace_back(monitor.spawn([&]() {
            auto status =
                limiter.throttle(&interruptible, getServiceContext()->getPreciseClockSource());
            ASSERT_EQ(status.code(), ErrorCodes::InterruptedAtShutdown)
                << "egress throttle() should be interrupted by SessionManager shutdown, not return "
                   "OK or another error";
            woke.set();
        }));

        // Wait until the spawned thread has enqueued and is actually parked in the wait, then
        // cancel via shutdown().
        const auto parkDeadline = Date_t::now() + Seconds(30);
        while (limiter.stats().addedToQueue() == baselineAddedToQueue ||
               !interruptible.isWaitingForConditionOrInterrupt()) {
            if (Date_t::now() >= parkDeadline) {
                break;
            }
            sleepmillis(1);
        }
        ASSERT_GT(limiter.stats().addedToQueue(), baselineAddedToQueue)
            << "throttle() never enqueued a waiter";
        ASSERT(interruptible.isWaitingForConditionOrInterrupt());

        sm.shutdown(Seconds(30));
        woke.get();
    });
}

// A MockSession whose peer-disconnect state is settable from the test, simulating a client closing
// the socket mid-tarpit. The blocking waitForPeerDisconnectUntil() models a real poll(2) for
// POLLRDHUP: it parks on a condition variable that is woken immediately when the test flips the
// peer to disconnected.
class DisconnectableMockSession : public MockSession {
public:
    explicit DisconnectableMockSession(TransportLayer* tl) : MockSession(tl) {}

    void setConnected(bool v) {
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _connected = v;
        }
        if (!v) {
            _cv.notify_all();
        }
    }

    bool waitForPeerDisconnectUntil(Date_t deadline) override {
        std::unique_lock<std::mutex> lk(_mutex);
        if (!_connected) {
            return true;
        }
        _cv.wait_until(lk, deadline.toSystemTimePoint(), [this] { return !_connected; });
        return !_connected;
    }

private:
    std::mutex _mutex;
    stdx::condition_variable _cv;
    bool _connected{true};
};

TEST_F(SessionManagerCommonTest, DisconnectShutdownAwareInterruptibleWakesOnClientDisconnect) {
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        MockSessionManagerCommon sm(getServiceContext());
        TransportLayerMock tl;
        auto session = std::make_shared<DisconnectableMockSession>(&tl);
        DisconnectShutdownAwareInterruptible interruptible{
            sm.getShutdownToken(), session.get(), getServiceContext()->getPreciseClockSource()};

        // Same single-token-burst setup as the shutdown test: the first throttle() consumes the
        // burst, the second parks in the tarpit wait.
        constexpr double kRefreshRatePerSec = 0.001;
        constexpr double kBurstCapacitySecs = 1000.0;  // burst = 1.0
        auto& limiter = admission::EgressResponseRateLimiter::get(getServiceContext());
        limiter.updateRateParameters(kRefreshRatePerSec, kBurstCapacitySecs);
        const auto baselineAddedToQueue = limiter.stats().addedToQueue();

        ASSERT_OK(limiter.throttle(&interruptible, getServiceContext()->getPreciseClockSource()));

        Notification<void> woke;
        std::vector<unittest::JoinThread> threads;
        threads.emplace_back(monitor.spawn([&]() {
            auto status =
                limiter.throttle(&interruptible, getServiceContext()->getPreciseClockSource());
            ASSERT_EQ(status.code(), ErrorCodes::ClientDisconnect)
                << "egress throttle() should be interrupted by client disconnect, not return OK or "
                   "another error";
            woke.set();
        }));

        // Wait until the spawned thread has enqueued and is parked in the tarpit wait, then flip
        // the peer to disconnected. The mock's waitForPeerDisconnectUntil is woken by the CV
        // notification, mirroring a real poll(2) waking on POLLRDHUP.
        const auto parkDeadline = Date_t::now() + Seconds(30);
        while (limiter.stats().addedToQueue() == baselineAddedToQueue ||
               !interruptible.isWaitingForConditionOrInterrupt()) {
            if (Date_t::now() >= parkDeadline) {
                break;
            }
            sleepmillis(1);
        }
        ASSERT_GT(limiter.stats().addedToQueue(), baselineAddedToQueue)
            << "throttle() never enqueued a waiter";
        ASSERT(interruptible.isWaitingForConditionOrInterrupt());

        session->setConnected(false);
        woke.get();
    });
}

}  // namespace
}  // namespace mongo::transport
