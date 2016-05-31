/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using std::string;
using std::vector;
using unittest::assertGet;

static const Seconds kFutureTimeout{5};

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

class RemoveShardTest : public ShardingCatalogTestFixture {
public:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(configHost);
    }

protected:
    const HostAndPort configHost{"TestHost1"};
};

TEST_F(RemoveShardTest, RemoveShardAnotherShardDraining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        ASSERT_EQUALS(ErrorCodes::ConflictingOperationInProgress,
                      catalogClient()->removeShard(operationContext(), shardName));
    });

    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                1);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardCantRemoveLastShard) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogClient()->removeShard(operationContext(), shardName));
    });

    // Report that there are no other draining operations ongoing
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Now report that there are no other shard left
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName),
                0);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStartDraining) {
    string shardName = "shardToRemove";
    const HostAndPort clientHost{"client1:12345"};
    setRemote(clientHost);

    auto future = launchAsync([&] {
        auto result = assertGet(catalogClient()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::STARTED, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName),
                1);

    // Report that the shard is not yet marked as draining
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                0);

    // Respond to request to update shard entry and mark it as draining.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(ShardType::ConfigNS, actualBatchedUpdate.getNS().ns());
        auto updates = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updates.size());
        auto update = updates.front();

        ASSERT_FALSE(update->getUpsert());
        ASSERT_FALSE(update->getMulti());
        ASSERT_EQUALS(BSON(ShardType::name() << shardName), update->getQuery());
        ASSERT_EQUALS(BSON("$set" << BSON(ShardType::draining(true))), update->getUpdateExpr());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    // Respond to request to reload information about existing shards
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        ShardType remainingShard;
        remainingShard.setHost("host1");
        remainingShard.setName("shard0");
        return vector<BSONObj>{remainingShard.toBSON()};
    });

    expectChangeLogCreate(configHost, BSON("ok" << 1));
    expectChangeLogInsert(
        configHost, network()->now(), "removeShard.start", "", BSON("shard" << shardName));

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingChunksRemaining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        auto result = assertGet(catalogClient()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::ONGOING, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName),
                1);

    // Report that the shard is already marked as draining
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are still chunks to drain
    expectCount(
        configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 10);

    // Report that there are no more databases to drain
    expectCount(configHost,
                NamespaceString(DatabaseType::ConfigNS),
                BSON(DatabaseType::primary(shardName)),
                0);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingDatabasesRemaining) {
    string shardName = "shardToRemove";

    auto future = launchAsync([&] {
        auto result = assertGet(catalogClient()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::ONGOING, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName),
                1);

    // Report that the shard is already marked as draining
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are no more chunks to drain
    expectCount(
        configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 0);

    // Report that there are still more databases to drain
    expectCount(configHost,
                NamespaceString(DatabaseType::ConfigNS),
                BSON(DatabaseType::primary(shardName)),
                5);

    future.timed_get(kFutureTimeout);
}

TEST_F(RemoveShardTest, RemoveShardCompletion) {
    string shardName = "shardToRemove";
    const HostAndPort clientHost{"client1:12345"};
    setRemote(clientHost);

    auto future = launchAsync([&] {
        auto result = assertGet(catalogClient()->removeShard(operationContext(), shardName));
        ASSERT_EQUALS(ShardDrainingStatus::COMPLETED, result);

    });

    // Report that there are no other draining operations ongoing
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName << ShardType::draining(true)),
                0);

    // Report that there *are* other shards left
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << NE << shardName),
                1);

    // Report that the shard is already marked as draining
    expectCount(configHost,
                NamespaceString(ShardType::ConfigNS),
                BSON(ShardType::name() << shardName << ShardType::draining(true)),
                1);

    // Report that there are no more chunks to drain
    expectCount(
        configHost, NamespaceString(ChunkType::ConfigNS), BSON(ChunkType::shard(shardName)), 0);

    // Report that there are no more databases to drain
    expectCount(configHost,
                NamespaceString(DatabaseType::ConfigNS),
                BSON(DatabaseType::primary(shardName)),
                0);

    // Respond to request to remove shard entry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedDeleteRequest actualBatchedDelete;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedDelete.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(ShardType::ConfigNS, actualBatchedDelete.getNS().ns());
        auto deletes = actualBatchedDelete.getDeletes();
        ASSERT_EQUALS(1U, deletes.size());
        auto deleteOp = deletes.front();

        ASSERT_EQUALS(0, deleteOp->getLimit());
        ASSERT_EQUALS(BSON(ShardType::name() << shardName), deleteOp->getQuery());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    // Respond to request to reload information about existing shards
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        ShardType remainingShard;
        remainingShard.setHost("host1");
        remainingShard.setName("shard0");
        return vector<BSONObj>{remainingShard.toBSON()};
    });

    expectChangeLogCreate(configHost, BSON("ok" << 1));
    expectChangeLogInsert(
        configHost, network()->now(), "removeShard", "", BSON("shard" << shardName));

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
