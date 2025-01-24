/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <string>

#include <absl/container/node_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

void createTestCollection(OperationContext* opCtx, const NamespaceString& nss) {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        opCtx);
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
}

void createTestView(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const NamespaceString& viewOn,
                    const std::vector<BSONObj>& pipeline) {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        opCtx);
    uassertStatusOK(createCollection(
        opCtx,
        nss.dbName(),
        BSON("create" << nss.coll() << "viewOn" << viewOn.coll() << "pipeline" << pipeline)));
}

UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    const auto optUuid = CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
    ASSERT(optUuid);
    return *optUuid;
}

class MultipleCollectionAccessorTest : public ShardServerTestFixture {
protected:
    void setUp() override;

    void installDatabaseMetadata(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const DatabaseVersion& dbVersion);
    void installUnshardedCollectionMetadata(OperationContext* opCtx, const NamespaceString& nss);
    void installShardedCollectionMetadata(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const DatabaseVersion& dbVersion,
                                          std::vector<ChunkType> chunks);

    const DatabaseName dbNameTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};


    const NamespaceString mainNss =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "main");

    const NamespaceString secondaryNss1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondary1");
    const NamespaceString secondaryNss2 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondary2");

    const NamespaceString secondaryView1 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondaryView1");
    const NamespaceString secondaryView2 =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "secondaryView2");

    const ShardVersion shardVersion = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(5, 0)}, CollectionPlacement(10, 1)),
        boost::optional<CollectionIndexes>(boost::none));
};

void MultipleCollectionAccessorTest::setUp() {
    ShardServerTestFixture::setUp();
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::RouterServer};

    // Setup test collections and metadata
    installDatabaseMetadata(operationContext(), dbNameTestDb, dbVersionTestDb);

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
    createTestView(operationContext(), secondaryView1, secondaryNss1, {});
    createTestView(operationContext(), secondaryView2, secondaryNss2, {});
}

void MultipleCollectionAccessorTest::installDatabaseMetadata(OperationContext* opCtx,
                                                             const DatabaseName& dbName,
                                                             const DatabaseVersion& dbVersion) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X, {}, {});
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->setDbInfo(opCtx, {dbName, kMyShardName, dbVersion});
}


void MultipleCollectionAccessorTest::installShardedCollectionMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const DatabaseVersion& dbVersion,
    std::vector<ChunkType> chunks) {
    ASSERT(!chunks.empty());

    const auto uuid = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        return autoColl.getCollection()->uuid();
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

    const auto collectionMetadata = CollectionMetadata(
        ChunkManager(kMyShardName, dbVersion, rtHandle, boost::none), kMyShardName);

    AutoGetCollection coll(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
        ->setFilteringMetadata(opCtx, collectionMetadata);
}

TEST_F(MultipleCollectionAccessorTest, mainCollectionViaAutoGetter) {
    AutoGetCollection coll(operationContext(), mainNss, MODE_IX);

    auto accessor = MultipleCollectionAccessor(coll.getCollection());
    ASSERT_FALSE(accessor.isAcquisition());
    ASSERT_EQ(coll.getCollection(), accessor.getMainCollection());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isCollectionPtr());
}

TEST_F(MultipleCollectionAccessorTest, mainCollectionViaAcquisition) {
    const auto acquisition =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), mainNss, AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    auto accessor = MultipleCollectionAccessor(acquisition);
    ASSERT_TRUE(accessor.isAcquisition());
    ASSERT_EQ(acquisition.getCollectionPtr(), accessor.getMainCollection());
    ASSERT_EQ(acquisition.uuid(), accessor.getMainAcquisition().uuid());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isAcquisition());
}

TEST_F(MultipleCollectionAccessorTest, secondaryCollectionsViaAutoGetter) {
    AutoGetCollection coll(operationContext(), mainNss, MODE_IX);

    const std::vector<NamespaceStringOrUUID> secondaryExecNssList = {secondaryNss1, secondaryNss2};
    auto accessor = MultipleCollectionAccessor(
        operationContext(), &coll.getCollection(), mainNss, false, secondaryExecNssList);
    // Check the main collection is correctly returned.
    ASSERT_FALSE(accessor.isAcquisition());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isCollectionPtr());

    // Check the secondary collections are correctly returned.
    auto secondaryCollectionMap = accessor.getSecondaryCollections();
    ASSERT_EQ(2, secondaryCollectionMap.size());
    ASSERT_EQ(secondaryNss1, secondaryCollectionMap[secondaryNss1]->ns());
    ASSERT_EQ(secondaryNss2, secondaryCollectionMap[secondaryNss2]->ns());

    // Check the lookup returns the correct acquisition by namespace.
    ASSERT_EQ(secondaryNss1, accessor.lookupCollection(secondaryNss1)->ns());
    ASSERT_EQ(secondaryNss2, accessor.lookupCollection(secondaryNss2)->ns());
}

TEST_F(MultipleCollectionAccessorTest, secondaryViewsViaAutoGetter) {
    AutoGetCollection coll(operationContext(), mainNss, MODE_IX);

    const std::vector<NamespaceStringOrUUID> secondaryExecNssList = {secondaryView1,
                                                                     secondaryView2};
    auto accessor = MultipleCollectionAccessor(
        operationContext(), &coll.getCollection(), mainNss, false, secondaryExecNssList);

    // Check the main collection is correctly returned.
    ASSERT_FALSE(accessor.isAcquisition());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isCollectionPtr());

    // Views return a null CollectionPtr.
    auto secondaryCollectionMap = accessor.getSecondaryCollections();
    ASSERT_EQ(2, secondaryCollectionMap.size());
    ASSERT_FALSE(secondaryCollectionMap[secondaryView1]);
    ASSERT_FALSE(secondaryCollectionMap[secondaryView2]);

    // Views return a null CollectionPtr.
    ASSERT_FALSE(accessor.lookupCollection(secondaryView1));
    ASSERT_FALSE(accessor.lookupCollection(secondaryView2));
}

TEST_F(MultipleCollectionAccessorTest, secondaryCollectionsViaAcquisition) {
    const auto acquisitionMain =
        acquireCollection(operationContext(),
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

    auto accessor =
        MultipleCollectionAccessor(acquisitionMain, {acquisitionSecondary1, acquisitionSecondary2});
    // Check the main collection is correctly returned.
    ASSERT_TRUE(accessor.isAcquisition());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isAcquisition());
    ASSERT_EQ(acquisitionMain.uuid(), accessor.getMainAcquisition().uuid());

    // Check the secondary collections are correctly returned.
    auto secondaryCollectionMap = accessor.getSecondaryCollections();
    ASSERT_EQ(2, secondaryCollectionMap.size());
    ASSERT_EQ(acquisitionSecondary1.getCollectionPtr(), secondaryCollectionMap[secondaryNss1]);
    ASSERT_EQ(acquisitionSecondary2.getCollectionPtr(), secondaryCollectionMap[secondaryNss2]);

    // Check the lookup returns the correct acquisition by namespace.
    ASSERT_EQ(acquisitionSecondary1.getCollectionPtr(), accessor.lookupCollection(secondaryNss1));
    ASSERT_EQ(acquisitionSecondary2.getCollectionPtr(), accessor.lookupCollection(secondaryNss2));
}

TEST_F(MultipleCollectionAccessorTest, secondaryViewsViaAcquisition) {
    const auto acquisitionMain =
        acquireCollection(operationContext(),
                          CollectionAcquisitionRequest::fromOpCtx(
                              operationContext(), mainNss, AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    const auto acquisitionSecondary1 =
        acquireCollectionOrView(operationContext(),
                                {
                                    secondaryView1,
                                    {},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kCanBeView,
                                },
                                MODE_IX);

    const auto acquisitionSecondary2 =
        acquireCollectionOrView(operationContext(),
                                {
                                    secondaryView2,
                                    {},
                                    repl::ReadConcernArgs(),
                                    AcquisitionPrerequisites::kWrite,
                                    AcquisitionPrerequisites::kCanBeView,
                                },
                                MODE_IX);

    auto accessor =
        MultipleCollectionAccessor(acquisitionMain, {acquisitionSecondary1, acquisitionSecondary2});

    // Check the main collection is correctly returned.
    ASSERT_TRUE(accessor.isAcquisition());
    ASSERT_TRUE(accessor.getMainCollectionPtrOrAcquisition().isAcquisition());

    // Views return a null CollectionPtr.
    auto secondaryCollectionMap = accessor.getSecondaryCollections();
    ASSERT_EQ(2, secondaryCollectionMap.size());
    ASSERT_FALSE(secondaryCollectionMap[secondaryView1]);
    ASSERT_FALSE(secondaryCollectionMap[secondaryView2]);

    // Views return a null CollectionPtr.
    ASSERT_FALSE(accessor.lookupCollection(secondaryView1));
    ASSERT_FALSE(accessor.lookupCollection(secondaryView2));
}

}  // namespace
}  // namespace mongo
