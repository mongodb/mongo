/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/sharding_environment/config_server_op_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/vector_clock/topology_time_ticker.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace {

class ConfigServerOpObserverTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        setUpAndInitializeConfigDb();

        auto clusterIdLoader = ClusterIdentityLoader::get(operationContext());
        ASSERT_OK(clusterIdLoader->loadClusterId(
            operationContext(), catalogClient(), repl::ReadConcernLevel::kLocalReadConcern));
        _clusterId = clusterIdLoader->getClusterId();
    }

    OID _clusterId;
    ConfigServerOpObserver _opObserver;
};

TEST_F(ConfigServerOpObserverTest, NodeClearsCatalogManagerOnConfigVersionRollBack) {
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = true;

    _opObserver.onReplicationRollback(operationContext(), rbInfo);

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigServerOpObserverTest, NodeDoesNotClearCatalogManagerWhenConfigVersionNotRolledBack) {
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = false;

    _opObserver.onReplicationRollback(operationContext(), rbInfo);

    ASSERT_EQ(ErrorCodes::AlreadyInitialized,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

using ConfigServerOpObserverTestDeathTest = ConfigServerOpObserverTest;
DEATH_TEST_F(ConfigServerOpObserverTestDeathTest,
             NodeClearsClusterIDOnConfigVersionRollBack,
             "Invariant failure") {
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = true;

    _opObserver.onReplicationRollback(operationContext(), rbInfo);
    ClusterIdentityLoader::get(operationContext())->getClusterId();
}

TEST_F(ConfigServerOpObserverTest, NodeDoesNotClearClusterIDWhenConfigVersionNotRolledBack) {
    ConfigServerOpObserver opObserver;
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = false;

    opObserver.onReplicationRollback(operationContext(), rbInfo);

    ASSERT_EQ(ClusterIdentityLoader::get(operationContext())->getClusterId(), _clusterId);
}

TEST_F(ConfigServerOpObserverTest, ConfigOpTimeAdvancedWhenMajorityCommitPointAdvanced) {
    repl::OpTime a(Timestamp(1, 1), 1);
    repl::OpTime b(Timestamp(1, 2), 1);

    _opObserver.onMajorityCommitPointUpdate(getServiceContext(), a);
    const auto aTime = VectorClock::get(getServiceContext())->getTime();
    ASSERT_EQ(a.getTimestamp(), aTime.configTime().asTimestamp());

    _opObserver.onMajorityCommitPointUpdate(getServiceContext(), b);
    const auto bTime = VectorClock::get(getServiceContext())->getTime();
    ASSERT_EQ(b.getTimestamp(), bTime.configTime().asTimestamp());
}

void testTopologyTimeTickerNotifications(OperationContext* opCtx,
                                         repl::MemberState memberState,
                                         std::function<void()> writeFn,
                                         Timestamp commitTs,
                                         std::map<Timestamp, Timestamp> expectedTicks) {
    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx)->setFollowerMode(memberState));

    // Get the TopologyTimeTicker state before the operation
    auto& ticker = TopologyTimeTicker::get(opCtx);
    EXPECT_EQ(0, ticker.getTopologyTimeByLocalCommitTime_forTest().size());

    WriteUnitOfWork wuow(opCtx);

    // Call the provided write function (insert/update)
    writeFn();

    // TopologyTimeTicker not ticked yet. (should not tick before committing the write unit).
    ASSERT_EQ(0, ticker.getTopologyTimeByLocalCommitTime_forTest().size());

    ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(commitTs));
    wuow.commit();

    // Verify that TopologyTimeTicker was notified as expected.
    const auto tickPointsAfter = ticker.getTopologyTimeByLocalCommitTime_forTest();
    ASSERT_EQ(expectedTicks, tickPointsAfter);
}

void testTopologyTimeTickerNotificationsInsert(OperationContext* opCtx,
                                               ConfigServerOpObserver& opObserver,
                                               repl::MemberState memberState,
                                               std::vector<InsertStatement> inserts,
                                               Timestamp commitTs,
                                               std::map<Timestamp, Timestamp> expectedTicks) {
    const auto writeFn = [&]() {
        NamespaceString nss = NamespaceString::kConfigsvrShardsNamespace;
        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        opObserver.onInserts(
            opCtx, collection.getCollectionPtr(), inserts.begin(), inserts.end(), {}, {}, false);
    };

    testTopologyTimeTickerNotifications(opCtx, memberState, writeFn, commitTs, expectedTicks);
}

void testTopologyTimeTickerNotificationsUpdate(OperationContext* opCtx,
                                               ConfigServerOpObserver& opObserver,
                                               repl::MemberState memberState,
                                               BSONObj update,
                                               Timestamp commitTs,
                                               std::map<Timestamp, Timestamp> expectedTicks) {
    const auto writeFn = [&]() {
        NamespaceString nss = NamespaceString::kConfigsvrShardsNamespace;
        const auto collection = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        BSONObj preImageDoc = BSON("_id" << "shard0" << "host" << "localhost:27017");
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = BSON("_id" << "shard0");
        updateArgs.update = update;

        OplogUpdateEntryArgs entryArgs(&updateArgs, collection.getCollectionPtr());
        opObserver.onUpdate(opCtx, entryArgs, nullptr);
    };

    testTopologyTimeTickerNotifications(opCtx, memberState, writeFn, commitTs, expectedTicks);
}

void resetTopologyTimeTicker(OperationContext* opCtx) {
    auto& topologyTimeTicker = TopologyTimeTicker::get(opCtx);
    topologyTimeTicker.onReplicationRollback(repl::OpTime(Timestamp(0, 1), 0));
    ASSERT_EQ(0, topologyTimeTicker.getTopologyTimeByLocalCommitTime_forTest().size());
}

TEST_F(ConfigServerOpObserverTest, InsertToConfigShardsNotifiesTopologyTimeTicker) {
    const auto opCtx = operationContext();
    const Timestamp newTopologyTime = Timestamp(2, 200);

    const Timestamp commitTimestamp(3, 400);
    const std::map<Timestamp, Timestamp> expectedTicks = {{commitTimestamp, Timestamp(2, 200)}};
    const std::vector<InsertStatement> inserts{
        InsertStatement(BSON("_id" << "shard1"
                                   << "host"
                                   << "localhost:27018"
                                   << "topologyTime" << newTopologyTime))};

    // Nodes in primary and secondary modes expect the TopologyTimeTicker to be notified.
    testTopologyTimeTickerNotificationsInsert(
        opCtx, _opObserver, repl::MemberState::RS_PRIMARY, inserts, commitTimestamp, expectedTicks);
    resetTopologyTimeTicker(opCtx);

    testTopologyTimeTickerNotificationsInsert(opCtx,
                                              _opObserver,
                                              repl::MemberState::RS_SECONDARY,
                                              inserts,
                                              commitTimestamp,
                                              expectedTicks);
    resetTopologyTimeTicker(opCtx);

    // Nodes undergoing initial sync or rollback do not expect the TopologyTimeTicker to be
    // notified.
    testTopologyTimeTickerNotificationsInsert(
        opCtx, _opObserver, repl::MemberState::RS_ROLLBACK, inserts, commitTimestamp, {});
    resetTopologyTimeTicker(opCtx);

    testTopologyTimeTickerNotificationsInsert(
        opCtx, _opObserver, repl::MemberState::RS_STARTUP2, inserts, commitTimestamp, {});
    resetTopologyTimeTicker(opCtx);
}

TEST_F(ConfigServerOpObserverTest, UpdateToConfigShardsNotifiesTopologyTimeTicker) {
    const auto opCtx = operationContext();
    const Timestamp newTopologyTime = Timestamp(2, 200);
    const Timestamp commitTimestamp(3, 400);
    const std::map<Timestamp, Timestamp> expectedTicks = {{commitTimestamp, Timestamp(2, 200)}};
    const auto update =
        BSON("$v" << 2 << "diff" << BSON("i" << BSON("topologyTime" << newTopologyTime)));

    // Nodes in primary and secondary modes expect the TopologyTimeTicker to be notified.
    testTopologyTimeTickerNotificationsUpdate(
        opCtx, _opObserver, repl::MemberState::RS_PRIMARY, update, commitTimestamp, expectedTicks);
    resetTopologyTimeTicker(opCtx);

    testTopologyTimeTickerNotificationsUpdate(opCtx,
                                              _opObserver,
                                              repl::MemberState::RS_SECONDARY,
                                              update,
                                              commitTimestamp,
                                              expectedTicks);
    resetTopologyTimeTicker(opCtx);

    // Nodes undergoing initial sync or rollback do not expect the TopologyTimeTicker to be
    // notified.
    testTopologyTimeTickerNotificationsUpdate(
        opCtx, _opObserver, repl::MemberState::RS_ROLLBACK, update, commitTimestamp, {});
    resetTopologyTimeTicker(opCtx);

    testTopologyTimeTickerNotificationsUpdate(
        opCtx, _opObserver, repl::MemberState::RS_STARTUP2, update, commitTimestamp, {});
    resetTopologyTimeTicker(opCtx);
}

TEST_F(ConfigServerOpObserverTest,
       BatchInsertToConfigShardsNotifiesTopologyTimeTickerWithGreatestTopologyTime) {
    auto opCtx = operationContext();
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx)->setFollowerMode(repl::MemberState::RS_PRIMARY));
    const Timestamp commitTimestamp(3, 400);

    testTopologyTimeTickerNotificationsInsert(
        opCtx,
        _opObserver,
        repl::MemberState::RS_PRIMARY,
        {InsertStatement(BSON("_id" << "shard0"
                                    << "host"
                                    << "localhost:27017"
                                    << "topologyTime" << Timestamp(200, 0))),
         InsertStatement(BSON("_id" << "shard1"
                                    << "host"
                                    << "localhost:27018"
                                    << "topologyTime" << Timestamp(300, 0))),
         InsertStatement(BSON("_id" << "shard2"
                                    << "host"
                                    << "localhost:27019"
                                    << "topologyTime" << Timestamp(100, 0)))},

        commitTimestamp,
        {{commitTimestamp, Timestamp(300, 0)}});
}

TEST_F(ConfigServerOpObserverTest,
       WritesToConfigShardsWithoutTimestampDoNotNotifyTopologyTimeTicker) {
    auto opCtx = operationContext();
    ASSERT_OK(
        repl::ReplicationCoordinator::get(opCtx)->setFollowerMode(repl::MemberState::RS_PRIMARY));
    const Timestamp commitTimestamp(3, 400);

    // Insert with document without a topologyTime should not tick.
    testTopologyTimeTickerNotificationsInsert(opCtx,
                                              _opObserver,
                                              repl::MemberState::RS_PRIMARY,
                                              {InsertStatement(BSON("_id" << "shard1"
                                                                          << "host"
                                                                          << "localhost:27018"
                                                                          << "draining" << true))},
                                              commitTimestamp,
                                              {});

    // Insert with topologyTime=Timestamp(0, 0) should not tick.
    testTopologyTimeTickerNotificationsInsert(
        opCtx,
        _opObserver,
        repl::MemberState::RS_PRIMARY,
        {InsertStatement(BSON("_id" << "shard1"
                                    << "host"
                                    << "localhost:27018"
                                    << "topologyTime" << Timestamp(0, 0)))},
        commitTimestamp,
        {});

    // Update with no topology time change should not tick.
    testTopologyTimeTickerNotificationsUpdate(
        opCtx,
        _opObserver,
        repl::MemberState::RS_PRIMARY,
        BSON("$v" << 2 << "diff" << BSON("i" << BSON("draining" << true))),
        commitTimestamp,
        {});

    // Update with topologyTime=Timestamp(0, 0) should not tick.
    testTopologyTimeTickerNotificationsUpdate(
        opCtx,
        _opObserver,
        repl::MemberState::RS_PRIMARY,
        BSON("$v" << 2 << "diff" << BSON("i" << BSON("topologyTime" << Timestamp(0, 0)))),
        commitTimestamp,
        {});
}

}  // namespace
}  // namespace mongo
