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

#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_id.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using std::string;
using std::vector;
using unittest::assertGet;

const KeyPattern kKeyPattern(BSON("_id" << 1));

class RemoveShardTest : public ConfigServerTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        setUpAndInitializeConfigDb();

        auto clusterIdLoader = ClusterIdentityLoader::get(operationContext());
        ASSERT_OK(clusterIdLoader->loadClusterId(
            operationContext(), catalogClient(), repl::ReadConcernLevel::kLocalReadConcern));
        _clusterId = clusterIdLoader->getClusterId();

        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        // Updating the cluster cardinality parameter and blocking ShardingDDLCoordinators require
        // the primary only services to have been set up.
        _skipUpdatingCardinalityParamFP =
            globalFailPointRegistry().find("skipUpdatingClusterCardinalityParameterAfterAddShard");
        _skipUpdatingCardinalityParamFP->setMode(FailPoint::alwaysOn);

        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP =
            globalFailPointRegistry().find("skipBlockingDDLCoordinatorsDuringAddAndRemoveShard");
        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP->setMode(FailPoint::alwaysOn);
    }

    void tearDown() override {
        _skipUpdatingCardinalityParamFP->setMode(FailPoint::off);
        _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP->setMode(FailPoint::off);
        TransactionCoordinatorService::get(operationContext())->interrupt();
        ConfigServerTestFixture::tearDown();
    }

    /**
     * Checks whether a particular shard's "draining" field is set to true.
     */
    bool isDraining(const std::string& shardName) {
        auto response = assertGet(shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::kConfigsvrShardsNamespace,
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
    ReadWriteConcernDefaultsLookupMock _lookupMock;
    FailPoint* _skipUpdatingCardinalityParamFP;
    FailPoint* _skipBlockingDDLCoordinatorsDuringAddAndRemoveShardFP;
};

TEST_F(RemoveShardTest, RemoveShardAnotherShardDraining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    ShardType shard3;
    shard3.setName("shard3");
    shard3.setHost("host3:12345");
    shard3.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1, shard2, shard3});

    auto result = ShardingCatalogManager::get(operationContext())
                      ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, result.status);
    ASSERT_EQUALS(false, result.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto result2 = ShardingCatalogManager::get(operationContext())
                       ->removeShard(operationContext(), shard2.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, result2.status);
    ASSERT_EQUALS(false, result2.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard2.getName()));
}

TEST_F(RemoveShardTest, RemoveShardCantRemoveLastShard) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->removeShard(operationContext(), shard1.getName()),
                       DBException,
                       ErrorCodes::IllegalOperation);
    ASSERT_FALSE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStartDraining) {
    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1, shard2});

    auto result = ShardingCatalogManager::get(operationContext())
                      ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, result.status);
    ASSERT_EQUALS(false, result.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingChunksRemaining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    auto epoch = OID::gen();
    const auto uuid = UUID::gen();
    const auto timestamp = Timestamp(1);
    ChunkType chunk1(uuid,
                     ChunkRange(BSON("_id" << 0), BSON("_id" << 20)),
                     ChunkVersion({epoch, timestamp}, {1, 1}),
                     shard1.getName());
    ChunkType chunk2(uuid,
                     ChunkRange(BSON("_id" << 21), BSON("_id" << 50)),
                     ChunkVersion({epoch, timestamp}, {1, 2}),
                     shard1.getName());
    ChunkType chunk3(uuid,
                     ChunkRange(BSON("_id" << 51), BSON("_id" << 1000)),
                     ChunkVersion({epoch, timestamp}, {1, 3}),
                     shard1.getName());

    chunk3.setJumbo(true);

    setupShards(std::vector<ShardType>{shard1, shard2});
    setupDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
                  shard1.getName());
    setupCollection(NamespaceString::createNamespaceString_forTest("testDB.testColl"),
                    kKeyPattern,
                    std::vector<ChunkType>{chunk1, chunk2, chunk3});

    auto startedResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, startedResult.status);
    ASSERT_EQUALS(false, startedResult.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::ONGOING, ongoingResult.status);
    ASSERT_EQUALS(true, ongoingResult.remainingCounts.has_value());
    ASSERT_EQUALS(3, ongoingResult.remainingCounts->totalChunks);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->totalCollections);
    ASSERT_EQUALS(1, ongoingResult.remainingCounts->jumboChunks);
    ASSERT_EQUALS(1, ongoingResult.remainingCounts->databases);
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingDatabasesRemaining) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1, shard2});
    setupDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
                  shard1.getName());

    auto startedResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, startedResult.status);
    ASSERT_EQUALS(false, startedResult.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::ONGOING, ongoingResult.status);
    ASSERT_EQUALS(true, ongoingResult.remainingCounts.has_value());
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->totalChunks);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->totalCollections);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->jumboChunks);
    ASSERT_EQUALS(1, ongoingResult.remainingCounts->databases);
    ASSERT_TRUE(isDraining(shard1.getName()));
}

TEST_F(RemoveShardTest, RemoveShardCompletion) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    auto epoch = OID::gen();
    auto uuid = UUID::gen();
    Timestamp timestamp = Timestamp(1);
    ChunkType chunk1(uuid,
                     ChunkRange(BSON("_id" << 0), BSON("_id" << 20)),
                     ChunkVersion({epoch, timestamp}, {1, 1}),
                     shard1.getName());
    ChunkType chunk2(uuid,
                     ChunkRange(BSON("_id" << 21), BSON("_id" << 50)),
                     ChunkVersion({epoch, timestamp}, {1, 2}),
                     shard1.getName());
    ChunkType chunk3(uuid,
                     ChunkRange(BSON("_id" << 51), BSON("_id" << 1000)),
                     ChunkVersion({epoch, timestamp}, {1, 3}),
                     shard1.getName());

    std::vector<ChunkType> chunks{chunk1, chunk2, chunk3};

    setupShards(std::vector<ShardType>{shard1, shard2});
    setupDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
                  shard2.getName());
    setupCollection(NamespaceString::createNamespaceString_forTest("testDB.testColl"),
                    kKeyPattern,
                    std::vector<ChunkType>{chunk1, chunk2, chunk3});

    auto startedResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::STARTED, startedResult.status);
    ASSERT_EQUALS(false, startedResult.remainingCounts.has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = ShardingCatalogManager::get(operationContext())
                             ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::ONGOING, ongoingResult.status);
    ASSERT_EQUALS(true, ongoingResult.remainingCounts.has_value());
    ASSERT_EQUALS(3, ongoingResult.remainingCounts->totalChunks);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->totalCollections);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->jumboChunks);
    ASSERT_EQUALS(0, ongoingResult.remainingCounts->databases);
    ASSERT_TRUE(isDraining(shard1.getName()));

    // Mock the operation during which the chunks are moved to the other shard.
    const NamespaceString chunkNS(NamespaceString::kConfigsvrChunksNamespace);
    for (const ChunkType& chunk : chunks) {
        ChunkType updatedChunk = chunk;
        updatedChunk.setShard(shard2.getName());
        ASSERT_OK(updateToConfigCollection(
            operationContext(), chunkNS, chunk.toConfigBSON(), updatedChunk.toConfigBSON(), false));
    }

    auto completedResult = ShardingCatalogManager::get(operationContext())
                               ->removeShard(operationContext(), shard1.getName());
    ASSERT_EQUALS(RemoveShardProgress::COMPLETED, completedResult.status);
    ASSERT_EQUALS(false, startedResult.remainingCounts.has_value());

    // Now make sure that the shard no longer exists on config.
    auto response = assertGet(shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        operationContext(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name() << shard1.getName()),
        BSONObj(),
        1));
    ASSERT_TRUE(response.docs.empty());
}

}  // namespace
}  // namespace mongo
