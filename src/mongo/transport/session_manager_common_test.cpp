// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session_manager_common.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <cerrno>

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

}  // namespace
}  // namespace mongo::transport
