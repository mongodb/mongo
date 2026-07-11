// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/multiple_collection_accessor.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/none.hpp>
#include <fmt/format.h>


namespace mongo {
namespace {

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto optUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    ASSERT(optUuid);
    return *optUuid;
}

class MultipleCollectionAccessorTest : public ShardServerTestFixture {
protected:
    void setUp() override;

    void installUnshardedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss);
    void installShardedCollectionMetadata(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const DatabaseVersion& dbVersion,
                                          std::vector<ChunkType> chunks);

    const DatabaseName dbNameTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};


    const NamespaceString mainNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "main");

    const NamespaceString mainView =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "mainView");

    const NamespaceString secondaryNss1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondary1");
    const NamespaceString secondaryNss2 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondary2");

    const NamespaceString secondaryView1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondaryView1");
    const NamespaceString secondaryView2 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondaryView2");

    const ShardVersion shardVersion = ShardVersionFactory::make(ChunkVersion(
        CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)));
};

void MultipleCollectionAccessorTest::setUp() {
    ShardServerTestFixture::setUp();
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    // Create all the required collections
    for (const auto& nss : {mainNss, secondaryNss1, secondaryNss2}) {
        createTestCollection(operationContext(), nss);
        const auto uuidShardedCollection1 = getCollectionUUID(operationContext(), nss);
        installShardedCollectionMetadata(
            operationContext(),
            nss,
            dbVersionTestDb,
            {ChunkType(uuidShardedCollection1,
                       ChunkRange{BSON("skey" << MINKEY), BSON("skey" << MAXKEY)},
                       shardVersion.placementVersion(),
                       kMyShardName)});
    }

    // Create all the required views
    createTestView(operationContext(), mainView, mainNss, {});
    createTestView(operationContext(), secondaryView1, secondaryNss1, {});
    createTestView(operationContext(), secondaryView2, secondaryNss2, {});
}

void MultipleCollectionAccessorTest::installShardedCollectionMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const DatabaseVersion& dbVersion,
    std::vector<ChunkType> chunks) {
    ASSERT(!chunks.empty());

    const auto uuid = [&] {
        auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
        return coll.uuid();
    }();

    const std::string shardKey("skey");
    const ShardKeyPattern shardKeyPattern{BSON(shardKey << 1)};
    const auto epoch = chunks.front().getVersion().epoch();
    const auto timestamp = chunks.front().getVersion().getTimestamp();

    auto rt = RoutingTableHistory::makeNew(nss,
                                           uuid,
                                           shardKeyPattern.getKeyPattern(),
                                           false, /* unsplittable */
                                           nullptr,
                                           false,
                                           epoch,
                                           timestamp,
                                           boost::none /* timeseriesFields */,
                                           boost::none /* resharding Fields */,
                                           true /* allowMigrations */,
                                           chunks);

    const auto version = rt.getVersion();
    const auto rtHandle =
        RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(std::move(rt)),
                                       ComparableChunkVersion::makeComparableChunkVersion(version));

    const auto collectionMetadata = CollectionMetadata(CurrentChunkManager(rtHandle), kMyShardName);

    CollectionShardingRuntime::acquireExclusive(opCtx, nss)
        ->setCollectionMetadata(opCtx, collectionMetadata);
}

TEST_F(MultipleCollectionAccessorTest, mainCollectionViaAcquisition) {
    const auto acquisition =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), mainNss, AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    auto accessor = MultipleCollectionAccessor(acquisition);
    ASSERT_EQ(acquisition.getCollectionPtr(), accessor.getMainCollection());
    ASSERT_EQ(acquisition.uuid(), accessor.getMainCollectionAcquisition().uuid());
    ASSERT_FALSE(accessor.hasNonExistentMainCollection());
}

TEST_F(MultipleCollectionAccessorTest, mainViewViaAcquisition) {
    const auto acquisition =
        acquireCollectionOrView(operationContext(),
                                CollectionOrViewAcquisitionRequest::fromOpCtx(
                                    operationContext(),
                                    NamespaceStringOrUUID(mainView),
                                    AcquisitionPrerequisites::OperationType::kWrite,
                                    AcquisitionPrerequisites::ViewMode::kCanBeView),
                                MODE_IX);

    auto accessor = MultipleCollectionAccessor(acquisition);
    ASSERT_FALSE(accessor.hasMainCollection());
    ASSERT_FALSE(accessor.hasNonExistentMainCollection());
}

TEST_F(MultipleCollectionAccessorTest, secondaryCollectionsViaAcquisition) {
    const auto acquisitionMain =
        acquireCollectionOrView(operationContext(),
                                CollectionAcquisitionRequest::fromOpCtx(
                                    operationContext(), mainNss, AcquisitionPrerequisites::kWrite),
                                MODE_IX);

    const auto acquisitionSecondary1 = acquireCollectionOrView(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), secondaryNss1, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    const auto acquisitionSecondary2 = acquireCollectionOrView(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), secondaryNss2, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    auto accessor = MultipleCollectionAccessor(
        acquisitionMain, makeAcquisitionMap({acquisitionSecondary1, acquisitionSecondary2}), false);
    // Check the main collection is correctly returned.
    ASSERT_EQ(acquisitionMain.getCollection().uuid(),
              accessor.getMainCollectionAcquisition().uuid());

    // Check the secondary collections are correctly returned.
    const auto& secondaryAcqMap = accessor.getSecondaryCollectionAcquisitions();
    ASSERT_EQ(2, secondaryAcqMap.size());
    ASSERT_EQ(acquisitionSecondary1.getCollectionPtr(),
              secondaryAcqMap.at(secondaryNss1).getCollectionPtr());
    ASSERT_EQ(acquisitionSecondary2.getCollectionPtr(),
              secondaryAcqMap.at(secondaryNss2).getCollectionPtr());

    // Check the lookup returns the correct acquisition by namespace.
    ASSERT_EQ(acquisitionSecondary1.getCollectionPtr(), accessor.lookupCollection(secondaryNss1));
    ASSERT_EQ(acquisitionSecondary2.getCollectionPtr(), accessor.lookupCollection(secondaryNss2));
}

TEST_F(MultipleCollectionAccessorTest, secondaryViewsViaAcquisition) {
    const auto acquisitionMain =
        acquireCollectionOrView(operationContext(),
                                CollectionAcquisitionRequest::fromOpCtx(
                                    operationContext(), mainNss, AcquisitionPrerequisites::kWrite),
                                MODE_IX);

    const auto acquisitionSecondary1 =
        acquireCollectionOrView(operationContext(),
                                {
                                    secondaryView1,
                                    PlacementConcern::kPretendUnsharded,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kCanBeView,
                                },
                                MODE_IX);

    const auto acquisitionSecondary2 =
        acquireCollectionOrView(operationContext(),
                                {
                                    secondaryView2,
                                    PlacementConcern::kPretendUnsharded,
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kCanBeView,
                                },
                                MODE_IX);

    auto accessor = MultipleCollectionAccessor(
        acquisitionMain, makeAcquisitionMap({acquisitionSecondary1, acquisitionSecondary2}), false);

    // Views return a null CollectionPtr.
    const auto& secondaryAcqMap = accessor.getSecondaryCollectionAcquisitions();
    ASSERT_EQ(2, secondaryAcqMap.size());
    ASSERT_FALSE(secondaryAcqMap.at(secondaryView1).getCollectionPtr());
    ASSERT_FALSE(secondaryAcqMap.at(secondaryView2).getCollectionPtr());

    // Views return a null CollectionPtr.
    ASSERT_FALSE(accessor.lookupCollection(secondaryView1));
    ASSERT_FALSE(accessor.lookupCollection(secondaryView2));
}

TEST_F(MultipleCollectionAccessorTest, nonExistentCollection) {
    // Create a namespace for a collection that doesn't exist in the catalog.
    const NamespaceString nonExistentNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "nonExistent");

    // Acquire the non-existent collection (this should succeed but the collection won't exist).
    const auto acquisition = acquireCollectionOrView(
        operationContext(),
        CollectionAcquisitionRequest::fromOpCtx(
            operationContext(), nonExistentNss, AcquisitionPrerequisites::kRead),
        MODE_IS);

    auto accessor = MultipleCollectionAccessor(acquisition);

    ASSERT_TRUE(accessor.hasNonExistentMainCollection());
    ASSERT_FALSE(accessor.hasMainCollection());
    // getMainCollection() should return a null CollectionPtr.
    ASSERT_FALSE(accessor.getMainCollection());
}

}  // namespace
}  // namespace mongo
