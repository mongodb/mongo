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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
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
#include <string>
#include <vector>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::executor {
namespace {

class EgressPoolRateLimiterResilienceTest : public NetworkInterfaceIntegrationFixture {
public:
    void setUp() override {
        auto opts = makeDefaultConnectionPoolOptions();
        opts.hostTimeout = Hours{24};
        opts.refreshTimeout = Seconds{5};
        _poolOptionsSnapshotForDiagnostics = opts;
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

    /**
     * Returns the current establishmentRateLimit section from serverStatus, or an empty BSONObj
     * if not present.
     */
    BSONObj getRateLimiterStats() {
        auto serverStatus = runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
        if (serverStatus.hasField("connections")) {
            auto connections = serverStatus["connections"].Obj();
            if (connections.hasField("establishmentRateLimit")) {
                return connections["establishmentRateLimit"].Obj().getOwned();
            }
        }
        return BSONObj();
    }

    void waitForRateLimiterStat(std::function<bool(const BSONObj&)> pred, StringData description) {
        const auto deadline = Date_t::now() + Seconds{30};
        int pollIterations = 0;
        BSONObj lastServerStatus;
        while (Date_t::now() < deadline) {
            ++pollIterations;
            lastServerStatus = runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
            if (lastServerStatus.hasField("connections")) {
                const BSONObj connections = lastServerStatus["connections"].Obj();
                if (connections.hasField("establishmentRateLimit")) {
                    const BSONObj rl = connections["establishmentRateLimit"].Obj();
                    if (pred(rl)) {
                        return;
                    }
                }
            }
            sleepmillis(100);
        }

        const std::string rlJson = lastServerStatus.hasField("connections") &&
                lastServerStatus["connections"].Obj().hasField("establishmentRateLimit")
            ? lastServerStatus["connections"]["establishmentRateLimit"].Obj().jsonString()
            : std::string("(missing establishmentRateLimit)");
        const std::string connectionsJson = lastServerStatus.hasField("connections")
            ? lastServerStatus["connections"].Obj().jsonString()
            : std::string("(missing connections)");
        const std::string poolStatsJson = _dumpEgressConnectionPoolStatsJson();

        LOGV2_ERROR(1196902,
                    "EgressPoolRateLimiterResilienceTest timed out waiting for rate limiter stat",
                    "description"_attr = description,
                    "pollIterations"_attr = pollIterations,
                    "grpcEgress"_attr = unittest::shouldUseGRPCEgress(),
                    "establishmentRateLimit"_attr = rlJson,
                    "connectionsSectionChars"_attr = connectionsJson.size());
        FAIL(
            fmt::format("Timed out waiting for rate-limiter stat: {}. "
                        "pollIterations={} grpcEgress={} "
                        "poolOptionsSnapshot: {} "
                        "lastEstablishmentRateLimit: {} "
                        "lastServerStatus.connections: {} "
                        "egressConnectionPoolStats: {}",
                        description,
                        pollIterations,
                        unittest::shouldUseGRPCEgress(),
                        _dumpConnectionPoolOptionsSnapshot(),
                        rlJson,
                        connectionsJson,
                        poolStatsJson));
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
        for (size_t i = 0; i < futures.size(); ++i) {
            auto res = futures[i].getNoThrow(interruptible());
            if (!res.isOK()) {
                const BSONObj lastServerStatus =
                    runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
                LOGV2_ERROR(1196903,
                            "EgressPoolRateLimiterResilienceTest assertAllRequestsSucceeded failed "
                            "(transport)",
                            "index"_attr = i,
                            "status"_attr = res.getStatus().toString());
                FAIL(
                    fmt::format("Request failed at transport level (index={}): {}. "
                                "Diagnostics: grpcEgress={} poolOptions={} "
                                "serverStatus.connections.establishmentRateLimit={} "
                                "egressConnectionPoolStats={}",
                                i,
                                res.getStatus().toString(),
                                unittest::shouldUseGRPCEgress(),
                                _dumpConnectionPoolOptionsSnapshot(),
                                _dumpEstablishmentRateLimitJson(lastServerStatus),
                                _dumpEgressConnectionPoolStatsJson()));
            }
            if (!res.getValue().status.isOK()) {
                const BSONObj lastServerStatus =
                    runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
                LOGV2_ERROR(1196904,
                            "EgressPoolRateLimiterResilienceTest assertAllRequestsSucceeded failed "
                            "(response status)",
                            "index"_attr = i,
                            "response"_attr = res.getValue().toString());
                FAIL(
                    fmt::format("Request failed at response status (index={}): {}. "
                                "Full response: {}. "
                                "Diagnostics: grpcEgress={} poolOptions={} "
                                "serverStatus.connections.establishmentRateLimit={} "
                                "egressConnectionPoolStats={}",
                                i,
                                res.getValue().status.toString(),
                                res.getValue().toString(),
                                unittest::shouldUseGRPCEgress(),
                                _dumpConnectionPoolOptionsSnapshot(),
                                _dumpEstablishmentRateLimitJson(lastServerStatus),
                                _dumpEgressConnectionPoolStatsJson()));
            }
        }
    }

    void assertAllRequestsFailed(std::vector<Future<RemoteCommandResponse>>& futures) {
        std::vector<StatusWith<RemoteCommandResponse>> results;
        results.reserve(futures.size());
        for (auto& f : futures) {
            results.push_back(f.getNoThrow(interruptible()));
        }

        for (size_t i = 0; i < results.size(); ++i) {
            const auto& res = results[i];
            const Status failStatus = res.isOK() ? res.getValue().status : res.getStatus();
            if (failStatus.code() == ErrorCodes::HostUnreachable) {
                continue;
            }

            const BSONObj lastServerStatus =
                runSetupCommandSync(DatabaseName::kAdmin, BSON("serverStatus" << 1));
            std::string allResponses;
            for (size_t j = 0; j < results.size(); ++j) {
                const auto& rj = results[j];
                if (rj.isOK()) {
                    allResponses += fmt::format(
                        "[{}] OK transport; response: {}\n", j, rj.getValue().toString());
                } else {
                    allResponses +=
                        fmt::format("[{}] StatusWith error: {}\n", j, rj.getStatus().toString());
                }
            }
            LOGV2_ERROR(1196905,
                        "EgressPoolRateLimiterResilienceTest assertAllRequestsFailed mismatch",
                        "index"_attr = i,
                        "expected"_attr = ErrorCodes::HostUnreachable,
                        "actual"_attr = failStatus.toString());
            FAIL(
                fmt::format("Expected HostUnreachable for all requests; mismatch at index={}. "
                            "Got: {}. "
                            "All futures: ---\n{}---\n"
                            "Diagnostics: grpcEgress={} poolOptions={} "
                            "serverStatus.connections.establishmentRateLimit={} "
                            "egressConnectionPoolStats={}",
                            i,
                            failStatus.toString(),
                            allResponses,
                            unittest::shouldUseGRPCEgress(),
                            _dumpConnectionPoolOptionsSnapshot(),
                            _dumpEstablishmentRateLimitJson(lastServerStatus),
                            _dumpEgressConnectionPoolStatsJson()));
        }
    }

private:
    /**
     * Snapshot of pool options from setUp; included in failure diagnostics only.
     */
    ConnectionPool::Options _poolOptionsSnapshotForDiagnostics{};

    std::string _dumpConnectionPoolOptionsSnapshot() const {
        const auto& o = _poolOptionsSnapshotForDiagnostics;
        return fmt::format(
            "minConnections={} maxConnections={} maxConnecting={} "
            "refreshTimeout={}ms refreshRequirement={}ms hostTimeout={}ms",
            o.minConnections,
            o.maxConnections,
            o.maxConnecting,
            o.refreshTimeout.count(),
            o.refreshRequirement.count(),
            o.hostTimeout.count());
    }

    std::string _dumpEgressConnectionPoolStatsJson() {
        ConnectionPoolStats stats;
        net().appendConnectionStats(&stats);
        BSONObjBuilder bob;
        stats.appendToBSON(bob);
        return bob.obj().jsonString();
    }

    static std::string _dumpEstablishmentRateLimitJson(const BSONObj& serverStatus) {
        if (!serverStatus.hasField("connections")) {
            return "(serverStatus has no connections)";
        }
        const BSONObj connections = serverStatus["connections"].Obj();
        if (!connections.hasField("establishmentRateLimit")) {
            return "(connections has no establishmentRateLimit)";
        }
        return connections["establishmentRateLimit"].Obj().jsonString();
    }
};

#define SKIP_ON_GRPC_RATE_LIMITER()                                                  \
    if (unittest::shouldUseGRPCEgress()) {                                           \
        LOGV2(1196901, "Skipping rate-limiter test: not applicable to gRPC egress"); \
        return;                                                                      \
    }

#ifndef __linux__
#define SKIP_ON_NON_LINUX()                                                                 \
    LOGV2(1196906,                                                                          \
          "Skipping test: client disconnect detection requires Linux epoll-based polling"); \
    return;
#else
#define SKIP_ON_NON_LINUX()
#endif

// ---------------------------------------------------------------------------
// With an established connection: requests survive rate-limiter rejections.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, RejectionWithEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();

    withEstablishedConnection([&] {
        auto baseline = getRateLimiterStats();
        auto baselineRejected = baseline.isEmpty() ? 0 : baseline["rejected"].numberLong();

        enableRateLimiter(/*maxQueueDepth=*/1);
        auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

        auto futures = sendPings(6, Minutes{10});

        waitForRateLimiterStat(
            [&](const BSONObj& rl) { return rl["rejected"].numberLong() > baselineRejected; },
            "rejected increased from baseline");
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
    SKIP_ON_NON_LINUX();

    withEstablishedConnection([&] {
        auto baseline = getRateLimiterStats();
        auto baselineInterrupted =
            baseline.isEmpty() ? 0 : baseline["interruptedDueToClientDisconnect"].numberLong();

        enableRateLimiter(/*maxQueueDepth=*/10);
        auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

        auto futures = sendPings(3, Minutes{10});

        waitForRateLimiterStat(
            [&](const BSONObj& rl) {
                return rl["interruptedDueToClientDisconnect"].numberLong() > baselineInterrupted;
            },
            "interruptedDueToClientDisconnect increased from baseline");
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

    auto baseline = getRateLimiterStats();
    auto baselineRejected = baseline.isEmpty() ? 0 : baseline["rejected"].numberLong();

    enableRateLimiter(/*maxQueueDepth=*/1);
    auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

    auto futures = sendPings(6, Seconds{30});

    waitForRateLimiterStat(
        [&](const BSONObj& rl) { return rl["rejected"].numberLong() > baselineRejected; },
        "rejected increased from baseline");

    hangFP.disable();
    disableRateLimiter();
    assertAllRequestsFailed(futures);
}

// ---------------------------------------------------------------------------
// Without an established connection: queue timeouts propagate to pending requests.
// ---------------------------------------------------------------------------
TEST_F(EgressPoolRateLimiterResilienceTest, TimeoutWithNoEstablishedConnection) {
    SKIP_ON_GRPC_RATE_LIMITER();
    SKIP_ON_NON_LINUX();

    auto baseline = getRateLimiterStats();
    auto baselineInterrupted =
        baseline.isEmpty() ? 0 : baseline["interruptedDueToClientDisconnect"].numberLong();

    enableRateLimiter(/*maxQueueDepth=*/10);
    auto hangFP = configureFailPoint("hangInRateLimiter", BSONObj());

    auto futures = sendPings(3, Seconds{30});

    waitForRateLimiterStat(
        [&](const BSONObj& rl) {
            return rl["interruptedDueToClientDisconnect"].numberLong() > baselineInterrupted;
        },
        "interruptedDueToClientDisconnect increased from baseline");

    hangFP.disable();
    disableRateLimiter();
    assertAllRequestsFailed(futures);
}

}  // namespace
}  // namespace mongo::executor
