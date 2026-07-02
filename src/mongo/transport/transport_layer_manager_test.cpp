/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/periodic_runner_factory.h"

#include <memory>

namespace mongo::transport {
namespace {

class TransportLayerManagerTest : public unittest::Test {
public:
    class TransportLayerMockWithConnect : public TransportLayerMock {
    public:
        StatusWith<std::shared_ptr<Session>> connect(
            HostAndPort peer,
            ConnectSSLMode sslMode,
            Milliseconds timeout,
            const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override {
            return createSession();
        }

        bool isStarted() const {
            return _started;
        }

        Status start() override {
            uassert(7402201,
                    "cannot start a transport layer more than once",
                    !std::exchange(_started, true));
            return Status::OK();
        }

    private:
        bool _started = false;
    };

    TransportLayerManagerImpl makeTLManager() {
        std::vector<TransportLayerMockWithConnect*> layerPtrs;
        std::vector<std::unique_ptr<TransportLayer>> layers;
        for (int i = 0; i < 5; i++) {
            auto layer = std::make_unique<TransportLayerMockWithConnect>();
            layerPtrs.push_back(layer.get());
            layers.push_back(std::move(layer));
        }

        return TransportLayerManagerImpl(std::move(layers), layerPtrs[0]);
    }

    std::vector<const TransportLayerMockWithConnect*> getMockTransportLayers(
        const TransportLayerManagerImpl& mgr) {
        std::vector<const TransportLayerMockWithConnect*> layerPtrs;
        for (auto&& tl : mgr.getTransportLayers()) {
            layerPtrs.push_back(dynamic_cast<const TransportLayerMockWithConnect*>(tl.get()));
        }
        return layerPtrs;
    }
};

TEST_F(TransportLayerManagerTest, StartAndShutdown) {
    auto manager = makeTLManager();
    ASSERT_OK(manager.setup());
    ASSERT_OK(manager.start());
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->isStarted());
    }

    manager.shutdown();
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->inShutdown());
    }
}

TEST_F(TransportLayerManagerTest, ShutdownBeforeSetup) {
    auto manager = makeTLManager();
    manager.shutdown();
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->inShutdown());
    }
    ASSERT_NOT_OK(manager.setup());
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->inShutdown());
    }
}

TEST_F(TransportLayerManagerTest, ShutdownAfterSetup) {
    auto manager = makeTLManager();
    ASSERT_OK(manager.setup());
    manager.shutdown();
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->inShutdown());
    }
    ASSERT_NOT_OK(manager.start());
    for (auto layer : getMockTransportLayers(manager)) {
        ASSERT_TRUE(layer->inShutdown());
    }
}

DEATH_TEST(PortsTestDeathTest,
           ShouldFailIfMainAndPriorityPortsCollide,
           "Port collision, ports must be unique.") {
    serverGlobalParams.port = 20017;
    serverGlobalParams.priorityPort = 20017;

    gFeatureFlagDedicatedPortForPriorityOperations.setForServerParameter(true);

    auto svcCtx = ServiceContext::make();
    svcCtx->setPeriodicRunner(makePeriodicRunner(svcCtx.get()));
    svcCtx->getService()->setServiceEntryPoint(
        std::make_unique<test::ServiceEntryPointUnimplemented>());

    std::ignore = TransportLayerManagerImpl::make(svcCtx.get(), true);
}

DEATH_TEST(PortsTestDeathTest,
           ShouldFailIfMainAndProxyPortsCollide,
           "Port collision, ports must be unique.") {
    serverGlobalParams.port = 20017;
    serverGlobalParams.proxyPort = 20017;

    gFeatureFlagDedicatedPortForPriorityOperations.setForServerParameter(true);

    auto svcCtx = ServiceContext::make();
    svcCtx->setPeriodicRunner(makePeriodicRunner(svcCtx.get()));
    svcCtx->getService()->setServiceEntryPoint(
        std::make_unique<test::ServiceEntryPointUnimplemented>());

    std::ignore = TransportLayerManagerImpl::make(svcCtx.get(), true);
}

DEATH_TEST(PortsTestDeathTest,
           ShouldFailIfFeatureFlagIsNotEnabled,
           "Priority port support is not enabled") {
    serverGlobalParams.port = 27017;
    serverGlobalParams.priorityPort = 27018;

    gFeatureFlagDedicatedPortForPriorityOperations.setForServerParameter(false);

    auto svcCtx = ServiceContext::make();
    svcCtx->setPeriodicRunner(makePeriodicRunner(svcCtx.get()));
    svcCtx->getService()->setServiceEntryPoint(
        std::make_unique<test::ServiceEntryPointUnimplemented>());

    std::ignore = TransportLayerManagerImpl::make(svcCtx.get(), true);
}

TEST_F(TransportLayerManagerTest, ConnectEgressLayer) {
    std::vector<std::unique_ptr<TransportLayer>> layers;

    auto egress = std::make_unique<TransportLayerMockWithConnect>();
    auto egressPtr = egress.get();
    layers.push_back(std::move(egress));
    layers.push_back(std::make_unique<TransportLayerMock>());

    TransportLayerManagerImpl manager(std::move(layers), egressPtr);
    uassertStatusOK(manager.setup());
    uassertStatusOK(manager.start());
    auto swSession = manager.getDefaultEgressLayer()->connect(
        HostAndPort("localhost:1234"), ConnectSSLMode::kDisableSSL, Milliseconds(100), boost::none);
    ASSERT_OK(swSession);
    ASSERT_TRUE(egressPtr->owns(swSession.getValue()->id()));
}

class MockSessionEstablishmentRateLimiter : public SessionEstablishmentRateLimiter {
public:
    int64_t queued() const override {
        return numQueued;
    }
    int64_t rejected() const override {
        return numRejected;
    }
    int64_t exempted() const override {
        return numExempted;
    }
    int64_t interruptedDueToClientDisconnect() const override {
        return numInterrupted;
    }

    int64_t numQueued = 0;
    int64_t numRejected = 0;
    int64_t numExempted = 0;
    int64_t numInterrupted = 0;
};

class MockSessionManagerCommonServerStatusTest : public MockSessionManagerCommon {
public:
    explicit MockSessionManagerCommonServerStatusTest(ServiceContext* svcCtx, bool optsIn)
        : MockSessionManagerCommon(svcCtx), _optsIn(optsIn) {}

    bool shouldIncludeInConnectionsServerStatus() const override {
        return _optsIn;
    }

    SessionStats getSessionStats() const override {
        return stats;
    }

    SessionEstablishmentRateLimiter& getSessionEstablishmentRateLimiter() override {
        return rateLimiter;
    }

    SessionStats stats;
    MockSessionEstablishmentRateLimiter rateLimiter;

private:
    bool _optsIn;
};

class TransportLayerManagerServerStatusTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto svcCtx = getServiceContext();

        optInSessionManager0 = new MockSessionManagerCommonServerStatusTest(svcCtx, true);
        optInSessionManager1 = new MockSessionManagerCommonServerStatusTest(svcCtx, true);
        optOutSessionManager = new MockSessionManagerCommonServerStatusTest(svcCtx, false);

        auto transportLayer0 = std::make_unique<TransportLayerMock>(
            std::unique_ptr<MockSessionManagerCommonServerStatusTest>(optInSessionManager0));
        auto* transportLayerPtr0 = transportLayer0.get();

        std::vector<std::unique_ptr<TransportLayer>> layers;
        layers.push_back(std::move(transportLayer0));
        layers.push_back(std::make_unique<TransportLayerMock>(
            std::unique_ptr<MockSessionManagerCommonServerStatusTest>(optInSessionManager1)));
        layers.push_back(std::make_unique<TransportLayerMock>(
            std::unique_ptr<MockSessionManagerCommonServerStatusTest>(optOutSessionManager)));
        svcCtx->setTransportLayerManager(
            std::make_unique<TransportLayerManagerImpl>(std::move(layers), transportLayerPtr0));

        client = svcCtx->getService()->makeClient("test");
        opCtx = client->makeOperationContext();

        section = findConnectionsServerStatusSection();
        ASSERT(section);
    }

    BSONObj generateSection() {
        return section->generateSection(opCtx.get(), BSONElement{});
    }

protected:
    MockSessionManagerCommonServerStatusTest* optInSessionManager0 = nullptr;
    MockSessionManagerCommonServerStatusTest* optInSessionManager1 = nullptr;
    MockSessionManagerCommonServerStatusTest* optOutSessionManager = nullptr;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    ServerStatusSection* section = nullptr;

private:
    static ServerStatusSection* findConnectionsServerStatusSection() {
        for (auto it = ServerStatusSectionRegistry::instance()->begin();
             it != ServerStatusSectionRegistry::instance()->end();
             ++it) {
            if (it->second->getSectionName() == "connections") {
                return it->second.get();
            }
        }
        return nullptr;
    }
};

/**
 * Verifies the exact set of top-level fields produced by the Connections serverStatus section
 * in a default environment (no priority port, no ServiceExecutorReserved, feature flag on).
 */
TEST_F(TransportLayerManagerServerStatusTest, FieldCount) {
    auto section = generateSection();
    const std::set<std::string> expected{"current",
                                         "available",
                                         "totalCreated",
                                         "rejected",
                                         "active",
                                         "queuedForEstablishment",
                                         "establishmentRateLimit",
                                         "threaded",
                                         "exhaustIsMaster",
                                         "exhaustHello",
                                         "awaitingTopologyChanges",
                                         "loadBalanced",
                                         "priority"};
    const auto actual = section.getFieldNames<std::set<std::string>>();
    std::vector<std::string> diff;

    std::ranges::set_difference(expected, actual, std::back_inserter(diff));
    ASSERT_EQ(diff, std::vector<std::string>{}) << "fields missing from section";

    diff.clear();
    std::ranges::set_difference(actual, expected, std::back_inserter(diff));
    ASSERT_EQ(diff, std::vector<std::string>{}) << "unexpected fields in section";
}

/**
 * Verifies that current is the sum of open sessions across opted-in managers, and that
 * opted-out managers are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, Current) {
    optInSessionManager0->stats.numOpenSessions = 3;
    optInSessionManager1->stats.numOpenSessions = 4;
    optOutSessionManager->stats.numOpenSessions = 100;
    ASSERT_EQ(generateSection()["current"].Int(), 7);
}

/**
 * Verifies that available is max(maxOpenSessions) - sum(numOpenSessions) across opted-in managers,
 * and opted-out managers are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, Available) {
    optInSessionManager0->stats.maxOpenSessions = 1000;
    optInSessionManager0->stats.numOpenSessions = 3;

    optInSessionManager1->stats.maxOpenSessions = 1000;
    optInSessionManager1->stats.numOpenSessions = 4;

    optOutSessionManager->stats.maxOpenSessions = 1000;
    optOutSessionManager->stats.numOpenSessions = 100;

    ASSERT_EQ(generateSection()["available"].Int(), 993);
}

/**
 * Verifies that maxOpenSessions is the max across opted-in managers, and opted-out managers are
 * excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, MaxOpenSessionsIsMax) {
    optInSessionManager0->stats.maxOpenSessions = 800;
    optInSessionManager0->stats.numOpenSessions = 3;

    optInSessionManager1->stats.maxOpenSessions = 1000;
    optInSessionManager1->stats.numOpenSessions = 4;

    optOutSessionManager->stats.maxOpenSessions = 2000;
    optOutSessionManager->stats.numOpenSessions = 100;

    ASSERT_EQ(generateSection()["available"].Int(), 993);
}

/**
 * Verifies that totalCreated is summed across opted-in managers and opted-out managers are
 * excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, TotalCreated) {
    optInSessionManager0->stats.numCreatedSessions = 10;
    optInSessionManager1->stats.numCreatedSessions = 20;
    optOutSessionManager->stats.numCreatedSessions = 999;
    ASSERT_EQ(generateSection()["totalCreated"].Int(), 30);
}

/**
 * Verifies that rejected is summed across opted-in managers and opted-out managers are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, Rejected) {
    optInSessionManager0->stats.numRejectedSessions = 2;
    optInSessionManager1->stats.numRejectedSessions = 3;
    optOutSessionManager->stats.numRejectedSessions = 999;
    ASSERT_EQ(generateSection()["rejected"].Int(), 5);
}

/**
 * Verifies that active is summed across opted-in managers and opted-out managers are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, Active) {
    optInSessionManager0->stats.numActiveOperations = 1;
    optInSessionManager1->stats.numActiveOperations = 2;
    optOutSessionManager->stats.numActiveOperations = 999;
    ASSERT_EQ(generateSection()["active"].Int(), 3);
}

/**
 * Verifies that queuedForEstablishment metrics are summed across opted-in managers and opted-out
 * managers are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, RateLimiter) {
    optInSessionManager0->rateLimiter.numQueued = 2;
    optInSessionManager0->rateLimiter.numExempted = 1;
    optInSessionManager0->rateLimiter.numRejected = 3;
    optInSessionManager0->rateLimiter.numInterrupted = 4;

    optInSessionManager1->rateLimiter.numQueued = 1;
    optInSessionManager1->rateLimiter.numExempted = 3;
    optInSessionManager1->rateLimiter.numRejected = 2;
    optInSessionManager1->rateLimiter.numInterrupted = 4;

    optOutSessionManager->rateLimiter.numQueued = 9;
    optOutSessionManager->rateLimiter.numExempted = 9;
    optOutSessionManager->rateLimiter.numRejected = 9;
    optOutSessionManager->rateLimiter.numInterrupted = 9;

    auto section = generateSection();
    ASSERT_EQ(section["queuedForEstablishment"].Int(), 3);
    auto rateLimit = section["establishmentRateLimit"].Obj();
    ASSERT_EQ(rateLimit["exempted"].Int(), 4);
    ASSERT_EQ(rateLimit["rejected"].Int(), 5);
    ASSERT_EQ(rateLimit["interruptedDueToClientDisconnect"].Int(), 8);
}

/**
 * Verifies that threaded mirrors current (all sessions are threaded).
 */
TEST_F(TransportLayerManagerServerStatusTest, Threaded) {
    optInSessionManager0->stats.numOpenSessions = 3;
    optInSessionManager1->stats.numOpenSessions = 4;
    optOutSessionManager->stats.numOpenSessions = 100;
    auto section = generateSection();
    ASSERT_EQ(section["threaded"].Int(), section["current"].Int());
    ASSERT_EQ(section["threaded"].Int(), 7);
}

/**
 * Verifies that limitExempt is omitted without a priority port and reflects the summed count
 * across opted-in managers when one is configured. Opted-out manager counts are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, LimitExempt) {
    optInSessionManager0->serviceExecutorStats.limitExempt.store(2);
    optInSessionManager1->serviceExecutorStats.limitExempt.store(1);
    optOutSessionManager->serviceExecutorStats.limitExempt.store(99);

    ASSERT_FALSE(generateSection().hasField("limitExempt"));

    serverGlobalParams.priorityPort = 27018;
    ON_BLOCK_EXIT([] { serverGlobalParams.priorityPort = boost::none; });
    ASSERT_EQ(generateSection()["limitExempt"].Int(), 3);
}

/**
 * Verifies that HelloMetrics is summed across opted-in managers and opted-out managers
 * are excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, HelloMetrics) {
    optInSessionManager0->helloMetrics.incrementNumAwaitingTopologyChanges();
    optInSessionManager0->helloMetrics.incrementNumAwaitingTopologyChanges();
    optInSessionManager1->helloMetrics.incrementNumAwaitingTopologyChanges();
    optOutSessionManager->helloMetrics.incrementNumAwaitingTopologyChanges();
    optOutSessionManager->helloMetrics.incrementNumAwaitingTopologyChanges();
    optOutSessionManager->helloMetrics.incrementNumAwaitingTopologyChanges();
    optOutSessionManager->helloMetrics.incrementNumAwaitingTopologyChanges();
    ASSERT_EQ(generateSection()["awaitingTopologyChanges"].Long(), 3);
}

/**
 * Verifies that adminConnections is absent when ServiceExecutorReserved is not installed, and
 * present with the expected sub-fields when it is (reservedAdminThreads > 0 at ServiceContext
 * construction time).
 */
TEST(TransportLayerManagerAdminConnectionsTest, AdminConnections) {
    auto findSection = [] {
        for (auto it = ServerStatusSectionRegistry::instance()->begin();
             it != ServerStatusSectionRegistry::instance()->end();
             ++it) {
            if (it->second->getSectionName() == "connections")
                return it->second.get();
        }
        return static_cast<ServerStatusSection*>(nullptr);
    };

    auto generateSection = [&](int reservedAdminThreads) -> BSONObj {
        ON_BLOCK_EXIT([&] { serverGlobalParams.reservedAdminThreads = 0; });
        serverGlobalParams.reservedAdminThreads = reservedAdminThreads;

        auto svcCtx = ServiceContext::make();
        auto sm = new MockSessionManagerCommonServerStatusTest(svcCtx.get(), true);
        auto tl = std::make_unique<TransportLayerMock>(
            std::unique_ptr<MockSessionManagerCommonServerStatusTest>(sm));
        auto* tlPtr = tl.get();
        std::vector<std::unique_ptr<TransportLayer>> layers;
        layers.push_back(std::move(tl));
        svcCtx->setTransportLayerManager(
            std::make_unique<TransportLayerManagerImpl>(std::move(layers), tlPtr));

        auto client = svcCtx->getService()->makeClient("test");
        auto opCtx = client->makeOperationContext();
        return findSection()->generateSection(opCtx.get(), BSONElement{});
    };

    ASSERT_FALSE(generateSection(/*reservedAdminThreads=*/0).hasField("adminConnections"));

    auto section = generateSection(/*reservedAdminThreads=*/1);
    auto adminConns = section["adminConnections"].Obj();
    auto reserved = adminConns["reserved"].Obj();
    ASSERT_EQ(reserved["threadsRunning"].Int(), 0);
    ASSERT_EQ(reserved["clientsInTotal"].Int(), 0);
    ASSERT_EQ(reserved["clientsRunning"].Int(), 0);
    ASSERT_EQ(reserved["clientsWaitingForData"].Int(), 0);
}

/**
 * Verifies that loadBalanced is summed across opted-in managers and opted-out managers are
 * excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, LoadBalanced) {
    optInSessionManager0->stats.numLoadBalancedSessions = 2;
    optInSessionManager1->stats.numLoadBalancedSessions = 1;
    optOutSessionManager->stats.numLoadBalancedSessions = 4;
    ASSERT_EQ(generateSection()["loadBalanced"].Int(), 3);
}

/**
 * Verifies that priority is omitted when the feature flag is disabled and reflects the summed
 * count across opted-in managers when it is enabled (the default). Opted-out manager counts are
 * excluded.
 */
TEST_F(TransportLayerManagerServerStatusTest, Priority) {
    optInSessionManager0->stats.numPrioritySessions = 2;
    optInSessionManager1->stats.numPrioritySessions = 1;
    optOutSessionManager->stats.numPrioritySessions = 4;

    {
        unittest::ServerParameterGuard guard{"featureFlagDedicatedPortForPriorityOperations",
                                             false};
        ASSERT_FALSE(generateSection().hasField("priority"));
    }

    ASSERT_EQ(generateSection()["priority"].Int(), 3);
}

}  // namespace
}  // namespace mongo::transport
