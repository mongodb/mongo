// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/sharding_environment/sharding_initialization_mongod.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace {

const std::string kShardName("TestShard");

/**
 * This test suite validates that when the default OpObserver chain is set up (which happens to
 * include the ShardingMongodOpObserver), writes to the 'admin.system.version' collection (and the
 * shardIdentity document specifically) will invoke the sharding initialization code.
 */
class ShardingInitializationOpObserverTest : public ShardingMongoDTestFixture {
protected:
    void setUp() override {
        ShardingMongoDTestFixture::setUp();

        // NOTE: this assumes that globalInit will always be called on the same thread as the main
        // test thread
        ShardingInitializationMongoD::get(operationContext())
            ->setGlobalInitMethodForTest(
                [this](OperationContext* opCtx, const ShardIdentity& shardIdentity) {
                    _initCallCount++;
                });
    }

    int getInitCallCount() const {
        return _initCallCount;
    }

private:
    service_context_test::ShardRoleOverride _shardRole;

    int _initCallCount = 0;
};

TEST_F(ShardingInitializationOpObserverTest, GlobalInitGetsCalledAfterWriteCommits) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::ConnectionType::kReplicaSet, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    DBDirectClient client(operationContext());
    client.insert(NamespaceString::createNamespaceString_forTest("admin.system.version"),
                  shardIdentity.toShardIdentityDocument());
    ASSERT_EQ(1, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, GlobalInitDoesntGetCalledIfWriteAborts) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::ConnectionType::kReplicaSet, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    // This part of the test ensures that the collection exists for the AutoGetCollection below to
    // find and also validates that the initializer does not get called for non-sharding documents
    DBDirectClient client(operationContext());
    client.insert(NamespaceString::createNamespaceString_forTest("admin.system.version"),
                  BSON("_id" << 1));
    ASSERT_EQ(0, getInitCallCount());

    {
        AutoGetCollection autoColl(
            operationContext(),
            NamespaceString::createNamespaceString_forTest("admin.system.version"),
            MODE_IX);

        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(Helpers::insert(
            operationContext(), *autoColl, shardIdentity.toShardIdentityDocument()));
        ASSERT_EQ(0, getInitCallCount());
    }

    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, GlobalInitDoesntGetsCalledIfNSIsNotForShardIdentity) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnectionString(
        ConnectionString(ConnectionString::ConnectionType::kReplicaSet, "a:1,b:2", "config"));
    shardIdentity.setShardName(kShardName);
    shardIdentity.setClusterId(OID::gen());

    DBDirectClient client(operationContext());
    client.insert(NamespaceString::createNamespaceString_forTest("admin.user"),
                  shardIdentity.toShardIdentityDocument());
    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(ShardingInitializationOpObserverTest, OnInsertOpThrowWithIncompleteShardIdentityDocument) {
    DBDirectClient client(operationContext());

    auto response = client.insertAcknowledged(
        NamespaceString::createNamespaceString_forTest("admin.system.version"),
        {BSON("_id" << ShardIdentityType::IdName << ShardIdentity::kShardNameFieldName
                    << kShardName)});

    ASSERT_NOT_OK(getStatusFromWriteCommandReply(response));
    ASSERT_EQ(0, response["n"].Int());
}

}  // namespace
}  // namespace mongo
