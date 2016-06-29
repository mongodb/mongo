/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class CollShardingStateTest : public mongo::unittest::Test {
public:
    void setUp() override {
        _service.setFastClockSource(stdx::make_unique<ClockSourceMock>());
        _service.setPreciseClockSource(stdx::make_unique<ClockSourceMock>());

        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _client = _service.makeClient("ShardingStateTest");
        // This skips version checking to avoid accessing a null ReplicationCoordinator.
        _client->setInDirectClient(true);
        _opCtx = _client->makeOperationContext();

        // Note: this assumes that globalInit will always be called on the same thread as the main
        // test thread.
        ShardingState::get(txn())->setGlobalInitMethodForTest(
            [this](OperationContext*, const ConnectionString&, StringData) {
                _initCallCount++;
                return Status::OK();
            });
    }

    void tearDown() override {}

    OperationContext* txn() {
        return _opCtx.get();
    }

    int getInitCallCount() const {
        return _initCallCount;
    }

private:
    ServiceContextNoop _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

    int _initCallCount = 0;
};

TEST_F(CollShardingStateTest, GlobalInitGetsCalledAfterWriteCommits) {
    std::unique_ptr<CollectionMetadata> nullCollMetadata;
    CollectionShardingState collShardingState(NamespaceString::kConfigCollectionNamespace,
                                              std::move(nullCollMetadata));

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    WriteUnitOfWork wuow(txn());
    collShardingState.onInsertOp(txn(), shardIdentity.toBSON());

    ASSERT_EQ(0, getInitCallCount());

    wuow.commit();

    ASSERT_EQ(1, getInitCallCount());
}

TEST_F(CollShardingStateTest, GlobalInitDoesntGetCalledIfWriteAborts) {
    std::unique_ptr<CollectionMetadata> nullCollMetadata;
    CollectionShardingState collShardingState(NamespaceString::kConfigCollectionNamespace,
                                              std::move(nullCollMetadata));

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    {
        WriteUnitOfWork wuow(txn());
        collShardingState.onInsertOp(txn(), shardIdentity.toBSON());

        ASSERT_EQ(0, getInitCallCount());
    }

    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(CollShardingStateTest, GlobalInitDoesntGetsCalledIfNSIsNotForShardIdentity) {
    std::unique_ptr<CollectionMetadata> nullCollMetadata;
    CollectionShardingState collShardingState(NamespaceString("admin.user"),
                                              std::move(nullCollMetadata));

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    WriteUnitOfWork wuow(txn());
    collShardingState.onInsertOp(txn(), shardIdentity.toBSON());

    ASSERT_EQ(0, getInitCallCount());

    wuow.commit();

    ASSERT_EQ(0, getInitCallCount());
}

TEST_F(CollShardingStateTest, OnInsertOpThrowWithIncompleteShardIdentityDocument) {
    std::unique_ptr<CollectionMetadata> nullCollMetadata;
    CollectionShardingState collShardingState(NamespaceString::kConfigCollectionNamespace,
                                              std::move(nullCollMetadata));

    ShardIdentityType shardIdentity;
    shardIdentity.setShardName("a");

    ASSERT_THROWS(collShardingState.onInsertOp(txn(), shardIdentity.toBSON()), AssertionException);
}

TEST_F(CollShardingStateTest, GlobalInitDoesntGetsCalledIfShardIdentityDocWasNotInserted) {
    std::unique_ptr<CollectionMetadata> nullCollMetadata;
    CollectionShardingState collShardingState(NamespaceString::kConfigCollectionNamespace,
                                              std::move(nullCollMetadata));

    WriteUnitOfWork wuow(txn());
    collShardingState.onInsertOp(txn(), BSON("_id" << 1));

    ASSERT_EQ(0, getInitCallCount());

    wuow.commit();

    ASSERT_EQ(0, getInitCallCount());
}

}  // unnamed namespace
}  // namespace mongo
