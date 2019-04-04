/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/client/remote_command_targeter_mock.h"
#include "merizo/db/catalog_raii.h"
#include "merizo/db/concurrency/d_concurrency.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/op_observer_impl.h"
#include "merizo/db/op_observer_registry.h"
#include "merizo/db/repl/replication_coordinator_mock.h"
#include "merizo/db/s/config_server_op_observer.h"
#include "merizo/db/s/shard_server_catalog_cache_loader.h"
#include "merizo/db/s/shard_server_op_observer.h"
#include "merizo/db/s/sharding_initialization_merizod.h"
#include "merizo/db/s/type_shard_identity.h"
#include "merizo/db/server_options.h"
#include "merizo/s/catalog/dist_lock_manager_mock.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/config_server_catalog_cache_loader.h"
#include "merizo/s/sharding_merizod_test_fixture.h"

namespace merizo {
namespace {

const std::string kShardName("TestShard");

/**
 * This test suite validates that when the default OpObserver chain is set up (which happens to
 * include the ShardingMerizodOpObserver), writes to the 'admin.system.version' collection (and the
 * shardIdentity document specifically) will invoke the sharding initialization code.
 */
class ShardingInitializationOpObserverTest : public ShardingMerizodTestFixture {
public:
    void setUp() override {
        ShardingMerizodTestFixture::setUp();

        // NOTE: this assumes that globalInit will always be called on the same thread as the main
        // test thread
        ShardingInitializationMerizoD::get(operationContext())
            ->setGlobalInitMethodForTest([this](OperationContext* opCtx,
                                                const ShardIdentity& shardIdentity,
                                                StringData distLockProcessId) {
                _initCallCount++;
                return Status::OK();
            });
    }

    void tearDown() override {
        ShardingState::get(getServiceContext())->clearForTests();

        ShardingMerizodTestFixture::tearDown();
    }

    int getInitCallCount() const {
        return _initCallCount;
    }

private:
    int _initCallCount = 0;
};

TEST_F(ShardingInitializationOpObserverTest, GlobalInitGetsCalledAfterWriteCommits) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    DBDirectClient client(operationContext());
    client.insert("admin.system.version", shardIdentity.toShardIdentityDocument());
    ASSERT_EQ(1, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, GlobalInitDoesntGetCalledIfWriteAborts) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    // This part of the test ensures that the collection exists for the AutoGetCollection below to
    // find and also validates that the initializer does not get called for non-sharding documents
    DBDirectClient client(operationContext());
    client.insert("admin.system.version", BSON("_id" << 1));
    ASSERT_EQ(0, getInitCallCount());

    {
        AutoGetCollection autoColl(
            operationContext(), NamespaceString("admin.system.version"), MODE_IX);

        WriteUnitOfWork wuow(operationContext());
        InsertStatement stmt(shardIdentity.toShardIdentityDocument());
        ASSERT_OK(autoColl.getCollection()->insertDocument(operationContext(), stmt, nullptr));
        ASSERT_EQ(0, getInitCallCount());
    }

    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, GlobalInitDoesntGetsCalledIfNSIsNotForShardIdentity) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    DBDirectClient client(operationContext());
    client.insert("admin.user", shardIdentity.toShardIdentityDocument());
    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, OnInsertOpThrowWithIncompleteShardIdentityDocument) {
    DBDirectClient client(operationContext());
    client.insert("admin.system.version",
                  BSON("_id" << ShardIdentityType::IdName << ShardIdentity::kShardNameFieldName
                             << kShardName));
    ASSERT(!client.getLastError().empty());
}

}  // namespace
}  // namespace merizo
