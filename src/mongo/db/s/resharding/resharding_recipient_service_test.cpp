/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/stale_exception.h"

namespace mongo {
namespace {

class ReshardingRecipientServiceTest : public ServiceContextMongoDTest,
                                       public CatalogCacheTestFixture {
public:
    const UUID kOrigUUID = UUID::gen();
    const NamespaceString kOrigNss = NamespaceString("db.foo");
    const ShardKeyPattern kReshardingKey = ShardKeyPattern(BSON("newKey" << 1));
    const OID kReshardingEpoch = OID::gen();
    const UUID kReshardingUUID = UUID::gen();
    const NamespaceString kReshardingNss = NamespaceString(
        str::stream() << "db." << NamespaceString::kTemporaryReshardingCollectionPrefix
                      << kOrigUUID);
    const Timestamp kDefaultFetchTimestamp = Timestamp(200, 1);

    void setUp() override {
        CatalogCacheTestFixture::setUp();

        repl::ReplicationCoordinator::set(
            getServiceContext(),
            std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
        ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_PRIMARY));

        auto _storageInterfaceImpl = std::make_unique<repl::StorageInterfaceImpl>();
        repl::StorageInterface::set(getServiceContext(), std::move(_storageInterfaceImpl));

        repl::setOplogCollectionName(getServiceContext());
        repl::createOplog(operationContext());
        MongoDSessionCatalog::onStepUp(operationContext());
    }

    void tearDown() override {
        CatalogCacheTestFixture::tearDown();
    }

    void expectListCollections(const NamespaceString& nss,
                               UUID uuid,
                               const std::vector<BSONObj>& collectionsDocs,
                               const HostAndPort& expectedHost) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listCollections"_sd);
            ASSERT_EQUALS(nss.db(), request.dbname);
            ASSERT_EQUALS(expectedHost, request.target);
            ASSERT_BSONOBJ_EQ(request.cmdObj["filter"].Obj(), BSON("info.uuid" << uuid));
            ASSERT(request.cmdObj.hasField("databaseVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["readConcern"].Obj(),
                              BSON("level"
                                   << "local"
                                   << "afterClusterTime" << kDefaultFetchTimestamp));

            std::string listCollectionsNs = str::stream() << nss.db() << "$cmd.listCollections";
            return BSON("ok" << 1 << "cursor"
                             << BSON("id" << 0LL << "ns" << listCollectionsNs << "firstBatch"
                                          << collectionsDocs));
        });
    }

    void expectListIndexes(const NamespaceString& nss,
                           UUID uuid,
                           const std::vector<BSONObj>& indexDocs,
                           const HostAndPort& expectedHost) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listIndexes"_sd);
            ASSERT_EQUALS(nss.db(), request.dbname);
            ASSERT_EQUALS(expectedHost, request.target);
            ASSERT_EQ(unittest::assertGet(UUID::parse(request.cmdObj.firstElement())), uuid);
            ASSERT(request.cmdObj.hasField("shardVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["readConcern"].Obj(),
                              BSON("level"
                                   << "local"
                                   << "afterClusterTime" << kDefaultFetchTimestamp));

            return BSON("ok" << 1 << "cursor"
                             << BSON("id" << 0LL << "ns" << nss.ns() << "firstBatch" << indexDocs));
        });
    }

    // Loads the metadata for the temporary resharding collection into the catalog cache by mocking
    // network responses. The collection contains a single chunk from minKey to maxKey for the given
    // shard key.
    void loadOneChunkMetadataForTemporaryReshardingColl(const NamespaceString& tempNss,
                                                        const NamespaceString& origNss,
                                                        const ShardKeyPattern& skey,
                                                        UUID uuid,
                                                        OID epoch) {
        auto future = scheduleRoutingInfoForcedRefresh(tempNss);

        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            CollectionType coll(tempNss, epoch, Date_t::now(), uuid);
            coll.setKeyPattern(skey.getKeyPattern());
            coll.setUnique(false);

            TypeCollectionReshardingFields reshardingFields;
            reshardingFields.setUuid(uuid);
            TypeCollectionRecipientFields recipientFields;
            recipientFields.setOriginalNamespace(origNss);
            recipientFields.setExistingUUID(uuid);
            // Populating the set of donor shard ids isn't necessary to test the functionality of
            // creating the temporary resharding collection.
            recipientFields.setDonorShardIds({});

            reshardingFields.setRecipientFields(recipientFields);
            coll.setReshardingFields(reshardingFields);

            return std::vector<BSONObj>{coll.toBSON()};
        }());
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            ChunkVersion version(1, 0, epoch);

            ChunkType chunk(tempNss,
                            {skey.getKeyPattern().globalMin(), skey.getKeyPattern().globalMax()},
                            version,
                            {"0"});
            chunk.setName(OID::gen());
            version.incMinor();

            return std::vector<BSONObj>{chunk.toConfigBSON()};
        }());

        future.default_timed_get();
    }

    void expectStaleDbVersionError(const NamespaceString& nss, StringData expectedCmdName) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldNameStringData(), expectedCmdName);
            return createErrorCursorResponse(Status(
                StaleDbRoutingVersion(nss.db().toString(), databaseVersion::makeNew(), boost::none),
                "dummy stale db version error"));
        });
    }

    void expectStaleEpochError(const NamespaceString& nss, StringData expectedCmdName) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldNameStringData(), expectedCmdName);
            return createErrorCursorResponse(
                Status(ErrorCodes::StaleEpoch, "dummy stale epoch error"));
        });
    }

    void verifyCollectionAndIndexes(const NamespaceString& nss,
                                    UUID uuid,
                                    const std::vector<BSONObj>& indexes) {
        DBDirectClient client(operationContext());

        auto collInfos = client.getCollectionInfos(nss.db().toString());
        ASSERT_EQ(collInfos.size(), 1);
        ASSERT_EQ(collInfos.front()["name"].str(), nss.coll());
        ASSERT_EQ(unittest::assertGet(UUID::parse(collInfos.front()["info"]["uuid"])), uuid);

        auto indexSpecs = client.getIndexSpecs(nss, false, 0);
        ASSERT_EQ(indexSpecs.size(), indexes.size());

        UnorderedFieldsBSONObjComparator comparator;
        std::vector<BSONObj> indexesCopy(indexes);
        for (const auto& indexSpec : indexSpecs) {
            for (auto it = indexesCopy.begin(); it != indexesCopy.end(); it++) {
                if (comparator.evaluate(indexSpec == *it)) {
                    indexesCopy.erase(it);
                    break;
                }
            }
        }
        ASSERT_EQ(indexesCopy.size(), 0);
    }
};

TEST_F(ReshardingRecipientServiceTest, CreateLocalReshardingCollectionBasic) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        kOrigNss, BSON("_id" << 1), boost::optional<std::string>("1"), kOrigUUID);

    {
        // The resharding collection shouldn't exist yet.
        AutoGetCollection autoColl(operationContext(), kReshardingNss, MODE_IS);
        ASSERT_FALSE(autoColl.getCollection());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(
        kReshardingNss, kOrigNss, kReshardingKey, kReshardingUUID, kReshardingEpoch);

    const std::vector<BSONObj> indexes = {BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"),
                                          BSON("v" << 2 << "key"
                                                   << BSON("a" << 1 << "b"
                                                               << "hashed")
                                                   << "name"
                                                   << "indexOne")};
    auto future = launchAsync([&] {
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << "_id_"))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    resharding::createTemporaryReshardingCollectionLocally(
        operationContext(), kReshardingNss, kDefaultFetchTimestamp);

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, indexes);
}

TEST_F(ReshardingRecipientServiceTest,
       CreatingLocalReshardingCollectionRetriesOnStaleVersionErrors) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        kOrigNss, BSON("_id" << 1), boost::optional<std::string>("1"), kOrigUUID);

    {
        // The resharding collection shouldn't exist yet.
        AutoGetCollection autoColl(operationContext(), kReshardingNss, MODE_IS);
        ASSERT_FALSE(autoColl.getCollection());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(
        kReshardingNss, kOrigNss, kReshardingKey, kReshardingUUID, kReshardingEpoch);

    const std::vector<BSONObj> indexes = {BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"),
                                          BSON("v" << 2 << "key"
                                                   << BSON("a" << 1 << "b"
                                                               << "hashed")
                                                   << "name"
                                                   << "indexOne")};
    auto future = launchAsync([&] {
        expectStaleDbVersionError(kOrigNss, "listCollections");
        expectGetDatabase(kOrigNss, shards[1].getHost());
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << "_id_"))},
            HostAndPort(shards[1].getHost()));

        expectStaleEpochError(kOrigNss, "listIndexes");
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    resharding::createTemporaryReshardingCollectionLocally(
        operationContext(), kReshardingNss, kDefaultFetchTimestamp);

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, indexes);
}

TEST_F(ReshardingRecipientServiceTest,
       CreateLocalReshardingCollectionCollectionAlreadyExistsWithNoIndexes) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        kOrigNss, BSON("_id" << 1), boost::optional<std::string>("1"), kOrigUUID);

    {
        // The resharding collection shouldn't exist yet.
        AutoGetCollection autoColl(operationContext(), kReshardingNss, MODE_IS);
        ASSERT_FALSE(autoColl.getCollection());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(
        kReshardingNss, kOrigNss, kReshardingKey, kReshardingUUID, kReshardingEpoch);

    const std::vector<BSONObj> indexes = {BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"),
                                          BSON("v" << 2 << "key"
                                                   << BSON("a" << 1 << "b"
                                                               << "hashed")
                                                   << "name"
                                                   << "indexOne")};

    // Create the collection and indexes to simulate retrying after a failover. Only include the id
    // index, because it is needed to create the collection.
    CollectionOptionsAndIndexes optionsAndIndexes = {
        kReshardingUUID, {indexes[0]}, indexes[0], BSON("uuid" << kReshardingUUID)};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        operationContext(), kReshardingNss, optionsAndIndexes);

    {
        // The collection should exist locally but only have the _id index.
        DBDirectClient client(operationContext());
        auto indexSpecs = client.getIndexSpecs(kReshardingNss, false, 0);
        ASSERT_EQ(indexSpecs.size(), 1);
    }

    auto future = launchAsync([&] {
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << "_id_"))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    resharding::createTemporaryReshardingCollectionLocally(
        operationContext(), kReshardingNss, kDefaultFetchTimestamp);

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, indexes);
}

TEST_F(ReshardingRecipientServiceTest,
       CreateLocalReshardingCollectionCollectionAlreadyExistsWithSomeIndexes) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        kOrigNss, BSON("_id" << 1), boost::optional<std::string>("1"), kOrigUUID);

    {
        // The resharding collection shouldn't exist yet.
        AutoGetCollection autoColl(operationContext(), kReshardingNss, MODE_IS);
        ASSERT_FALSE(autoColl.getCollection());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(
        kReshardingNss, kOrigNss, kReshardingKey, kReshardingUUID, kReshardingEpoch);

    const std::vector<BSONObj> indexes = {BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"),
                                          BSON("v" << 2 << "key"
                                                   << BSON("a" << 1 << "b"
                                                               << "hashed")
                                                   << "name"
                                                   << "indexOne"),
                                          BSON("v" << 2 << "key" << BSON("c.d" << 1) << "name"
                                                   << "nested")};

    // Create the collection and indexes to simulate retrying after a failover. Only include the id
    // index, because it is needed to create the collection.
    CollectionOptionsAndIndexes optionsAndIndexes = {
        kReshardingUUID, {indexes[0], indexes[2]}, indexes[0], BSON("uuid" << kReshardingUUID)};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        operationContext(), kReshardingNss, optionsAndIndexes);

    {
        // The collection should exist locally but only have the _id index.
        DBDirectClient client(operationContext());
        auto indexSpecs = client.getIndexSpecs(kReshardingNss, false, 0);
        ASSERT_EQ(indexSpecs.size(), 2);
    }

    auto future = launchAsync([&] {
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << "_id_"))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    resharding::createTemporaryReshardingCollectionLocally(
        operationContext(), kReshardingNss, kDefaultFetchTimestamp);

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, indexes);
}

TEST_F(ReshardingRecipientServiceTest,
       CreateLocalReshardingCollectionCollectionAlreadyExistsWithAllIndexes) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        kOrigNss, BSON("_id" << 1), boost::optional<std::string>("1"), kOrigUUID);

    {
        // The resharding collection shouldn't exist yet.
        AutoGetCollection autoColl(operationContext(), kReshardingNss, MODE_IS);
        ASSERT_FALSE(autoColl.getCollection());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(
        kReshardingNss, kOrigNss, kReshardingKey, kReshardingUUID, kReshardingEpoch);

    const std::vector<BSONObj> indexes = {BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                                   << "_id_"),
                                          BSON("v" << 2 << "key"
                                                   << BSON("a" << 1 << "b"
                                                               << "hashed")
                                                   << "name"
                                                   << "indexOne")};

    // Create the collection and indexes to simulate retrying after a failover.
    CollectionOptionsAndIndexes optionsAndIndexes = {
        kReshardingUUID, indexes, indexes[0], BSON("uuid" << kReshardingUUID)};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        operationContext(), kReshardingNss, optionsAndIndexes);

    auto future = launchAsync([&] {
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << "_id_"))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    resharding::createTemporaryReshardingCollectionLocally(
        operationContext(), kReshardingNss, kDefaultFetchTimestamp);

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, indexes);
}

}  // namespace
}  // namespace mongo
