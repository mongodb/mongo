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

#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using unittest::assertGet;

class ConfigInitializationTest : public ConfigServerTestFixture {
protected:
    /* Generate and insert the entries into the config.shards collection
     * Given the desired number of shards n, generates a vector of n ShardType objects (in BSON
     * format) according to the following template,  Given the i-th element :
     *  - shard_id : shard<i>
     *  - host : localhost:3000<i>
     *  - state : always 1 (kShardAware)
     */
    void setupConfigShard(OperationContext* opCtx, int nShards) {
        for (auto& doc : _generateConfigShardSampleData(nShards)) {
            ASSERT_OK(
                insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
        }
    }

    /* Generate and insert the entries into the config.database collection
     * Given the desired number of db n, generates a vector of n DatabaseType objects (in BSON
     * format) according to the following template,  Given the i-th element :
     *  - dbName : db<i>
     *  - primaryShard :  shard<i>
     *  - databaseVersion : always DatabaseVersion::makeFixed()
     */
    void setupConfigDatabase(OperationContext* opCtx, int nDbs) {
        for (int i = 1; i <= nDbs; i++) {
            const std::string dbName = "db" + std::to_string(i);
            const std::string shardName = "shard" + std::to_string(i);
            const DatabaseType dbEntry(dbName, ShardId(shardName), DatabaseVersion::makeFixed());
            ASSERT_OK(insertToConfigCollection(
                opCtx, NamespaceString::kConfigDatabasesNamespace, dbEntry.toBSON()));
        }
    }

    /*
    Helper function to check a returned placement type against the expected values.
    */
    void assertPlacementType(const NamespacePlacementType& result,
                             NamespaceString&& expectedNss,
                             const Timestamp& expectedTimestamp,
                             std::vector<std::string>&& expectedShards) {

        std::vector<ShardId> expectedShardIds;
        std::transform(expectedShards.begin(),
                       expectedShards.end(),
                       std::back_inserter(expectedShardIds),
                       [](const std::string& s) { return ShardId(s); });

        ASSERT_EQ(result.getTimestamp(), expectedTimestamp);
        ASSERT_EQ(result.getNss(), expectedNss);

        auto shards = result.getShards();
        std::sort(shards.begin(), shards.end());
        std::sort(expectedShardIds.begin(), expectedShardIds.end());

        ASSERT_EQ(expectedShardIds, shards);
    }

private:
    std::vector<BSONObj> _generateConfigShardSampleData(int nShards) const {
        std::vector<BSONObj> configShardData;
        for (int i = 1; i <= nShards; i++) {
            const std::string shardName = "shard" + std::to_string(i);
            const std::string shardHost = "localhost:" + std::to_string(30000 + i);
            const auto& doc = BSON("_id" << shardName << "host" << shardHost << "state" << 1);

            configShardData.push_back(doc);
        }

        return configShardData;
    }
};

TEST_F(ConfigInitializationTest, InitClusterMultipleVersionDocs) {
    VersionType version;
    version.setClusterId(OID::gen());
    ASSERT_OK(
        insertToConfigCollection(operationContext(), VersionType::ConfigNS, version.toBSON()));

    ASSERT_OK(insertToConfigCollection(operationContext(),
                                       VersionType::ConfigNS,
                                       BSON("_id"
                                            << "a second document")));

    ASSERT_EQ(ErrorCodes::TooManyMatchingDocuments,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, InitInvalidConfigVersionDoc) {
    BSONObj versionDoc(fromjson(R"({
                    _id: 1,
                    clusterId: "should be an ID"
                })"));
    ASSERT_OK(insertToConfigCollection(operationContext(), VersionType::ConfigNS, versionDoc));

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}


TEST_F(ConfigInitializationTest, InitNoVersionDocEmptyConfig) {
    // Make sure there is no existing document
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc =
        assertGet(findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());
}

TEST_F(ConfigInitializationTest, OnlyRunsOnce) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc =
        assertGet(findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());

    ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                  ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, ReRunsIfDocRolledBackThenReElected) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc =
        assertGet(findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    VersionType foundVersion = assertGet(VersionType::fromBSON(versionDoc));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());

    // Now remove the version document and re-run initializeConfigDatabaseIfNeeded().
    {
        // Mirror what happens if the config.version document is rolled back.
        ON_BLOCK_EXIT([&] {
            replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY).ignore();
        });
        ASSERT_OK(replicationCoordinator()->setFollowerMode(repl::MemberState::RS_ROLLBACK));
        auto opCtx = operationContext();
        repl::UnreplicatedWritesBlock uwb(opCtx);
        auto nss = VersionType::ConfigNS;
        writeConflictRetry(opCtx, "removeConfigDocuments", nss.ns(), [&] {
            AutoGetCollection coll(opCtx, nss, MODE_IX);
            ASSERT_TRUE(coll);
            auto cursor = coll->getCursor(opCtx);
            std::vector<RecordId> recordIds;
            while (auto recordId = cursor->next()) {
                recordIds.push_back(recordId->id);
            }
            mongo::WriteUnitOfWork wuow(opCtx);
            for (const auto& recordId : recordIds) {
                collection_internal::deleteDocument(
                    opCtx, *coll, kUninitializedStmtId, recordId, nullptr);
            }
            wuow.commit();
            ASSERT_EQUALS(0UL, coll->numRecords(opCtx));
        });
    }

    // Verify the document was actually removed.
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    // Re-create the config.version document.
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto newVersionDoc =
        assertGet(findOneOnConfigCollection(operationContext(), VersionType::ConfigNS, BSONObj()));

    VersionType newFoundVersion = assertGet(VersionType::fromBSON(newVersionDoc));

    ASSERT_TRUE(newFoundVersion.getClusterId().isSet());
    ASSERT_NOT_EQUALS(newFoundVersion.getClusterId(), foundVersion.getClusterId());
}

TEST_F(ConfigInitializationTest, BuildsNecessaryIndexes) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    std::vector<BSONObj> expectedChunksIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"),
        BSON("v" << 2 << "key" << BSON("uuid" << 1 << "min" << 1) << "name"
                 << "uuid_1_min_1"
                 << "unique" << true),
        BSON("v" << 2 << "key" << BSON("uuid" << 1 << "shard" << 1 << "min" << 1) << "name"
                 << "uuid_1_shard_1_min_1"
                 << "unique" << true),
        BSON("v" << 2 << "key" << BSON("uuid" << 1 << "lastmod" << 1) << "name"
                 << "uuid_1_lastmod_1"
                 << "unique" << true),
        BSON("v" << 2 << "key" << BSON("uuid" << 1 << "shard" << 1 << "onCurrentShardSince" << 1)
                 << "name"
                 << "uuid_1_shard_1_onCurrentShardSince_1")};

    auto expectedShardsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("host" << 1) << "name"
                 << "host_1")};
    auto expectedTagsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                 << "_id_"),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "min" << 1) << "name"
                 << "ns_1_min_1"),
        BSON("v" << 2 << "key" << BSON("ns" << 1 << "tag" << 1) << "name"
                 << "ns_1_tag_1")};

    auto foundChunksIndexes = assertGet(getIndexes(operationContext(), ChunkType::ConfigNS));
    assertBSONObjsSame(expectedChunksIndexes, foundChunksIndexes);

    auto foundShardsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString::kConfigsvrShardsNamespace));
    assertBSONObjsSame(expectedShardsIndexes, foundShardsIndexes);

    auto foundTagsIndexes = assertGet(getIndexes(operationContext(), TagsType::ConfigNS));
    assertBSONObjsSame(expectedTagsIndexes, foundTagsIndexes);


    auto expectedPlacementHistoryIndexes =
        std::vector<BSONObj>{BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                      << "_id_"),
                             BSON("v" << 2 << "unique" << true << "key"
                                      << BSON("nss" << 1 << "timestamp" << -1) << "name"
                                      << "nss_1_timestamp_-1")};
    auto foundlacementHistoryIndexes = assertGet(
        getIndexes(operationContext(), NamespaceString::kConfigsvrPlacementHistoryNamespace));
    assertBSONObjsSame(expectedPlacementHistoryIndexes, foundlacementHistoryIndexes);
}


TEST_F(ConfigInitializationTest, InizializePlacementHistory) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    setupConfigDatabase(operationContext(), 3);

    setupConfigShard(operationContext(), 10);

    // generate coll1 and its chunk:
    // 10 chunks: from shard1 to shard5
    // note: the Range is irrelevant for the test, we only care about the shard
    std::vector<ChunkType> chunks1;
    auto coll1Uuid = UUID::gen();
    for (uint32_t i = 1; i <= 5; i++) {
        int lb = 10 * (i - 1);
        int ub = 10 * i;
        ChunkType c1(coll1Uuid,
                     ChunkRange(BSON("x" << lb << "y" << lb), BSON("x" << ub << "y" << ub)),
                     ChunkVersion({OID::gen(), {i, i}}, {i, i}),
                     ShardId("shard" + std::to_string(i)));

        lb = 10 * (6 + i - 1);
        ub = 10 * (6 + i);

        // add another chunk on the same shard, we want to ensure that we only get one entry per
        // shard per collection
        ChunkType c2(coll1Uuid,
                     ChunkRange(BSON("x" << lb << "y" << lb), BSON("x" << ub << "y" << ub)),
                     ChunkVersion({OID::gen(), {i, i}}, {i + 6, i + 6}),
                     ShardId("shard" + std::to_string(i)));

        chunks1.push_back(c1);
        chunks1.push_back(c2);
    }

    setupCollection(NamespaceString::createNamespaceString_forTest("db1.coll1"),
                    BSON("x" << 1 << "y" << 1),
                    chunks1);

    // generate coll2 and its chunk:
    // 10 chunks: from shard6 to shard10
    std::vector<ChunkType> chunks2;
    auto coll2Uuid = UUID::gen();
    for (uint32_t i = 6; i <= 10; i++) {
        int lb = 10 * (i - 1);
        int ub = 10 * i;
        ChunkType c1(coll2Uuid,
                     ChunkRange(BSON("x" << lb << "y" << lb), BSON("x" << ub << "y" << ub)),
                     ChunkVersion({OID::gen(), {i, i}}, {i + 6, i + 6}),
                     ShardId("shard" + std::to_string(i)));

        lb = 10 * (11 + i - 1);
        ub = 10 * (11 + i);

        // add another chunk on the same shard, we want to ensure that we only get one entry per
        // shard per collection
        ChunkType c2(coll2Uuid,
                     ChunkRange(BSON("x" << lb << "y" << lb), BSON("x" << ub << "y" << ub)),
                     ChunkVersion({OID::gen(), {i, i}}, {i, i}),
                     ShardId("shard" + std::to_string(i)));

        chunks2.push_back(c2);
        chunks2.push_back(c1);
    }

    setupCollection(NamespaceString::createNamespaceString_forTest("db1.coll2"),
                    BSON("x" << 1 << "y" << 1),
                    chunks2);

    // Ensure that the vector clock is able to return an up-to-date config time to both the
    // ShardingCatalogManager and this test.
    ConfigServerOpObserver opObserver;
    auto initTime = VectorClock::get(operationContext())->getTime();
    repl::OpTime majorityCommitPoint(initTime.clusterTime().asTimestamp(), 1);
    opObserver.onMajorityCommitPointUpdate(getServiceContext(), majorityCommitPoint);

    auto timeAtInitialization = VectorClock::get(operationContext())->getTime();
    auto configTimeAtInitialization = timeAtInitialization.configTime().asTimestamp();

    // init placement history
    ShardingCatalogManager::get(operationContext())->initializePlacementHistory(operationContext());

    // check db1
    auto db1Entry = findOneOnConfigCollection<NamespacePlacementType>(
        operationContext(),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON("nss"
             << "db1"));
    assertPlacementType(db1Entry,
                        NamespaceString::createNamespaceString_forTest("db1"),
                        configTimeAtInitialization,
                        {"shard1"});

    // check db2
    auto db2Entry = findOneOnConfigCollection<NamespacePlacementType>(
        operationContext(),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON("nss"
             << "db2"));
    assertPlacementType(db2Entry,
                        NamespaceString::createNamespaceString_forTest("db2"),
                        configTimeAtInitialization,
                        {"shard2"});

    // check db3
    auto db3Entry = findOneOnConfigCollection<NamespacePlacementType>(
        operationContext(),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON("nss"
             << "db3"));
    assertPlacementType(db3Entry,
                        NamespaceString::createNamespaceString_forTest("db3"),
                        configTimeAtInitialization,
                        {"shard3"});

    // check coll1
    auto coll1Entry = findOneOnConfigCollection<NamespacePlacementType>(
        operationContext(),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON("nss"
             << "db1.coll1"));
    assertPlacementType(coll1Entry,
                        NamespaceString::createNamespaceString_forTest("db1.coll1"),
                        configTimeAtInitialization,
                        {"shard1", "shard2", "shard3", "shard4", "shard5"});

    // check coll2
    auto coll2Entry = findOneOnConfigCollection<NamespacePlacementType>(
        operationContext(),
        NamespaceString::kConfigsvrPlacementHistoryNamespace,
        BSON("nss"
             << "db1.coll2"));
    assertPlacementType(coll2Entry,
                        NamespaceString::createNamespaceString_forTest("db1.coll2"),
                        configTimeAtInitialization,
                        {"shard6", "shard7", "shard8", "shard9", "shard10"});

    // Re-run  the command without advancing the config time; this will have the effect of
    // generating  a second snapshot where each namespace is already present within config.placement
    // history under the same (namespace, timestamp) tuple.
    // We expect this command to complete without raising any exception due to uniqueness
    // constraints.
    ShardingCatalogManager::get(operationContext())->initializePlacementHistory(operationContext());
}

}  // unnamed namespace
}  // namespace mongo
