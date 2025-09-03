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

#include "mongo/db/global_catalog/router_role_api/router_role.h"

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

/**
 * This sub-class does not initialize a session, so the OperationContext will not contain
 * a TransactionRouter instance. This allows testing cases when not operating within a transaction.
 */
class RouterRoleTest : public RouterCatalogCacheTestFixture {
public:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();

        _nss = NamespaceString::createNamespaceString_forTest("test.foo");
        setupNShards(2);
        loadRoutingTableWithTwoChunksAndTwoShards(_nss);
    }

    void tearDown() override {
        RouterCatalogCacheTestFixture::tearDown();
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

    void setupCatalogCacheWithHistory() {
        auto uuid = UUID::gen();
        ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

        auto future = scheduleRoutingInfoForcedRefresh(_nss);

        OID latestEpoch = OID::gen();
        Timestamp latestTimestamp{300, 1};

        CollectionType collType(
            _nss, latestEpoch, latestTimestamp, Date_t::now(), uuid, shardKeyPattern.toBSON());

        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            std::vector<BSONObj> results;
            results.push_back(collType.toBSON());
            ChunkVersion latestVersion({latestEpoch, latestTimestamp}, {3, 0});

            ChunkType chunk1(uuid,
                             {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                             latestVersion,
                             {"0"});
            chunk1.setOnCurrentShardSince(Timestamp(300, 0));
            chunk1.setHistory({ChunkHistory(*chunk1.getOnCurrentShardSince(), ShardId("0")),
                               ChunkHistory(Timestamp(200, 300), ShardId("1")),
                               ChunkHistory(Timestamp(100, 200), ShardId("0"))});
            chunk1.setName(OID::gen());

            latestVersion.incMinor();
            ChunkType chunk2(uuid,
                             {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                             latestVersion,
                             {"1"});
            chunk2.setOnCurrentShardSince(Timestamp(300, 0));
            chunk2.setHistory({ChunkHistory(*chunk2.getOnCurrentShardSince(), ShardId("1")),
                               ChunkHistory(Timestamp(200, 300), ShardId("0")),
                               ChunkHistory(Timestamp(100, 200), ShardId("1"))});
            chunk2.setName(OID::gen());

            results.push_back(BSON("chunks" << chunk1.toConfigBSON()));
            results.push_back(BSON("chunks" << chunk2.toConfigBSON()));

            return results;
        }());

        future.default_timed_get();
    }

protected:
    NamespaceString _nss;
};

/**
 * This sub-class initializes a session within the OperationContext.
 */
class RouterRoleTestTxn : public RouterRoleTest {
public:
    void setUp() override {
        RouterRoleTest::setUp();

        const auto opCtx = operationContext();
        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        _routerOpCtxSession.emplace(opCtx);

        // The service needs to be set to the RouterServer service so that the "insert" command can
        // be found when attaching txn request fields for the tests in this file.
        auto targetService =
            operationContext()->getServiceContext()->getService(ClusterRole::RouterServer);
        operationContext()->getClient()->setService(targetService);
    }

    void tearDown() override {
        _routerOpCtxSession.reset();
        RouterRoleTest::tearDown();
    }

    void actAsSubRouter(LogicalTime afterClusterTime = LogicalTime(Timestamp(3, 1))) {
        // Attach txn request fields for a request to shard0 so that the shard acts as a sub-router.
        TxnNumber txnNum{3};
        operationContext()->setTxnNumber(txnNum);
        operationContext()->setActiveTransactionParticipant();
        operationContext()->setInMultiDocumentTransaction();
        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(
            BSON("insert" << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                          << BSON(repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                  << afterClusterTime.asTimestamp()
                                  << repl::ReadConcernArgs::kLevelFieldName << "majority"))));
        repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    }

    void setupTransactionWithAtClusterTime(Timestamp clusterTime, int txnId) {
        TxnNumber txnNum{txnId};
        operationContext()->setTxnNumber(txnNum);

        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(
            BSON("find" << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                        << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << clusterTime << repl::ReadConcernArgs::kLevelFieldName
                                << "snapshot"))));
        repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    }

    void setupReadConcernAtClusterTime(Timestamp clusterTime) {
        repl::ReadConcernArgs readConcernArgs;
        ASSERT_OK(readConcernArgs.initialize(
            BSON("find" << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                        << BSON(repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << clusterTime << repl::ReadConcernArgs::kLevelFieldName
                                << "snapshot"))));
        repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    }

private:
    boost::optional<RouterOperationContextSession> _routerOpCtxSession;
};

TEST_F(RouterRoleTestTxn, DBPrimaryRouterDoesNotRetryForSubRouter) {
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

TEST_F(RouterRoleTestTxn, DBPrimaryRouterDoesNotRetryOnStaleDbVersionForNonSubRouter) {
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

TEST_F(RouterRoleTest, DBPrimaryRouterRetryOnStaleDbVersionForNonSubRouterWithoutTxn) {
    // The DBPrimaryRouter should retry on StaleDbVersion error when not operating within a
    // transaction.
    int tries = 0;
    sharding::router::DBPrimaryRouter router(getServiceContext(), _nss.dbName());
    auto future = launchAsync([&] {
        router.route(
            operationContext(),
            "test",
            [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                tries++;
                if (tries == 1) {
                    uasserted(StaleDbRoutingVersion(_nss.dbName(),
                                                    DatabaseVersion(UUID::gen(), Timestamp(0, 0)),
                                                    DatabaseVersion(UUID::gen(), Timestamp(0, 0))),
                              "staleDbVersion");
                }
            });
    });
    future.default_timed_get();

    ASSERT_EQ(tries, 2);
}

TEST_F(RouterRoleTestTxn, DBPrimaryRouterSetsPlacementConflictTimeIfSubRouter) {
    auto clusterTime = Timestamp(1, 1);
    actAsSubRouter(LogicalTime(clusterTime));

    sharding::router::DBPrimaryRouter router(getServiceContext(), _nss.dbName());
    router.route(
        operationContext(), "test", [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
            auto txnRouter = TransactionRouter::get(opCtx);
            ASSERT(txnRouter);

            auto placementConflictTime = txnRouter.getPlacementConflictTime();
            ASSERT(placementConflictTime);
            ASSERT_EQ(placementConflictTime->asTimestamp(), clusterTime);
        });
}

TEST_F(RouterRoleTest, DBPrimaryRouterExceedsMaxRetryAttempts) {
    int maxTestRetries = 10;

    // Sets the number of retries, but values less than 10 are rejected due to parameter validation.
    RAIIServerParameterControllerForTest controller("maxNumStaleVersionRetries", maxTestRetries);

    // The DBPrimaryRouter should retry until it reaches the maximum available retries.
    int tries = 0;
    sharding::router::DBPrimaryRouter router(getServiceContext(), _nss.dbName());
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
                         tries++;
                         uasserted(
                             StaleDbRoutingVersion(_nss.dbName(),
                                                   DatabaseVersion(UUID::gen(), Timestamp(0, 0)),
                                                   DatabaseVersion(UUID::gen(), Timestamp(0, 0))),
                             "staleDbVersion");
                     });
    });

    ASSERT_THROWS_CODE_AND_WHAT(
        future.default_timed_get(),
        DBException,
        ErrorCodes::StaleDbVersion,
        "Exceeded maximum number of 10 retries attempting 'test' :: caused by :: staleDbVersion");
    ASSERT_EQ(tries, maxTestRetries + 1);
}

TEST_F(RouterRoleTestTxn, CollectionRouterDoesNotRetryForSubRouter) {
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

TEST_F(RouterRoleTestTxn, CollectionRouterDoesNotRetryOnStaleConfigForNonSubRouter) {
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

TEST_F(RouterRoleTestTxn, CollectionRouterSetsPlacementConflictTimeIfSubRouter) {
    auto clusterTime = Timestamp(1, 1);
    actAsSubRouter(LogicalTime(clusterTime));

    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    router.route(
        operationContext(), "test", [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
            auto txnRouter = TransactionRouter::get(opCtx);
            ASSERT(txnRouter);

            auto placementConflictTime = txnRouter.getPlacementConflictTime();
            ASSERT(placementConflictTime);
            ASSERT_EQ(placementConflictTime->asTimestamp(), clusterTime);
        });
}

TEST_F(RouterRoleTest, CollectionRouterRetryOnStaleConfigWithoutTxn) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should retry on StaleConfig errors when not operating within a
    // transaction.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         ASSERT_TRUE(cri.hasRoutingTable());
                         tries++;
                         if (tries == 1) {
                             uasserted(StaleConfigInfo(_nss,
                                                       ShardVersionFactory::make(ChunkVersion(
                                                           {epoch, timestamp}, {2, 0})),
                                                       boost::none,
                                                       ShardId{"0"}),
                                       "StaleConfig error");
                         }
                     });
    });
    mockConfigServerQueries(_nss, epoch, timestamp);
    future.default_timed_get();

    ASSERT_EQ(tries, 2);

    // The CollectionRouter should not retry on StaleConfig error if a shard is stale in
    // StaleConfigInfo.
    tries = 0;
    sharding::router::CollectionRouter routerNotRetry(getServiceContext(), _nss, false);
    ASSERT_THROWS_CODE(
        routerNotRetry.route(operationContext(),
                             "test",
                             [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                 ASSERT_TRUE(cri.hasRoutingTable());
                                 tries++;
                                 uasserted(StaleConfigInfo(_nss,
                                                           ShardVersionFactory::make(ChunkVersion(
                                                               {epoch, timestamp}, {2, 0})),
                                                           boost::none,
                                                           ShardId{"0"}),
                                           "StaleConfig error");
                             }),
        DBException,
        ErrorCodes::StaleConfig);
    ASSERT_EQ(1, tries);

    tries = 0;
    sharding::router::CollectionRouter routerNotRetry2(getServiceContext(), _nss, false);
    ASSERT_THROWS_CODE(routerNotRetry2.route(
                           operationContext(),
                           "test",
                           [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                               ASSERT_TRUE(cri.hasRoutingTable());
                               tries++;
                               // Create versions where router has newer version than shard (shard
                               // stale condition)
                               const OID staleEpoch{OID::gen()};
                               const Timestamp staleTimestamp{1, 0};
                               auto shardVersion =
                                   ChunkVersion({staleEpoch, staleTimestamp}, {1, 0});
                               auto routerVersion =
                                   ChunkVersion({staleEpoch, staleTimestamp}, {2, 0});
                               uasserted(StaleConfigInfo(_nss,
                                                         ShardVersionFactory::make(routerVersion),
                                                         ShardVersionFactory::make(shardVersion),
                                                         ShardId{"0"}),
                                         "StaleConfig error");
                           }),
                       DBException,
                       ErrorCodes::StaleConfig);
    ASSERT_EQ(1, tries);
}

TEST_F(RouterRoleTest, CollectionRouterRetryOnStaleDbVersionWithoutTxn) {
    // The CollectionRouter should retry on StaleDbVersion error when not operating within a
    // transaction.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    auto future = launchAsync([&] {
        router.route(
            operationContext(),
            "test",
            [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                ASSERT_TRUE(cri.hasRoutingTable());
                tries++;
                if (tries == 1) {
                    uasserted(StaleDbRoutingVersion(_nss.dbName(),
                                                    DatabaseVersion(UUID::gen(), Timestamp(0, 0)),
                                                    DatabaseVersion(UUID::gen(), Timestamp(0, 0))),
                              "staleDbVersion");
                }
            });
    });
    future.default_timed_get();

    ASSERT_EQ(tries, 2);

    // The CollectionRouter should not retry on StaleDbVersion error if the shard is stale in
    // StaleDbRoutingVersion.
    tries = 0;
    sharding::router::CollectionRouter routerNotRetry(getServiceContext(), _nss, false);
    ASSERT_THROWS_CODE(
        routerNotRetry.route(operationContext(),
                             "test",
                             [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                 ASSERT_TRUE(cri.hasRoutingTable());
                                 tries++;
                                 uasserted(StaleDbRoutingVersion(
                                               _nss.dbName(),
                                               DatabaseVersion(UUID::gen(), Timestamp(0, 0)),
                                               boost::none),
                                           "staleDbVersion");
                             }),
        DBException,
        ErrorCodes::StaleDbVersion);

    ASSERT_EQ(1, tries);

    tries = 0;
    sharding::router::CollectionRouter routerNotRetry2(getServiceContext(), _nss, false);
    ASSERT_THROWS_CODE(
        routerNotRetry2.route(operationContext(),
                              "test",
                              [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                  ASSERT_TRUE(cri.hasRoutingTable());
                                  tries++;
                                  // Create versions where router has newer version than shard
                                  // (shard stale condition)
                                  const UUID staleEpoch{UUID::gen()};
                                  uasserted(StaleDbRoutingVersion(
                                                _nss.dbName(),
                                                DatabaseVersion(staleEpoch, Timestamp(2, 0)),
                                                DatabaseVersion(staleEpoch, Timestamp(1, 0))),
                                            "staleDbVersion");
                              }),
        DBException,
        ErrorCodes::StaleDbVersion);

    ASSERT_EQ(1, tries);
}

TEST_F(RouterRoleTest, CollectionRouterRetryOnStaleEpochWithoutTxn) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should retry on StaleEpoch error when not operating within a
    // transaction.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    auto future = launchAsync([&] {
        router.route(
            operationContext(),
            "test",
            [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                ASSERT_TRUE(cri.hasRoutingTable());
                tries++;
                if (tries == 1) {
                    uasserted(
                        StaleEpochInfo(
                            _nss,
                            ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0})),
                            ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0}))),
                        "staleEpoch");
                }
            });
    });
    mockConfigServerQueries(_nss, epoch, timestamp);
    future.default_timed_get();
    ASSERT_EQ(tries, 2);

    // The CollectionRouter should retry on StaleEpoch(without ExtraInfo) error when not operating
    // within a transaction.
    tries = 0;
    sharding::router::CollectionRouter router2(getServiceContext(), _nss);
    auto future2 = launchAsync([&] {
        router2.route(operationContext(),
                      "test",
                      [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                          ASSERT_TRUE(cri.hasRoutingTable());
                          tries++;
                          if (tries == 1) {
                              uasserted(ErrorCodes::StaleEpoch, "staleEpoch");
                          }
                      });
    });
    mockConfigServerQueries(_nss, OID::gen(), Timestamp{2, 0});
    future2.default_timed_get();
    ASSERT_EQ(tries, 2);

    // The CollectionRouter should not retry on StaleEpoch error if nss is not involved in routing.
    tries = 0;
    sharding::router::CollectionRouter routerNotRetry(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(
        routerNotRetry.route(
            operationContext(),
            "test",
            [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                ASSERT_TRUE(cri.hasRoutingTable());
                tries++;
                uasserted(StaleEpochInfo(
                              NamespaceString::createNamespaceString_forTest("test.foo_not_exist"),
                              ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0})),
                              ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0}))),
                          "staleEpoch");
            }),
        DBException,
        ErrorCodes::StaleEpoch);
    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTestTxn, CollectionRouterRetryOnTransactionParticipantFailedUnyieldWithoutTxn) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should not retry on TransactionParticipantFailedUnyield error.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(router.route(operationContext(),
                                    "test",
                                    [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                        tries++;
                                        const auto status = Status(
                                            StaleConfigInfo(_nss,
                                                            ShardVersionFactory::make(ChunkVersion(
                                                                {epoch, timestamp}, {2, 0})),
                                                            boost::none,
                                                            ShardId{"0"}),
                                            "StaleConfig error");
                                        uasserted(TransactionParticipantFailedUnyieldInfo(status),
                                                  "StaleConfig error");
                                    }),
                       DBException,
                       ErrorCodes::TransactionParticipantFailedUnyield);
    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTestTxn, CollectionRouterWithRoutingContextDoesNotRetryForSubRouter) {
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

TEST_F(RouterRoleTestTxn,
       CollectionRouterWithRoutingContextDoesNotRetryOnStaleConfigForNonSubRouter) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter should not retry on StaleConfig error if the shard did not act as
    // a sub-router.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(router.routeWithRoutingContext(
                           operationContext(),
                           "test",
                           [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                               tries++;
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

TEST_F(RouterRoleTest, CollectionRouterWithRoutingContextRetryOnStaleConfigWithoutTxn) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    // The CollectionRouter::routeWithRoutingContext should retry on StaleConfig error when not
    // operating within a transaction.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    auto future = launchAsync([&] {
        router.routeWithRoutingContext(
            operationContext(), "test", [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                tries++;
                const auto& cri = routingCtx.getCollectionRoutingInfo(_nss);
                ASSERT_TRUE(cri.hasRoutingTable());
                if (tries == 1) {
                    uasserted(StaleConfigInfo(_nss,
                                              ShardVersionFactory::make(
                                                  ChunkVersion({epoch, timestamp}, {2, 0})),
                                              boost::none,
                                              ShardId{"0"}),
                              "StaleConfig error");
                } else {
                    routingCtx.onRequestSentForNss(_nss);
                    return BSONObj();
                }
            });
    });
    mockConfigServerQueries(_nss, epoch, timestamp);
    future.default_timed_get();

    ASSERT_EQ(tries, 2);
}

DEATH_TEST_F(RouterRoleTest,
             CollectionRouterWithRoutingContextRetryOnStaleConfigWithoutValidation,
             "RoutingContext ended without validating routing tables for nss test.foo") {
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT_THROWS_CODE(router.routeWithRoutingContext(
                           operationContext(),
                           "test",
                           [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                               const auto& cri = routingCtx.getCollectionRoutingInfo(_nss);
                               ASSERT_TRUE(cri.hasRoutingTable());
                           }),
                       DBException,
                       10446900);
}

TEST_F(RouterRoleTestTxn, CollectionRouterWithRoutingContextAtTransactionClusterTime) {
    setupCatalogCacheWithHistory();

    int txnId = 4;

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a transaction.
    // Each timestamp should route to the expected shard based on the historical chunk distribution.
    auto testAtTimestamp =
        [&](const Timestamp& clusterTime, int testId, std::string expectedShardId) {
            setupTransactionWithAtClusterTime(clusterTime, txnId);
            sharding::router::CollectionRouter router(getServiceContext(), _nss);

            auto future = launchAsync([&] {
                router.routeWithRoutingContext(
                    operationContext(),
                    "test",
                    [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                        const auto& cri = routingCtx.getCollectionRoutingInfo(_nss);
                        ASSERT_TRUE(cri.hasRoutingTable());
                        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
                        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(
                            BSON("_id" << testId), ShardId(expectedShardId)));
                        routingCtx.onRequestSentForNss(_nss);
                        return BSONObj();
                    });
            });
            future.default_timed_get();
            txnId++;
        };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTestTxn, CollectionRouterWithRoutingContextAtReadConcernClusterTime) {
    setupCatalogCacheWithHistory();

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a read concern.
    // Each timestamp should route to the expected shard based on the historical chunk distribution.
    auto testAtTimestamp =
        [&](const Timestamp& clusterTime, int testId, std::string expectedShardId) {
            setupReadConcernAtClusterTime(clusterTime);
            sharding::router::CollectionRouter router(getServiceContext(), _nss);

            auto future = launchAsync([&] {
                router.routeWithRoutingContext(
                    operationContext(),
                    "test",
                    [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                        const auto& cri = routingCtx.getCollectionRoutingInfo(_nss);
                        ASSERT_TRUE(cri.hasRoutingTable());
                        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
                        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(
                            BSON("_id" << testId), ShardId(expectedShardId)));
                        routingCtx.onRequestSentForNss(_nss);
                        return BSONObj();
                    });
            });
            future.default_timed_get();
        };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTestTxn, MultiCollectionRouterDoesNotRetryForSubRouter) {
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

TEST_F(RouterRoleTestTxn, MultiCollectionRouterDoesNotRetryOnStaleConfigForNonSubRouter) {
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

TEST_F(RouterRoleTest, MultiCollectionRouterRetryOnStaleConfig) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};
    auto nss2 = NamespaceString::createNamespaceString_forTest("test.foo2");

    // The MultiCollectionRouter should retry on StaleConfig error when not operating within a
    // transaction.
    int tries = 0;
    sharding::router::MultiCollectionRouter router(getServiceContext(), {_nss, nss2});
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test",
                     [&](OperationContext* opCtx,
                         stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap) {
                         ASSERT_EQ(2, criMap.size());
                         for (const auto& [nss, cri] : criMap) {
                             ASSERT_TRUE(cri.hasRoutingTable());
                         }
                         tries++;
                         if (tries == 1) {
                             uasserted(StaleConfigInfo(_nss,
                                                       ShardVersionFactory::make(ChunkVersion(
                                                           {epoch, timestamp}, {2, 0})),
                                                       boost::none,
                                                       ShardId{"0"}),
                                       "StaleConfig error");
                         }
                     });
    });
    mockConfigServerQueries(nss2, epoch, timestamp);
    mockConfigServerQueries(_nss, epoch, timestamp);
    future.default_timed_get();

    ASSERT_EQ(tries, 2);
}

TEST_F(RouterRoleTest,
       MultiCollectionRouterDoesNotRetryOnStaleConfigNonTargetedNamespaceWhenShardIsNotStale) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};
    auto nss2 = NamespaceString::createNamespaceString_forTest("test.foo2");

    // The MultiCollectionRouter should not retry on StaleConfig error with non-targeted
    // namespace when the router is stale.
    int tries = 0;
    sharding::router::MultiCollectionRouter router(getServiceContext(), {_nss, nss2});
    auto future = launchAsync([&] {
        router.route(
            operationContext(),
            "test",
            [&](OperationContext* opCtx,
                stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap) {
                ASSERT_EQ(2, criMap.size());
                for (const auto& [nss, cri] : criMap) {
                    ASSERT_TRUE(cri.hasRoutingTable());
                }
                tries++;
                uasserted(StaleConfigInfo(
                              NamespaceString::createNamespaceString_forTest("test.foo_not_exist"),
                              ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 0})),
                              ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {2, 1})),
                              ShardId{"0"}),
                          "StaleConfig error");
            });
    });
    mockConfigServerQueries(nss2, epoch, timestamp);
    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::StaleConfig);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTestTxn, MultiCollectionRouterSetsPlacementConflictTimeIfSubRouter) {
    auto clusterTime = Timestamp(1, 1);
    actAsSubRouter(LogicalTime(clusterTime));

    sharding::router::MultiCollectionRouter router(getServiceContext(), {_nss});
    router.route(operationContext(),
                 "test",
                 [&](OperationContext* opCtx,
                     stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap) {
                     auto txnRouter = TransactionRouter::get(opCtx);
                     ASSERT(txnRouter);

                     auto placementConflictTime = txnRouter.getPlacementConflictTime();
                     ASSERT(placementConflictTime);
                     ASSERT_EQ(placementConflictTime->asTimestamp(), clusterTime);
                 });
}

// TODO SERVER-102931: Integrate RouterAcquisitionSnapshot into the tests below.
TEST_F(RouterRoleTestTxn, RoutingContextCreationAndDestruction) {
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

DEATH_TEST_F(RouterRoleTestTxn,
             InvalidRoutingContextDestruction,
             "RoutingContext ended without validating routing tables for nss") {
    const auto opCtx = operationContext();

    ASSERT_THROWS_CODE(routing_context_utils::withValidatedRoutingContext(
                           opCtx, std::vector{_nss}, [&](RoutingContext& routingCtx) {}),
                       AssertionException,
                       10446900);
}

DEATH_TEST_F(RouterRoleTestTxn,
             CannotDeclareDuplicateNssOnRoutingContext,
             "declared multiple times in RoutingContext") {
    const auto opCtx = operationContext();

    ASSERT_THROWS_CODE(routing_context_utils::withValidatedRoutingContext(
                           opCtx, std::vector{_nss, _nss}, [&](RoutingContext& routingCtx) {}),
                       AssertionException,
                       10292300);
}

TEST_F(RouterRoleTestTxn, CannotAccessUndeclaredNssOnRoutingContext) {
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

TEST_F(RouterRoleTestTxn, RoutingContextPropagatesCatalogCacheErrors) {
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

TEST_F(RouterRoleTestTxn, RoutingContextRoutingTablesAreImmutable) {
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

TEST_F(RouterRoleTestTxn, RoutingContextCreationWithCRI) {
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

TEST_F(RouterRoleTest, CollectionRouterExceedsMaxRetryAttempts) {
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT(!TransactionRouter::get(operationContext()));
    int maxTestRetries = 10;

    // Sets the number of retries, but values less than 10 are rejected due to parameter
    // validation.
    RAIIServerParameterControllerForTest controller("maxNumStaleVersionRetries", maxTestRetries);

    // The CollectionRouter should retry until it reaches the maximum available retries.
    int tries = 0;
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test shard not found",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         tries++;
                         uasserted(ErrorCodes::ShardNotFound, "Shard has been removed");
                     });
    });

    unsigned int startTime = 1;

    // Simulate queries to update the cache when stale errors occur.
    for (int i = 0; i < maxTestRetries; i++) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(_nss.dbName(), {"0"}, DatabaseVersion(UUID::gen(), Timestamp(1, 0)));
            return std::vector<BSONObj>{db.toBSON()};
        }());

        OID epoch{OID::gen()};
        Timestamp timestamp{startTime, 0};
        startTime++;

        mockConfigServerQueries(_nss, epoch, timestamp);
    }

    ASSERT_THROWS_CODE_AND_WHAT(future.default_timed_get(),
                                DBException,
                                ErrorCodes::ShardNotFound,
                                "Exceeded maximum number of 10 retries attempting 'test shard not "
                                "found' :: caused by :: Shard has been removed");
    ASSERT_EQ(tries, maxTestRetries + 1);
}

TEST_F(RouterRoleTestTxn, CollectionRouterDoesNotRetryOnShardNotFound) {
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT(TransactionRouter::get(operationContext()));

    // The CollectionRouter should not retry on ShardNotFound error.
    int tries = 0;
    ASSERT_THROWS_CODE(router.route(operationContext(),
                                    "test shard not found",
                                    [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                                        ASSERT_TRUE(cri.hasRoutingTable());
                                        tries++;
                                        uasserted(ErrorCodes::ShardNotFound,
                                                  "Shard has been removed");
                                        FAIL("should not reach here");
                                    }),
                       DBException,
                       ErrorCodes::ShardNotFound);

    ASSERT_EQ(tries, 1);
}

TEST_F(RouterRoleTest, CollectionRouterRetryOnShardNotFound) {
    OID epoch{OID::gen()};
    Timestamp timestamp{1, 0};

    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    ASSERT(!TransactionRouter::get(operationContext()));

    // The CollectionRouter should retry on ShardNotFound error when not operating within a
    // transaction.
    int tries = 0;
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test shard not found",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         ASSERT_TRUE(cri.hasRoutingTable());
                         tries++;
                         if (tries == 1) {
                             uasserted(ErrorCodes::ShardNotFound, "Shard has been removed");
                         }
                     });
    });
    expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
        DatabaseType db(_nss.dbName(), {"0"}, DatabaseVersion(UUID::gen(), Timestamp(1, 0)));
        return std::vector<BSONObj>{db.toBSON()};
    }());
    mockConfigServerQueries(_nss, epoch, timestamp);
    future.default_timed_get();

    ASSERT_EQ(tries, 2);
}

TEST_F(RouterRoleTestTxn, CatalogCacheGetRoutingInfoAtTransactionClusterTime) {
    setupCatalogCacheWithHistory();

    int txnId = 4;

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a transaction.
    // Each timestamp should route to the expected shard based on the historical chunk
    // distribution.
    auto testAtTimestamp = [&](const Timestamp& clusterTime,
                               int testId,
                               std::string expectedShardId) {
        setupTransactionWithAtClusterTime(clusterTime, txnId);

        auto catalogCache = Grid::get(getServiceContext())->catalogCache();

        auto cri = catalogCache->getCollectionRoutingInfoAt(operationContext(), _nss, clusterTime)
                       .getValue();
        ASSERT_TRUE(cri.hasRoutingTable());
        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(BSON("_id" << testId),
                                                            ShardId(expectedShardId)));
        txnId++;
    };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTestTxn, CatalogCacheGetRoutingInfoAtReadConcernClusterTime) {
    setupCatalogCacheWithHistory();

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a read concern.
    // Each timestamp should route to the expected shard based on the historical chunk
    // distribution.
    auto testAtTimestamp = [&](const Timestamp& clusterTime,
                               int testId,
                               std::string expectedShardId) {
        setupReadConcernAtClusterTime(clusterTime);

        auto catalogCache = Grid::get(getServiceContext())->catalogCache();

        auto cri = catalogCache->getCollectionRoutingInfoAt(operationContext(), _nss, clusterTime)
                       .getValue();
        ASSERT_TRUE(cri.hasRoutingTable());
        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(BSON("_id" << testId),
                                                            ShardId(expectedShardId)));
    };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTestTxn, CollectionRouterGetRoutingInfoAtTransactionClusterTime) {
    setupCatalogCacheWithHistory();

    int txnId = 4;

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a transaction.
    // Each timestamp should route to the expected shard based on the historical chunk
    // distribution.
    auto testAtTimestamp =
        [&](const Timestamp& clusterTime, int testId, std::string expectedShardId) {
            setupTransactionWithAtClusterTime(clusterTime, txnId);

            sharding::router::CollectionRouter router(getServiceContext(), _nss);

            auto future = launchAsync([&] {
                router.route(
                    operationContext(),
                    "test transaction cluster time",
                    [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                        ASSERT_TRUE(cri.hasRoutingTable());
                        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
                        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(
                            BSON("_id" << testId), ShardId(expectedShardId)));
                    });
            });
            future.default_timed_get();
            txnId++;
        };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTestTxn, CollectionRouterGetRoutingInfoAtReadConcernClusterTime) {
    setupCatalogCacheWithHistory();

    // Test routing behavior at different timestamps to verify that catalog cache history works
    // correctly when clusterTime is set in a read concern.
    // Each timestamp should route to the expected shard based on the historical chunk
    // distribution.
    auto testAtTimestamp =
        [&](const Timestamp& clusterTime, int testId, std::string expectedShardId) {
            setupReadConcernAtClusterTime(clusterTime);

            sharding::router::CollectionRouter router(getServiceContext(), _nss);

            auto future = launchAsync([&] {
                router.route(
                    operationContext(),
                    "test transaction cluster time",
                    [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                        ASSERT_TRUE(cri.hasRoutingTable());
                        ASSERT_EQ(3, cri.getCollectionVersion().placementVersion().majorVersion());
                        ASSERT_TRUE(cri.getChunkManager().keyBelongsToShard(
                            BSON("_id" << testId), ShardId(expectedShardId)));
                    });
            });
            future.default_timed_get();
        };

    // Test different timestamps to verify history works
    testAtTimestamp(Timestamp(150, 0), 2, "1");
    testAtTimestamp(Timestamp(250, 0), 2, "0");
    testAtTimestamp(Timestamp(350, 0), 2, "1");
    testAtTimestamp(Timestamp(150, 0), -2, "0");
    testAtTimestamp(Timestamp(250, 0), -2, "1");
    testAtTimestamp(Timestamp(350, 0), -2, "0");
    ASSERT_THROWS_CODE(
        testAtTimestamp(Timestamp(1, 0), 2, "0"), DBException, ErrorCodes::StaleChunkHistory);
}

TEST_F(RouterRoleTest, CollectionRouterRetryOnStaleConfigTimeseriesBucket) {
    const OID epoch{OID::gen()};
    const Timestamp timestamp{1, 0};

    auto bucketNss = _nss.makeTimeseriesBucketsNamespace();

    // The CollectionRouter should retry because bucket collection maps to targeted timeseries view.
    int tries = 0;
    sharding::router::CollectionRouter router(getServiceContext(), _nss);
    auto future = launchAsync([&] {
        router.route(operationContext(),
                     "test timeseries routing",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         ASSERT_TRUE(cri.hasRoutingTable());
                         tries++;
                         if (tries == 1) {
                             uasserted(StaleConfigInfo(bucketNss,
                                                       ShardVersionFactory::make(ChunkVersion(
                                                           {epoch, timestamp}, {2, 0})),
                                                       boost::none,
                                                       ShardId{"0"}),
                                       "StaleConfig error");
                         }
                     });
    });
    mockConfigServerQueries(bucketNss, epoch, timestamp);
    future.default_timed_get();

    ASSERT_EQ(tries, 2);
}
}  // namespace
}  // namespace mongo
