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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_options.h"
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
        InsertStatement stmt(shardIdentity.toShardIdentityDocument());
        ASSERT_OK(
            collection_internal::insertDocument(operationContext(), *autoColl, stmt, nullptr));
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
