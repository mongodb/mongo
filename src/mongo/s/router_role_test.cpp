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

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/grid.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
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
            BSON("insert"
                 << "test" << repl::ReadConcernArgs::kReadConcernFieldName
                 << BSON(repl::ReadConcernArgs::kAfterClusterTimeFieldName
                         << afterClusterTime.asTimestamp() << repl::ReadConcernArgs::kLevelFieldName
                         << "majority"))));
        repl::ReadConcernArgs::get(operationContext()) = readConcernArgs;
    }

private:
    boost::optional<RouterOperationContextSession> _routerOpCtxSession;
};

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

}  // namespace
}  // namespace mongo
