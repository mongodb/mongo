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
#include "mongo/db/ops/write_ops.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/grid.h"
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

const Seconds kFutureTimeout{5};

BSONObj getReplSecondaryOkMetadata() {
    BSONObjBuilder o;
    ReadPreferenceSetting(ReadPreference::Nearest).toContainingBSON(&o);
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}

class RemoveShardTest : public ConfigServerTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Make sure clusterID is written to the config.version collection.
        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));

        auto clusterIdLoader = ClusterIdentityLoader::get(operationContext());
        ASSERT_OK(clusterIdLoader->loadClusterId(operationContext(),
                                                 repl::ReadConcernLevel::kLocalReadConcern));
        _clusterId = clusterIdLoader->getClusterId();
    }

    /**
     * Checks whether a particular shard's "draining" field is set to true.
     */
    bool isDraining(const std::string& shardName) {
        auto response = assertGet(shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            NamespaceString(ShardType::ConfigNS),
            BSON(ShardType::name() << shardName),
            BSONObj(),
            1));
        BSONObj shardBSON = response.docs.front();
        if (shardBSON.hasField("draining")) {
            return shardBSON["draining"].Bool();
        }
        return false;
    }

    const HostAndPort configHost{"TestHost1"};
    OID _clusterId;
};

TEST_F(RemoveShardTest, RemoveShardAnotherShardDraining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setMaxSizeMB(100);
    shard2.setState(ShardType::ShardState::kShardAware);

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1, shard2}));

    auto result = assertGet(ShardingCatalogManager::get(operationContext())
                                ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::STARTED, result);
    ASSERT_TRUE(isDraining(shard1.getName()));

    ASSERT_EQUALS(ErrorCodes::ConflictingOperationInProgress,
                  ShardingCatalogManager::get(operationContext())
                      ->removeShard(operationContext(), shard2.getName()));
    ASSERT_FALSE(isDraining(shard2.getName()));
}

TEST_F(RemoveShardTest, RemoveShardCantRemoveLastShard) {
    string shardName = "shardToRemove";

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1}));

    ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                  ShardingCatalogManager::get(operationContext())
                      ->removeShard(operationContext(), shard1.getName()));
    ASSERT_FALSE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStartDraining) {
    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setMaxSizeMB(100);
    shard2.setState(ShardType::ShardState::kShardAware);

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1, shard2}));

    auto result = assertGet(ShardingCatalogManager::get(operationContext())
                                ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::STARTED, result);
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingChunksRemaining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setMaxSizeMB(100);
    shard2.setState(ShardType::ShardState::kShardAware);

    auto epoch = OID::gen();
    ChunkType chunk1(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 0), BSON("_id" << 20)),
                     ChunkVersion(1, 1, epoch),
                     shard1.getName());
    ChunkType chunk2(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 21), BSON("_id" << 50)),
                     ChunkVersion(1, 2, epoch),
                     shard1.getName());
    ChunkType chunk3(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 51), BSON("_id" << 1000)),
                     ChunkVersion(1, 3, epoch),
                     shard1.getName());

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1, shard2}));
    setupDatabase("testDB", shard1.getName(), true);
    ASSERT_OK(setupChunks(std::vector<ChunkType>{chunk1, chunk2, chunk3}));

    auto startedResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::STARTED, startedResult);
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::ONGOING, ongoingResult);
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingDatabasesRemaining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setMaxSizeMB(100);
    shard2.setState(ShardType::ShardState::kShardAware);

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1, shard2}));
    setupDatabase("testDB", shard1.getName(), false);

    auto startedResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::STARTED, startedResult);
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::ONGOING, ongoingResult);
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardCompletion) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setMaxSizeMB(100);
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setMaxSizeMB(100);
    shard2.setState(ShardType::ShardState::kShardAware);

    auto epoch = OID::gen();
    ChunkType chunk1(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 0), BSON("_id" << 20)),
                     ChunkVersion(1, 1, epoch),
                     shard1.getName());
    ChunkType chunk2(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 21), BSON("_id" << 50)),
                     ChunkVersion(1, 2, epoch),
                     shard1.getName());
    ChunkType chunk3(NamespaceString("testDB.testColl"),
                     ChunkRange(BSON("_id" << 51), BSON("_id" << 1000)),
                     ChunkVersion(1, 3, epoch),
                     shard1.getName());

    std::vector<ChunkType> chunks{chunk1, chunk2, chunk3};

    ASSERT_OK(setupShards(std::vector<ShardType>{shard1, shard2}));
    setupDatabase("testDB", shard2.getName(), false);
    ASSERT_OK(setupChunks(std::vector<ChunkType>{chunk1, chunk2, chunk3}));

    auto startedResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::STARTED, startedResult);
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = assertGet(ShardingCatalogManager::get(operationContext())
                                       ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::ONGOING, ongoingResult);
    ASSERT_TRUE(isDraining(shard1.getName()));

    // Mock the operation during which the chunks are moved to the other shard.
    const NamespaceString chunkNS(ChunkType::ConfigNS);
    for (ChunkType chunk : chunks) {
        ChunkType updatedChunk = chunk;
        updatedChunk.setShard(shard2.getName());
        ASSERT_OK(updateToConfigCollection(
            operationContext(), chunkNS, chunk.toConfigBSON(), updatedChunk.toConfigBSON(), false));
    }

    auto completedResult = assertGet(ShardingCatalogManager::get(operationContext())
                                         ->removeShard(operationContext(), shard1.getName()));
    ASSERT_EQUALS(ShardDrainingStatus::COMPLETED, completedResult);

    // Now make sure that the shard no longer exists on config.
    auto response = assertGet(shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        operationContext(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kMajorityReadConcern,
        NamespaceString(ShardType::ConfigNS),
        BSON(ShardType::name() << shard1.getName()),
        BSONObj(),
        1));
    ASSERT_TRUE(response.docs.empty());
}

}  // namespace
}  // namespace mongo
