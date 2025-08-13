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

#include "mongo/db/sharding_environment/client/shard.h"

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
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/cluster_identity_loader.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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

    RemoveShardProgress removeShardCommandFlow(OperationContext* opCtx, const ShardId& shardId) {
        if (auto drainingProgress =
                ShardingCatalogManager::get(opCtx)->checkPreconditionsAndStartDrain(opCtx,
                                                                                    shardId)) {
            return *drainingProgress;
        }
        auto drainingProgress =
            ShardingCatalogManager::get(opCtx)->checkDrainingProgress(opCtx, shardId);
        if (drainingProgress.getState() != ShardDrainingStateEnum::kDrainingComplete) {
            return drainingProgress;
        }
        return ShardingCatalogManager::get(opCtx)->removeShard(opCtx, shardId);
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

    void setupManyDatabases(int startIndex, int endIndex, const ShardId& primaryShard) {
        for (int i = startIndex; i <= endIndex; i++) {
            auto databaseName =
                fmt::format("testDB_1234567890123456789012345678901234567890_{}", i);
            setupDatabase(DatabaseName::createDatabaseName_forTest(boost::none, databaseName),
                          primaryShard);
        }
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

    auto result = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, result.getState());
    ASSERT_EQUALS(false, result.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto result2 = removeShardCommandFlow(operationContext(), shard2.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, result2.getState());
    ASSERT_EQUALS(false, result2.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard2.getName()));
}

TEST_F(RemoveShardTest, RemoveShardCantRemoveLastShard) {

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1});

    ASSERT_THROWS_CODE(removeShardCommandFlow(operationContext(), shard1.getName()),
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

    auto result = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, result.getState());
    ASSERT_EQUALS(false, result.getRemaining().has_value());
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

    auto startedResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, startedResult.getState());
    ASSERT_EQUALS(false, startedResult.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kOngoing, ongoingResult.getState());
    ASSERT_EQUALS(true, ongoingResult.getRemaining().has_value());
    ASSERT_EQUALS(3, ongoingResult.getRemaining()->getChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getCollectionsToMove());
    ASSERT_EQUALS(1, ongoingResult.getRemaining()->getJumboChunks());
    ASSERT_EQUALS(1, ongoingResult.getRemaining()->getDbs());
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

    auto startedResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, startedResult.getState());
    ASSERT_EQUALS(false, startedResult.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kOngoing, ongoingResult.getState());
    ASSERT_EQUALS(true, ongoingResult.getRemaining().has_value());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getCollectionsToMove());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getJumboChunks());
    ASSERT_EQUALS(1, ongoingResult.getRemaining()->getDbs());
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

    auto startedResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, startedResult.getState());
    ASSERT_EQUALS(false, startedResult.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kOngoing, ongoingResult.getState());
    ASSERT_EQUALS(true, ongoingResult.getRemaining().has_value());
    ASSERT_EQUALS(3, ongoingResult.getRemaining()->getChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getCollectionsToMove());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getJumboChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getDbs());
    ASSERT_TRUE(isDraining(shard1.getName()));

    // Mock the operation during which the chunks are moved to the other shard.
    const NamespaceString chunkNS(NamespaceString::kConfigsvrChunksNamespace);
    for (const ChunkType& chunk : chunks) {
        ChunkType updatedChunk = chunk;
        updatedChunk.setShard(shard2.getName());
        ASSERT_OK(updateToConfigCollection(
            operationContext(), chunkNS, chunk.toConfigBSON(), updatedChunk.toConfigBSON(), false));
    }

    auto completedResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kCompleted, completedResult.getState());
    ASSERT_EQUALS(false, startedResult.getRemaining().has_value());

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

TEST_F(RemoveShardTest, RemoveShardCommitWithPreconditionsNotMet) {
    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("host1:12345");
    shard1.setState(ShardType::ShardState::kShardAware);

    ShardType shard2;
    shard2.setName("shard2");
    shard2.setHost("host2:12345");
    shard2.setState(ShardType::ShardState::kShardAware);

    setupShards(std::vector<ShardType>{shard1, shard2});

    // Removing a shard that was already removed or does not exist should throw ShardNotFound.
    ShardId nonExistingShard{"fakeShardId"};
    auto res = ShardingCatalogManager::get(operationContext())
                   ->removeShard(operationContext(), nonExistingShard);
    ASSERT_EQ(res.getState(), ShardDrainingStateEnum::kCompleted);
    // Calling the final function in the shard removal procedure on a shard which is not draining
    // should throw ConflictingOperationInProgress as this should only happen if a sequence of
    // parallel add/remove shard operations or a manual update of the draining flag occurred. This
    // error should be handled by the caller.
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->removeShard(operationContext(), shard1.getName()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(RemoveShardTest, RemoveShardStillDrainingChunksRemainingMaxBSONSize) {

    FailPointEnableBlock changeMaxUserSizeFP("changeBSONObjMaxUserSize",
                                             BSON("maxUserSize" << 20480));

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

    // Add so many databases, that the resulting BSON does not exceed the max size.
    // dbsToMove array has to contain all databases and "truncated" field does not exist.
    setupManyDatabases(1, 100, shard1.getName());

    setupCollection(NamespaceString::createNamespaceString_forTest(
                        "testDB_1234567890123456789012345678901234567890_1.testColl"),
                    kKeyPattern,
                    std::vector<ChunkType>{chunk1, chunk2, chunk3});

    auto startedResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kStarted, startedResult.getState());
    ASSERT_EQUALS(false, startedResult.getRemaining().has_value());
    ASSERT_TRUE(isDraining(shard1.getName()));

    auto ongoingResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kOngoing, ongoingResult.getState());
    ASSERT_EQUALS(true, ongoingResult.getRemaining().has_value());
    ASSERT_EQUALS(3, ongoingResult.getRemaining()->getChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getCollectionsToMove());
    ASSERT_EQUALS(1, ongoingResult.getRemaining()->getJumboChunks());
    ASSERT_EQUALS(100, ongoingResult.getRemaining()->getDbs());
    ASSERT_TRUE(isDraining(shard1.getName()));

    BSONObjBuilder result;

    ShardingCatalogManager::get(operationContext())
        ->appendShardDrainingStatus(operationContext(), result, ongoingResult, shard1.getName());
    auto resultObj = result.obj();
    ASSERT_FALSE(resultObj.hasElement("truncated"));

    ASSERT_EQUALS(100, resultObj["dbsToMove"].Array().size());

    // Add more databases, that the resulting BSON exceeds the max size.
    // dbsToMove array must contain only a subset of the databases and "truncated" field is enabled.
    setupManyDatabases(101, 200, shard1.getName());

    ongoingResult = removeShardCommandFlow(operationContext(), shard1.getName());
    ASSERT_EQUALS(ShardDrainingStateEnum::kOngoing, ongoingResult.getState());
    ASSERT_EQUALS(true, ongoingResult.getRemaining().has_value());
    ASSERT_EQUALS(3, ongoingResult.getRemaining()->getChunks());
    ASSERT_EQUALS(0, ongoingResult.getRemaining()->getCollectionsToMove());
    ASSERT_EQUALS(1, ongoingResult.getRemaining()->getJumboChunks());
    ASSERT_EQUALS(200, ongoingResult.getRemaining()->getDbs());
    ASSERT_TRUE(isDraining(shard1.getName()));

    BSONObjBuilder resultTruncated;

    ShardingCatalogManager::get(operationContext())
        ->appendShardDrainingStatus(
            operationContext(), resultTruncated, ongoingResult, shard1.getName());
    resultObj = resultTruncated.obj();
    ASSERT_TRUE(resultObj.hasElement("truncated"));
    ASSERT_TRUE(resultObj["truncated"].booleanSafe());

    ASSERT_EQUALS(167, resultObj["dbsToMove"].Array().size());
}

}  // namespace
}  // namespace mongo
