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
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"
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
};

TEST_F(ConfigServerOpObserverTest, NodeClearsCatalogManagerOnConfigVersionRollBack) {
    ConfigServerOpObserver opObserver;
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = true;

    opObserver.onReplicationRollback(operationContext(), rbInfo);

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigServerOpObserverTest, NodeDoesNotClearCatalogManagerWhenConfigVersionNotRolledBack) {
    ConfigServerOpObserver opObserver;
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = false;

    opObserver.onReplicationRollback(operationContext(), rbInfo);

    ASSERT_EQ(ErrorCodes::AlreadyInitialized,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

DEATH_TEST_F(ConfigServerOpObserverTest,
             NodeClearsClusterIDOnConfigVersionRollBack,
             "Invariant failure") {
    ConfigServerOpObserver opObserver;
    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.configServerConfigVersionRolledBack = true;

    opObserver.onReplicationRollback(operationContext(), rbInfo);

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
    ConfigServerOpObserver opObserver;

    repl::OpTime a(Timestamp(1, 1), 1);
    repl::OpTime b(Timestamp(1, 2), 1);

    opObserver.onMajorityCommitPointUpdate(getServiceContext(), a);
    const auto aTime = VectorClock::get(getServiceContext())->getTime();
    ASSERT_EQ(a.getTimestamp(), aTime.configTime().asTimestamp());

    opObserver.onMajorityCommitPointUpdate(getServiceContext(), b);
    const auto bTime = VectorClock::get(getServiceContext())->getTime();
    ASSERT_EQ(b.getTimestamp(), bTime.configTime().asTimestamp());
}

}  // namespace
}  // namespace mongo
