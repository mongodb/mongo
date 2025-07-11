/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/s/router_role.h"

#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
class RouterRoleTest : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();

        const auto opCtx = operationContext();
        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        _routerOpCtxSession.emplace(opCtx);

        // The service needs to be set to the RouterServer service so that the "insert" command can
        // be found when attaching txn request fields for the tests in this file.
        auto targetService =
            operationContext()->getServiceContext()->getService(ClusterRole::RouterServer);
        operationContext()->getClient()->setService(targetService);

        _nss = NamespaceString::createNamespaceString_forTest("test.foo");
        setupNShards(2);
        loadRoutingTableWithTwoChunksAndTwoShards(_nss);
    }

    void tearDown() override {
        _routerOpCtxSession.reset();
        RouterCatalogCacheTestFixture::tearDown();
    }

    void actAsSubRouter() {
        // Attach txn request fields for a request to shard0 so that the shard acts as a sub-router.
        TxnNumber txnNum{3};
        operationContext()->setTxnNumber(txnNum);
        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(
            BSON("insert" << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                          << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                                  << LogicalTime(Timestamp(3, 1)).asTimestamp()
                                  << repl::ReadConcernArgs::kLevelFieldName << "snapshot"))));
        repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;

        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), txnNum, TransactionRouter::TransactionActions::kStartOrContinue);
        txnRouter.attachTxnFieldsIfNeeded(
            operationContext(), ShardId{"0"}, BSON("insert" << "test"));
    }

    void mockConfigServerQueries(NamespaceString _nss, OID epoch, Timestamp timestamp) {
        // Mock the expected config server queries.
        ChunkVersion version({epoch, timestamp}, {2, 0});
        ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
        UUID uuid{UUID::gen()};

        ChunkType chunk1(
            uuid, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});
        chunk1.setName(OID::gen());
        version.incMinor();

        ChunkType chunk2(
            uuid, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        expectCollectionAndChunksAggregation(
            _nss, epoch, timestamp, uuid, shardKeyPattern, {chunk1, chunk2});
    }

protected:
    NamespaceString _nss;

private:
    boost::optional<RouterOperationContextSession> _routerOpCtxSession;
};

TEST_F(RouterRoleTest, DBPrimaryRouterDoesNotRetryForSubRouter) {
    actAsSubRouter();

    // The DBPrimaryRouter should not retry if the shard acted as a sub-router.
    int tries = 0;
    sharding::router::DBPrimaryRouter router(getServiceContext(), _nss.dbName());
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         tries++;
                         uasserted(
                             StaleDbRoutingVersion(_nss.dbName(),
                                                   DatabaseVersion(UUID::gen(), Timestamp(2, 3)),
                                                   DatabaseVersion(UUID::gen(), Timestamp(5, 3))),
                             "staleDbVersion");
                     }),
        DBException,
        ErrorCodes::StaleDbVersion);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, DBPrimaryRouterDoesNotRetryOnStaleDbVersionForNonSubRouter) {
    // The DBPrimaryRouter should not retry on StaleDbVersion error if the shard did not act
    // as a sub-router.
    int tries = 0;
    sharding::router::DBPrimaryRouter router(getServiceContext(), _nss.dbName());
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         tries++;
                         uasserted(
                             StaleDbRoutingVersion(_nss.dbName(),
                                                   DatabaseVersion(UUID::gen(), Timestamp(0, 0)),
                                                   DatabaseVersion(UUID::gen(), Timestamp(0, 0))),
                             "staleDbVersion");
                     }),
        DBException,
        ErrorCodes::StaleDbVersion);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, CollectionRouterDoesNotRetryForSubRouter) {
    actAsSubRouter();

    // The CollectionRouter should not retry if the shard acted as a sub-router.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         tries++;
                         OID epoch{OID::gen()};
                         Timestamp timestamp{1, 0};
                         uasserted(StaleConfigInfo(_nss,
                                                   ShardVersionFactory::make(
                                                       ChunkVersion({epoch, timestamp}, {2, 0})),
                                                   boost::none,
                                                   ShardId{"0"}),
                                   "StaleConfig error");
                     }),
        DBException,
        ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, CollectionRouterDoesNotRetryOnStaleConfigForNonSubRouter) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should not retry on StaleConfig error if the shard did not act as
    // a sub-router.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         tries++;
                         uasserted(StaleConfigInfo(_nss,
                                                   ShardVersionFactory::make(
                                                       ChunkVersion({epoch, timestamp}, {2, 0})),
                                                   boost::none,
                                                   ShardId{"0"}),
                                   "StaleConfig error");
                     }),
        DBException,
        ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, CollectionRouterWithRoutingContextDoesNotRetryForSubRouter) {
    actAsSubRouter();

    // The CollectionRouter should not retry if the shard acted as a sub-router.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(router.routeWithRoutingContext(
                           operationContext(),
                           "test",
                           [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                               tries++;
                               OID epoch{OID::gen()};
                               Timestamp timestamp{1, 0};
                               uasserted(StaleConfigInfo(_nss,
                                                         ShardVersionFactory::make(ChunkVersion(
                                                             {epoch, timestamp}, {2, 0})),
                                                         boost::none,
                                                         ShardId{"0"}),
                                         "StaleConfig error");
                           }),
                       DBException,
                       ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, CollectionRouterWithRoutingContextDoesNotRetryOnStaleConfigForNonSubRouter) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should not retry on StaleConfig error if the shard did not act as
    // a sub-router.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(
        // auto future = launchAsync([&] {
        router.routeWithRoutingContext(
            operationContext(),
            "test",
            [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                tries++;
                uasserted(StaleConfigInfo(
                              _nss,
                              ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0})),
                              boost::none,
                              ShardId{"0"}),
                          "StaleConfig error");
            }),
        DBException,
        ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, MultiCollectionRouterDoesNotRetryForSubRouter) {
    actAsSubRouter();

    // The MultiCollectionRouter should not retry if the shard acted as a sub-router.
    int tries = 0;
    sharding::router::MultiCollectionRouter router(getServiceContext(), {_nss});
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx,
                         stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap) {
                         tries++;
                         OID epoch{OID::gen()};
                         Timestamp timestamp{1, 0};
                         uasserted(StaleConfigInfo(_nss,
                                                   ShardVersionFactory::make(
                                                       ChunkVersion({epoch, timestamp}, {2, 0})),
                                                   boost::none,
                                                   ShardId{"0"}),
                                   "StaleConfig error");
                     }),
        DBException,
        ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, MultiCollectionRouterDoesNotRetryOnStaleConfigForNonSubRouter) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The MultiCollectionRouter should not retry on StaleConfig error if the shard did not act
    // as a sub-router.
    int tries = 0;
    sharding::router::MultiCollectionRouter router(getServiceContext(), {_nss});
    ASSERT_THROWS_CODE(
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx,
                         stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap) {
                         tries++;
                         uasserted(StaleConfigInfo(_nss,
                                                   ShardVersionFactory::make(
                                                       ChunkVersion({epoch, timestamp}, {2, 0})),
                                                   boost::none,
                                                   ShardId{"0"}),
                                   "StaleConfig error");
                     }),
        DBException,
        ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

// TODO SERVER-102931: Integrate RouterAcquisitionSnapshot into the tests below.
TEST_F(RouterRoleTest, RoutingContextCreationAndDestruction) {
    auto opCtx = operationContext();

    const auto expectedCri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, _nss));

    routing_context_utils::withValidatedRoutingContext(
        opCtx, std::vector{_nss}, [&](RoutingContext& routingCtx) {
            const auto& actualCri = routingCtx.getCollectionRoutingInfo(_nss);

            ASSERT_TRUE(actualCri.hasRoutingTable());
            ASSERT_TRUE(actualCri.isSharded());
            ASSERT_EQ(actualCri.getCollectionVersion(), expectedCri.getCollectionVersion());

            routingCtx.onRequestSentForNss(_nss);
        });
}

DEATH_TEST_F(RouterRoleTest,
             InvalidRoutingContextDestruction,
             "RoutingContext ended without validating routing tables for nss") {
    const auto opCtx = operationContext();

    ASSERT_THROWS_CODE(routing_context_utils::withValidatedRoutingContext(
                           opCtx, std::vector{_nss}, [&](RoutingContext& routingCtx) {}),
                       AssertionException,
                       10446900);
}

DEATH_TEST_F(RouterRoleTest,
             CannotDeclareDuplicateNssOnRoutingContext,
             "declared multiple times in RoutingContext") {
    const auto opCtx = operationContext();

    ASSERT_THROWS_CODE(routing_context_utils::withValidatedRoutingContext(
                           opCtx, std::vector{_nss, _nss}, [&](RoutingContext& routingCtx) {}),
                       AssertionException,
                       10292300);
}

DEATH_TEST_F(RouterRoleTest,
             CannotAcquireCRIForCollectionlessAggregate,
             "Incorrectly attempted to acquire a CollectionRoutingInfo for a collectionless "
             "aggregate, which is not a real namespace.") {
    const auto opCtx = operationContext();
    const auto nss = NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");

    ASSERT_THROWS_CODE(routing_context_utils::withValidatedRoutingContext(
                           opCtx, std::vector{nss}, [&](RoutingContext& routingCtx) {}),
                       AssertionException,
                       10292500);
}

TEST_F(RouterRoleTest, CannotAccessUndeclaredNssOnRoutingContext) {
    const auto opCtx = operationContext();

    ASSERT_THROWS_CODE(
        routing_context_utils::withValidatedRoutingContext(
            opCtx,
            std::vector{_nss},
            [&](RoutingContext& routingCtx) {
                const auto cri = routingCtx.getCollectionRoutingInfo(
                    NamespaceString::createNamespaceString_forTest("test.nonexistent_nss"));
            }),
        DBException,
        10292301);
}

TEST_F(RouterRoleTest, RoutingContextPropagatesCatalogCacheErrors) {
    auto opCtx = operationContext();

    auto future = launchAsync([this, &opCtx] {
        // Mark the routing info as stale to force a refresh.
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        catalogCache->onStaleCollectionVersion(_nss, boost::none);

        routing_context_utils::withValidatedRoutingContext(
            opCtx, std::vector{_nss}, [&](RoutingContext& routingCtx) {});
    });

    onCommand([](const executor::RemoteCommandRequest& request) {
        return Status(ErrorCodes::InternalError, "Simulated failure");
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::InternalError);
}

TEST_F(RouterRoleTest, RoutingContextRoutingTablesAreImmutable) {
    const auto opCtx = operationContext();

    routing_context_utils::withValidatedRoutingContext(
        opCtx, std::vector{_nss}, [&](RoutingContext& routingCtx) {
            const auto criA = routingCtx.getCollectionRoutingInfo(_nss);
            const auto versionA = criA.getCollectionVersion().placementVersion();

            // Schedule a refresh with a new placement version.
            auto future = scheduleRoutingInfoForcedRefresh(_nss);

            mockConfigServerQueries(_nss, versionA.epoch(), versionA.getTimestamp());

            const auto criB = *future.default_timed_get();
            const auto versionB = criB.getCollectionVersion().placementVersion();

            // Routing tables have changed in the CatalogCache, but not in the RoutingContext.
            ASSERT_NE(versionA, versionB);
            ASSERT_EQ(
                routingCtx.getCollectionRoutingInfo(_nss).getCollectionVersion().placementVersion(),
                versionA);

            routingCtx.onRequestSentForNss(_nss);
        });
}

TEST_F(RouterRoleTest, RoutingContextCreationWithCRI) {
    auto opCtx = operationContext();
    auto cri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, _nss));

    RoutingContext routingCtx(opCtx, {{_nss, cri}});
    const auto& actualCri = routingCtx.getCollectionRoutingInfo(_nss);

    ASSERT_TRUE(actualCri.hasRoutingTable());
    ASSERT_TRUE(actualCri.isSharded());
    ASSERT_EQ(actualCri.getCollectionVersion(), cri.getCollectionVersion());

    routingCtx.onRequestSentForNss(_nss);
}
}  // namespace
}  // namespace mongo
