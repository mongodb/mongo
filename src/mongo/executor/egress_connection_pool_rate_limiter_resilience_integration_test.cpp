/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

/**
 * Integration tests verifying the egress connection pool's behavior when the server-side
 * ingress rate limiter rejects or queues new connections.
 *
 * When established connections already exist in the pool, background establishment failures
 * trigger the pool's refresh boolean, so the pool's acquisition queue is never cleared
 * and all pending requests eventually succeed.
 *
 * When NO established connections exist, the error propagates to all pending requests.
 */

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/executor_integration_test_connection_stats.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::executor {
namespace {

class EgressPoolRateLimiterResilienceTest : public NetworkInterfaceIntegrationFixture {
public:
    void setUp() override {
        auto opts = makeDefaultConnectionPoolOptions();
        opts.hostTimeout = Hours{24};
        opts.refreshTimeout = Seconds{5};
        setConnectionPoolOptions(opts);

        startNet();
    }

    void tearDown() override {
        disableRateLimiter();
        NetworkInterfaceIntegrationFixture::tearDown();
    }

    const AsyncClientFactory& getFactory() {
        return checked_cast<NetworkInterfaceTL&>(net()).getClientFactory_forTest();
    }

    RemoteCommandRequest makeRequest(BSONObj cmd,
                                     Milliseconds timeout = RemoteCommandRequest::kNoTimeout) {
        return RemoteCommandRequest(
            getServer(), DatabaseName::kAdmin, std::move(cmd), BSONObj(), nullptr, timeout);
    }

    void enableRateLimiter(int maxQueueDepth) {
        runSetupCommandSync(
            DatabaseName::kAdmin,
            BSON("setParameter" << 1 << "ingressConnectionEstablishmentRateLimiterEnabled" << true
                                << "ingressConnectionEstablishmentRatePerSec" << 1
                                << "ingressConnectionEstablishmentBurstCapacitySecs" << 1
                                << "ingressConnectionEstablishmentMaxQueueDepth" << maxQueueDepth));
    }

    void disableRateLimiter() {
        runSetupCommandSync(DatabaseName::kAdmin,
                            BSON("setParameter"
                                 << 1 << "ingressConnectionEstablishmentRateLimiterEnabled"
                                 << false));
    }

    void waitForRateLimiterStat(std::function<bool(const BSONObj&)> pred, StringData description) {
        const auto deadline = Date_t::now() + Seconds{30};
        while (Date_t::now() < deadline) {
            auto status = runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
            const auto& rl = status["connections"]["establishmentRateLimit"];
            if (pred(rl.Obj())) {
                return;
            }
            sleepmillis(100);
        }
        FAIL(fmt::format("Timed out waiting for rate-limiter stat: {}", description));
    }

    std::vector<Future<RemoteCommandResponse>> sendPings(int count, Milliseconds timeout) {
        std::vector<Future<RemoteCommandResponse>> futures;
        for (int i = 0; i < count; ++i) {
            futures.push_back(
                runCommand(makeCallbackHandle(), makeRequest(BSON("ping" << 1), timeout)));
        }
        return futures;
    }

    // Establishes one connection, blocks it in-use, runs `body`, then cleans up.
    void withEstablishedConnection(std::function<void()> body) {
        assertCommandOK(DatabaseName::kAdmin, BSON("ping" << 1));
        assertConnectionStatsSoon(
            getFactory(),
            getServer(),
            [](const ConnectionStatsPer& s) { return s.available >= 1; },
            [](const GRPCConnectionStats&) { return false; },
            "Expected >= 1 available connection after initial ping");

        auto blockFP = configureFailPoint(
            "failCommand", BSON("failCommands" << BSON_ARRAY("echo") << "blockConnection" << true));
        auto blockedFuture = runCommand(makeCallbackHandle(), makeRequest(BSON("echo" << 1)));

        assertConnectionStatsSoon(
            getFactory(),
            getServer(),
            [](const ConnectionStatsPer& s) { return s.inUse >= 1; },
            [](const GRPCConnectionStats&) { return false; },
            "Expected >= 1 in-use connection from blocked echo");

        body();

        blockFP.disable();
        (void)blockedFuture.getNoThrow(interruptible());
        assertCommandOK(DatabaseName::kAdmin, BSON("ping" << 1));
    }

    void assertPoolHasEstablishedConnection(StringData context) {
        assertConnectionStats(
            getFactory(),
            getServer(),
            [](const ConnectionStatsPer& s) { return s.inUse + s.available + s.leased >= 1; },
            [](const GRPCConnectionStats&) { return false; },
            context);
    }

    void assertAllRequestsSucceeded(std::vector<Future<RemoteCommandResponse>>& futures) {
        for (auto& f : futures) {
            auto res = f.getNoThrow(interruptible());
            ASSERT_OK(res.getStatus()) << "Request failed at transport level";
            ASSERT_OK(res.getValue().status) << "Request failed at command level";
        }
    }

    void assertAllRequestsFailed(std::vector<Future<RemoteCommandResponse>>& futures) {
        for (auto& f : futures) {
            auto res = f.getNoThrow(interruptible());
            Status failStatus = res.isOK() ? res.getValue().status : res.getStatus();
            ASSERT_EQ(failStatus.code(), ErrorCodes::HostUnreachable)
                << "Expected HostUnreachable, got: " << failStatus;
        }
    }
};

#define SKIP_ON_GRPC_RATE_LIMITER()                                                  \
    if (unittest::shouldUseGRPCEgress()) {                                           \
        LOGV2(1196901, "Skipping rate-limiter test: not applicable to gRPC egress"); \
        return;                                                                      \
    }

// ---------------------------------------------------------------------------
// With an established connection: requests survive rate-limiter rejections.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, RejectionWithEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();

    withEstablishedConnection([&] {
        enableRateLimiter(/*maxQueueDepth=*/1);
        auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

        auto futures = sendPings(6, Minutes{10});

        waitForRateLimiterStat([](const BSONObj& rl) { return rl["rejected"].numberLong() >= 1; },
                               "rejected >= 1");
        assertPoolHasEstablishedConnection(
            "Pool should still have >= 1 established connection after rejections");

        hangFP.disable();
        disableRateLimiter();
        assertAllRequestsSucceeded(futures);
    });
}

// ---------------------------------------------------------------------------
// With an established connection: requests survive rate-limiter queue timeouts.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, TimeoutWithEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();

    withEstablishedConnection([&] {
        enableRateLimiter(/*maxQueueDepth=*/10);
        auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

        auto futures = sendPings(3, Minutes{10});

        waitForRateLimiterStat(
            [](const BSONObj& rl) {
                return rl["interruptedDueToClientDisconnect"].numberLong() >= 1;
            },
            "interruptedDueToClientDisconnect >= 1");
        assertPoolHasEstablishedConnection(
            "Pool should still have >= 1 established connection after queue timeouts");

        hangFP.disable();
        disableRateLimiter();
        assertAllRequestsSucceeded(futures);
    });
}

// ---------------------------------------------------------------------------
// Without an established connection: rejections propagate to pending requests.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, RejectionWithNoEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();

    enableRateLimiter(/*maxQueueDepth=*/1);
    auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

    auto futures = sendPings(6, Seconds{30});

    waitForRateLimiterStat([](const BSONObj& rl) { return rl["rejected"].numberLong() >= 1; },
                           "rejected >= 1");

    hangFP.disable();
    disableRateLimiter();
    assertAllRequestsFailed(futures);
}

// ---------------------------------------------------------------------------
// Without an established connection: queue timeouts propagate to pending requests.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, TimeoutWithNoEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();

    enableRateLimiter(/*maxQueueDepth=*/10);
    auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

    auto futures = sendPings(3, Seconds{30});

    waitForRateLimiterStat(
        [](const BSONObj& rl) { return rl["interruptedDueToClientDisconnect"].numberLong() >= 1; },
        "interruptedDueToClientDisconnect >= 1");

    hangFP.disable();
    disableRateLimiter();
    assertAllRequestsFailed(futures);
}

}  // namespace
}  // namespace mongo::executor
