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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_op_observer.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using unittest::assertGet;

class ConfigInitializationTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interrupt();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

    /* Generate and insert an entry into the config.shards collection using the received shard ID
     * and an auto generated value for the host port.
     */
    ShardType createShardMetadata(OperationContext* opCtx, const ShardId& shardId) {
        static uint32_t numInvocations = 0;
        const std::string host("localhost:" + std::to_string(30000 + numInvocations++));
        ShardType shard(shardId.toString(), host);
        shard.setState(ShardType::ShardState::kShardAware);
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrShardsNamespace, shard.toBSON()));
        return shard;
    }


    std::pair<CollectionType, std::vector<ChunkType>> createCollectionAndChunksMetadata(
        OperationContext* opCtx,
        const NamespaceString& collName,
        int32_t numChunks,
        std::vector<ShardId> desiredShards,
        bool setOnCurrentShardSice = true) {
        const auto collUUID = UUID::gen();
        const auto collEpoch = OID::gen();
        const auto collCreationTime = Date_t::now();
        const auto collTimestamp = Timestamp(collCreationTime);
        const auto shardKeyPattern = KeyPattern(BSON("x" << 1));

        ChunkVersion chunkVersion({collEpoch, collTimestamp}, {1, 1});

        std::vector<ChunkType> collChunks;
        std::shuffle(desiredShards.begin(), desiredShards.end(), std::random_device());
        for (int32_t i = 1; i <= numChunks; ++i) {
            const auto minKey = i == 1 ? shardKeyPattern.globalMin() : BSON("x" << i * 10);
            const auto maxKey =
                i == numChunks ? shardKeyPattern.globalMax() : BSON("x" << (i + 1) * 10);
            const auto shardId = desiredShards[i % desiredShards.size()];
            chunkVersion.incMinor();
            ChunkType chunk(collUUID, ChunkRange(minKey, maxKey), chunkVersion, shardId);
            if (setOnCurrentShardSice) {
                Timestamp onShardSince(collCreationTime + Milliseconds(i));
                chunk.setHistory({ChunkHistory(onShardSince, shardId)});
                chunk.setOnCurrentShardSince(onShardSince);
            }

            collChunks.push_back(std::move(chunk));
        }

        auto coll = setupCollection(collName, shardKeyPattern, collChunks);

        return std::make_pair(std::move(coll), std::move(collChunks));
    }

    void assertSamePlacementInfo(const NamespacePlacementType& expected,
                                 const NamespacePlacementType& found) {

        ASSERT_EQ(expected.getNss(), found.getNss());
        ASSERT_EQ(expected.getTimestamp(), found.getTimestamp());
        ASSERT_EQ(expected.getUuid(), found.getUuid());

        auto expectedShards = expected.getShards();
        auto foundShards = found.getShards();
        std::sort(expectedShards.begin(), expectedShards.end());
        std::sort(foundShards.begin(), foundShards.end());
        ASSERT_EQ(expectedShards, foundShards);
    }

private:
    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

TEST_F(ConfigInitializationTest, InitClusterMultipleVersionDocs) {
    VersionType version;
    version.setClusterId(OID::gen());
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, version.toBSON()));

    ASSERT_OK(insertToConfigCollection(operationContext(),
                                       NamespaceString::kConfigVersionNamespace,
                                       BSON("_id" << "a second document")));

    ASSERT_EQ(ErrorCodes::TooManyMatchingDocuments,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, InitInvalidConfigVersionDoc) {
    BSONObj versionDoc(fromjson(R"({
                    _id: 1,
                    clusterId: "should be an ID"
                })"));
    ASSERT_OK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, versionDoc));

    ASSERT_EQ(ErrorCodes::TypeMismatch,
              ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));
}


TEST_F(ConfigInitializationTest, InitNoVersionDocEmptyConfig) {
    // Make sure there is no existing document
    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument,
                  findOneOnConfigCollection(
                      operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    VersionType foundVersion = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());
}

TEST_F(ConfigInitializationTest, OnlyRunsOnce) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    VersionType foundVersion = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

    ASSERT_TRUE(foundVersion.getClusterId().isSet());

    ASSERT_EQUALS(ErrorCodes::AlreadyInitialized,
                  ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));
}

TEST_F(ConfigInitializationTest, ReRunsIfDocRolledBackThenReElected) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto versionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    VersionType foundVersion = VersionType::parse(versionDoc, IDLParserContext("VersionType"));

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
        auto nss = NamespaceString::kConfigVersionNamespace;
        writeConflictRetry(opCtx, "removeConfigDocuments", nss, [&] {
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
                  findOneOnConfigCollection(
                      operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    // Throw out cached information.
    ShardingCatalogManager::get(operationContext())
        ->discardCachedConfigDatabaseInitializationState();

    // Re-create the config.version document.
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    auto newVersionDoc = assertGet(findOneOnConfigCollection(
        operationContext(), NamespaceString::kConfigVersionNamespace, BSONObj()));

    VersionType newFoundVersion =
        VersionType::parse(newVersionDoc, IDLParserContext("VersionType"));

    ASSERT_TRUE(newFoundVersion.getClusterId().isSet());
    ASSERT_NOT_EQUALS(newFoundVersion.getClusterId(), foundVersion.getClusterId());
}

TEST_F(ConfigInitializationTest, BuildsNecessaryIndexes) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    std::vector<BSONObj> expectedChunksIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
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
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "unique" << true << "key" << BSON("host" << 1) << "name"
                 << "host_1")};
    auto expectedTagsIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "unique" << true << "key" << BSON("ns" << 1 << "min" << 1) << "name"
                 << "ns_1_min_1"),
        BSON("v" << 2 << "key" << BSON("ns" << 1 << "tag" << 1) << "name"
                 << "ns_1_tag_1")};

    auto foundChunksIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString::kConfigsvrChunksNamespace));
    assertBSONObjsSame(expectedChunksIndexes, foundChunksIndexes);

    auto foundShardsIndexes =
        assertGet(getIndexes(operationContext(), NamespaceString::kConfigsvrShardsNamespace));
    assertBSONObjsSame(expectedShardsIndexes, foundShardsIndexes);

    auto foundTagsIndexes = assertGet(getIndexes(operationContext(), TagsType::ConfigNS));
    assertBSONObjsSame(expectedTagsIndexes, foundTagsIndexes);


    auto expectedPlacementHistoryIndexes = std::vector<BSONObj>{
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "unique" << true << "key" << BSON("nss" << 1 << "timestamp" << -1)
                 << "name"
                 << "nss_1_timestamp_-1")};
    auto foundlacementHistoryIndexes = assertGet(
        getIndexes(operationContext(), NamespaceString::kConfigsvrPlacementHistoryNamespace));
    assertBSONObjsSame(expectedPlacementHistoryIndexes, foundlacementHistoryIndexes);
}

TEST_F(ConfigInitializationTest, InizializePlacementHistory) {
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->initializeConfigDatabaseIfNeeded(operationContext()));

    // Test setup
    // - Four shards
    // - Three databases (one with no sharded collections)
    // - Three sharded collections (one with corrupted placement data)
    const std::vector<ShardId> allShardIds = {
        ShardId("shard1"), ShardId("shard2"), ShardId("shard3"), ShardId("shard4")};
    for (const auto& id : allShardIds) {
        createShardMetadata(operationContext(), id);
    }

    // (dbname, primaryShard, timestamp field of DatabaseVersion)
    const std::vector<std::tuple<DatabaseName, ShardId, Timestamp>> databaseInfos{
        std::make_tuple(
            DatabaseName::createDatabaseName_forTest(boost::none, "dbWithoutCollections"),
            ShardId("shard1"),
            Timestamp(1, 1)),
        std::make_tuple(
            DatabaseName::createDatabaseName_forTest(boost::none, "dbWithCollections_1_2"),
            ShardId("shard2"),
            Timestamp(2, 1)),
        std::make_tuple(
            DatabaseName::createDatabaseName_forTest(boost::none, "dbWithCorruptedCollection"),
            ShardId("shard3"),
            Timestamp(3, 1))};

    for (const auto& [dbName, primaryShard, timestamp] : databaseInfos) {
        setupDatabase(dbName, ShardId(primaryShard), DatabaseVersion(UUID::gen(), timestamp));
    }

    NamespaceString coll1Name =
        NamespaceString::createNamespaceString_forTest("dbWithCollections_1_2", "coll1");
    std::vector<ShardId> expectedColl1Placement{ShardId("shard1"), ShardId("shard4")};
    const auto [coll1, coll1Chunks] =
        createCollectionAndChunksMetadata(operationContext(), coll1Name, 2, expectedColl1Placement);

    NamespaceString coll2Name =
        NamespaceString::createNamespaceString_forTest("dbWithCollections_1_2", "coll2");
    std::vector<ShardId> expectedColl2Placement{
        ShardId("shard1"), ShardId("shard2"), ShardId("shard3"), ShardId("shard4")};
    const auto [coll2, coll2Chunks] =
        createCollectionAndChunksMetadata(operationContext(), coll2Name, 8, expectedColl2Placement);

    NamespaceString corruptedCollName = NamespaceString::createNamespaceString_forTest(
        "dbWithCorruptedCollection", "corruptedColl");
    std::vector<ShardId> expectedCorruptedCollPlacement{
        ShardId("shard1"), ShardId("shard2"), ShardId("shard3")};
    const auto [corruptedColl, corruptedCollChunks] =
        createCollectionAndChunksMetadata(operationContext(),
                                          corruptedCollName,
                                          8,
                                          expectedCorruptedCollPlacement,
                                          false /* setOnCurrentShardSince*/);

    // Ensure that the vector clock is able to return an up-to-date config time to both the
    // ShardingCatalogManager and this test.
    ConfigServerOpObserver opObserver;
    auto now = VectorClock::get(operationContext())->getTime();
    repl::OpTime majorityCommitPoint(now.clusterTime().asTimestamp(), 1);
    opObserver.onMajorityCommitPointUpdate(getServiceContext(), majorityCommitPoint);

    now = VectorClock::get(operationContext())->getTime();
    const auto timeAtFirstInvocation = now.configTime().asTimestamp();

    // init placement history
    ShardingCatalogManager::get(operationContext())->initializePlacementHistory(operationContext());

    auto verifyOutcome = [&,
                          coll1 = coll1,
                          coll1Chunks = coll1Chunks,
                          coll2 = coll2,
                          coll2Chunks = coll2Chunks,
                          corruptedColl = corruptedColl](const Timestamp& timeAtInitialization) {
        DBDirectClient dbClient(operationContext());

        // The expected amount of documents has been generated
        ASSERT_EQUALS(
            dbClient.count(NamespaceString::kConfigsvrPlacementHistoryNamespace, BSONObj()),
            3 /*numDatabases*/ + 3 /*numCollections*/ + 2 /*numMarkers*/);

        // Each database is correctly described
        for (const auto& [dbName, primaryShard, timeOfCreation] : databaseInfos) {
            const NamespacePlacementType expectedEntry(
                NamespaceString::createNamespaceString_forTest(dbName),
                timeOfCreation,
                {primaryShard});
            const auto generatedEntry = findOneOnConfigCollection<NamespacePlacementType>(
                operationContext(),
                NamespaceString::kConfigsvrPlacementHistoryNamespace,
                BSON("nss" << dbName.toString_forTest()));

            assertSamePlacementInfo(expectedEntry, generatedEntry);
        }

        // Each collection is properly described:
        const auto getExpectedTimestampForColl = [](const std::vector<ChunkType>& collChunks) {
            return std::max_element(collChunks.begin(),
                                    collChunks.end(),
                                    [](const ChunkType& lhs, const ChunkType& rhs) {
                                        return *lhs.getOnCurrentShardSince() <
                                            *rhs.getOnCurrentShardSince();
                                    })
                ->getOnCurrentShardSince()
                .value();
        };

        // - coll1
        NamespacePlacementType expectedEntryForColl1(
            coll1.getNss(), getExpectedTimestampForColl(coll1Chunks), expectedColl1Placement);
        expectedEntryForColl1.setUuid(coll1.getUuid());
        const auto generatedEntryForColl1 = findOneOnConfigCollection<NamespacePlacementType>(
            operationContext(),
            NamespaceString::kConfigsvrPlacementHistoryNamespace,
            BSON("nss" << coll1.getNss().ns_forTest()));

        assertSamePlacementInfo(expectedEntryForColl1, generatedEntryForColl1);

        // - coll2
        NamespacePlacementType expectedEntryForColl2(
            coll2.getNss(), getExpectedTimestampForColl(coll2Chunks), expectedColl2Placement);
        expectedEntryForColl2.setUuid(coll2.getUuid());
        const auto generatedEntryForColl2 = findOneOnConfigCollection<NamespacePlacementType>(
            operationContext(),
            NamespaceString::kConfigsvrPlacementHistoryNamespace,
            BSON("nss" << coll2.getNss().ns_forTest()));

        assertSamePlacementInfo(expectedEntryForColl2, generatedEntryForColl2);

        // - corruptedColl
        NamespacePlacementType expectedEntryForCorruptedColl(
            corruptedColl.getNss(), timeAtInitialization, expectedCorruptedCollPlacement);
        expectedEntryForCorruptedColl.setUuid(corruptedColl.getUuid());
        const auto generatedEntryForCorruptedColl =
            findOneOnConfigCollection<NamespacePlacementType>(
                operationContext(),
                NamespaceString::kConfigsvrPlacementHistoryNamespace,
                BSON("nss" << corruptedColl.getNss().ns_forTest()));

        assertSamePlacementInfo(expectedEntryForCorruptedColl, generatedEntryForCorruptedColl);

        // Check placement initialization markers:
        // - one entry at begin-of-time with all the currently existing shards (and no UUID set).
        const NamespacePlacementType expectedMarkerForDawnOfTime(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
            Timestamp(0, 1),
            allShardIds);
        const auto generatedMarkerForDawnOfTime = findOneOnConfigCollection<NamespacePlacementType>(
            operationContext(),
            NamespaceString::kConfigsvrPlacementHistoryNamespace,
            BSON("nss"
                 << ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.ns_forTest()
                 << "timestamp" << Timestamp(0, 1)));

        assertSamePlacementInfo(expectedMarkerForDawnOfTime, generatedMarkerForDawnOfTime);

        // - one entry at the time the initialization is performed with an empty set of shards
        // (and no UUID set).
        const NamespacePlacementType expectedMarkerForInitializationTime(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
            timeAtInitialization,
            {});
        const auto generatedMarkerForInitializationTime =
            findOneOnConfigCollection<NamespacePlacementType>(
                operationContext(),
                NamespaceString::kConfigsvrPlacementHistoryNamespace,
                BSON("nss" << ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker
                                  .ns_forTest()
                           << "timestamp" << timeAtInitialization));

        assertSamePlacementInfo(expectedMarkerForInitializationTime,
                                generatedMarkerForInitializationTime);
    };

    verifyOutcome(timeAtFirstInvocation);

    // Perform a second invocation - the content created by the previous invocation should have been
    // fully replaced by a new full representation with updated initialization markers

    now = VectorClock::get(operationContext())->getTime();
    majorityCommitPoint = repl::OpTime(now.clusterTime().asTimestamp(), 1);
    opObserver.onMajorityCommitPointUpdate(getServiceContext(), majorityCommitPoint);

    now = VectorClock::get(operationContext())->getTime();
    const auto timeAtSecondInvocation = now.configTime().asTimestamp();
    ASSERT_GT(timeAtSecondInvocation, timeAtFirstInvocation);

    ShardingCatalogManager::get(operationContext())->initializePlacementHistory(operationContext());

    verifyOutcome(timeAtSecondInvocation);
}

}  // unnamed namespace
}  // namespace mongo
