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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <list>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class RecipientServiceExternalStateTest : public ShardCatalogCacheTestFixture {
protected:
    RecipientServiceExternalStateTest()
        : ShardCatalogCacheTestFixture(std::make_unique<MongoDScopedGlobalServiceContextForTest>(
              MongoDScopedGlobalServiceContextForTest::Options{}
                  .setCreateShardingState(false)
                  .useMockClock(true)
                  .useMockTickSource(true),
              shouldSetupTL)) {}

    void setUp() override {
        ShardCatalogCacheTestFixture::setUp();

        repl::ReplicationCoordinator::set(
            getServiceContext(),
            std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext()));
        ASSERT_OK(repl::ReplicationCoordinator::get(getServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_PRIMARY));

        auto _storageInterfaceImpl = std::make_unique<repl::StorageInterfaceImpl>();
        repl::StorageInterface::set(getServiceContext(), std::move(_storageInterfaceImpl));

        repl::createOplog(operationContext());

        MongoDSessionCatalog::set(
            getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
        mongoDSessionCatalog->onStepUp(operationContext());
    }

    void tearDown() override {
        ShardCatalogCacheTestFixture::tearDown();
    }

    void expectListCollections(const NamespaceString& nss,
                               UUID uuid,
                               const std::vector<BSONObj>& collectionsDocs,
                               const HostAndPort& expectedHost) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listCollections"_sd);
            ASSERT_EQUALS(nss.dbName(), request.dbname);
            ASSERT_EQUALS(expectedHost, request.target);
            ASSERT_BSONOBJ_EQ(request.cmdObj["filter"].Obj(), BSON("info.uuid" << uuid));
            ASSERT(request.cmdObj.hasField("databaseVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["readConcern"].Obj(),
                              BSON("level" << "local"
                                           << "afterClusterTime" << kDefaultFetchTimestamp));

            std::string listCollectionsNs = str::stream()
                << nss.db_forTest() << "$cmd.listCollections";
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
            ASSERT_EQUALS(nss.dbName(), request.dbname);
            ASSERT_EQUALS(expectedHost, request.target);
            ASSERT_EQ(request.cmdObj.firstElement().checkAndGetStringData(), nss.coll());
            ASSERT(request.cmdObj.hasField("shardVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["readConcern"].Obj(),
                              BSON("level" << "local"
                                           << "afterClusterTime" << kDefaultFetchTimestamp));

            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("id" << 0LL << "ns" << nss.ns_forTest() << "firstBatch" << indexDocs));
        });
    }

    // Loads the metadata for the temporary resharding collection into the catalog cache by mocking
    // network responses. The collection contains a single chunk from minKey to maxKey for the given
    // shard key.
    void loadOneChunkMetadataForTemporaryReshardingColl(const NamespaceString& tempNss,
                                                        const NamespaceString& origNss,
                                                        const ShardKeyPattern& skey,
                                                        UUID uuid,
                                                        OID epoch,
                                                        Timestamp timestamp,
                                                        const BSONObj& collation = {}) {
        auto future = scheduleRoutingInfoForcedRefresh(tempNss);

        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            CollectionType coll(
                tempNss, epoch, timestamp, Date_t::now(), uuid, skey.getKeyPattern());
            coll.setDefaultCollation(collation);

            TypeCollectionReshardingFields reshardingFields;
            reshardingFields.setReshardingUUID(uuid);
            TypeCollectionRecipientFields recipientFields;
            recipientFields.setSourceNss(origNss);
            recipientFields.setSourceUUID(uuid);
            // Populating the set of donor shard ids isn't necessary to test the functionality of
            // creating the temporary resharding collection.
            recipientFields.setDonorShards({});
            recipientFields.setMinimumOperationDurationMillis(5000);

            reshardingFields.setRecipientFields(recipientFields);
            coll.setReshardingFields(reshardingFields);

            ChunkVersion version({epoch, timestamp}, {1, 0});

            ChunkType chunk(uuid,
                            {skey.getKeyPattern().globalMin(), skey.getKeyPattern().globalMax()},
                            version,
                            {"0"});
            chunk.setName(OID::gen());
            version.incMinor();

            const auto chunkObj = BSON("chunks" << chunk.toConfigBSON());
            return std::vector<BSONObj>{coll.toBSON(), chunkObj};
        }());

        future.default_timed_get();
    }

    void expectRefreshReturnForOriginalColl(const NamespaceString& origNss,
                                            const ShardKeyPattern& skey,
                                            UUID uuid,
                                            OID epoch,
                                            Timestamp timestamp) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
            CollectionType coll(
                origNss, epoch, timestamp, Date_t::now(), uuid, skey.getKeyPattern());

            ChunkVersion version({epoch, timestamp}, {2, 0});

            ChunkType chunk(uuid,
                            {skey.getKeyPattern().globalMin(), skey.getKeyPattern().globalMax()},
                            version,
                            {"0"});
            chunk.setName(OID::gen());
            version.incMinor();

            const auto chunkObj = BSON("chunks" << chunk.toConfigBSON());
            return std::vector<BSONObj>{coll.toBSON(), chunkObj};
        }());
    }

    void expectStaleDbVersionError(const NamespaceString& nss, StringData expectedCmdName) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldNameStringData(), expectedCmdName);
            return createErrorCursorResponse(Status(
                StaleDbRoutingVersion(
                    nss.dbName(), DatabaseVersion(UUID::gen(), Timestamp(1, 1)), boost::none),
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

        auto collInfos = client.getCollectionInfos(nss.dbName());
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

    void verifyTempReshardingCollectionAndMetadata() {
        RecipientStateMachineExternalStateImpl externalState;
        externalState.ensureTempReshardingCollectionExistsWithIndexes(
            operationContext(), kMetadata, kDefaultFetchTimestamp);

        auto scopedCsr =
            CollectionShardingRuntime::acquireShared(operationContext(), kReshardingNss);
        ASSERT(scopedCsr->getCurrentMetadataIfKnown() == boost::none);
    }

    const ShardKeyPattern kShardKey = ShardKeyPattern(BSON("_id" << 1));

    const NamespaceString kOrigNss = NamespaceString::createNamespaceString_forTest("db.foo");
    const OID kOrigEpoch = OID::gen();
    const Timestamp kOrigTimestamp = Timestamp(1);
    const UUID kOrigUUID = UUID::gen();

    const NamespaceString kReshardingNss = NamespaceString::createNamespaceString_forTest(
        str::stream() << "db." << NamespaceString::kTemporaryReshardingCollectionPrefix
                      << kOrigUUID);
    const ShardKeyPattern kReshardingKey = ShardKeyPattern(BSON("newKey" << 1));
    const OID kReshardingEpoch = OID::gen();
    const Timestamp kReshardingTimestamp = Timestamp(2);
    const UUID kReshardingUUID = UUID::gen();

    const CommonReshardingMetadata kMetadata{
        kReshardingUUID, kOrigNss, kOrigUUID, kReshardingNss, kReshardingKey.getKeyPattern()};
    const Timestamp kDefaultFetchTimestamp = Timestamp(200, 1);
};

TEST_F(RecipientServiceExternalStateTest, ReshardingConfigServerUpdatesHaveNoTimeout) {
    RecipientStateMachineExternalStateImpl externalState;
    auto future = launchAsync([&] {
        externalState.updateCoordinatorDocument(
            operationContext(), BSON("query" << "test"), BSON("update" << "test"));
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_FALSE(request.cmdObj.hasField("maxTimeMS"));
        ASSERT_EQUALS(request.timeout, executor::RemoteCommandRequest::kNoTimeout);
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(RecipientServiceExternalStateTest, CreateLocalReshardingCollectionBasic) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(kOrigNss,
                                                  kShardKey.toBSON(),
                                                  boost::optional<std::string>("1"),
                                                  kOrigUUID,
                                                  kOrigEpoch,
                                                  kOrigTimestamp);

    {
        // The resharding collection shouldn't exist yet.
        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{kReshardingNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_FALSE(coll.exists());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(kReshardingNss,
                                                   kOrigNss,
                                                   kReshardingKey,
                                                   kReshardingUUID,
                                                   kReshardingEpoch,
                                                   kReshardingTimestamp);

    const std::vector<BSONObj> indexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "key"
                 << BSON("a" << 1 << "b"
                             << "hashed")
                 << "name"
                 << "indexOne")};
    // When creating collection, only _id index should be created.
    const std::vector<BSONObj> expectedIndexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName)};
    auto future = launchAsync([&] {
        expectRefreshReturnForOriginalColl(
            kOrigNss, kShardKey, kOrigUUID, kOrigEpoch, kOrigTimestamp);
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << IndexConstants::kIdIndexName))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    verifyTempReshardingCollectionAndMetadata();

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, expectedIndexes);
}

TEST_F(RecipientServiceExternalStateTest,
       CreatingLocalReshardingCollectionRetriesOnStaleVersionErrors) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(kOrigNss,
                                                  kShardKey.toBSON(),
                                                  boost::optional<std::string>("1"),
                                                  kOrigUUID,
                                                  kOrigEpoch,
                                                  kOrigTimestamp);

    {
        // The resharding collection shouldn't exist yet.
        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{kReshardingNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_FALSE(coll.exists());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(kReshardingNss,
                                                   kOrigNss,
                                                   kReshardingKey,
                                                   kReshardingUUID,
                                                   kReshardingEpoch,
                                                   kReshardingTimestamp);

    const std::vector<BSONObj> indexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "key"
                 << BSON("a" << 1 << "b"
                             << "hashed")
                 << "name"
                 << "indexOne")};
    // When creating collection, only _id index should be created.
    const std::vector<BSONObj> expectedIndexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName)};

    auto future = launchAsync([&] {
        expectRefreshReturnForOriginalColl(
            kOrigNss, kShardKey, kOrigUUID, kOrigEpoch, kOrigTimestamp);
        expectStaleDbVersionError(kOrigNss, "listCollections");
        expectGetDatabase(kOrigNss, shards[1].getHost());
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << IndexConstants::kIdIndexName))},
            HostAndPort(shards[1].getHost()));

        expectStaleEpochError(kOrigNss, "listIndexes");
        expectRefreshReturnForOriginalColl(
            kOrigNss, kShardKey, kOrigUUID, kOrigEpoch, kOrigTimestamp);
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    verifyTempReshardingCollectionAndMetadata();

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, expectedIndexes);
}

TEST_F(RecipientServiceExternalStateTest,
       CreateLocalReshardingCollectionCollectionAlreadyExistsWithNoIndexes) {
    auto shards = setupNShards(2);

    // Shard kOrigNss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1"
    // respectively. ShardId("1") is the primary shard for the database.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(kOrigNss,
                                                  kShardKey.toBSON(),
                                                  boost::optional<std::string>("1"),
                                                  kOrigUUID,
                                                  kOrigEpoch,
                                                  kOrigTimestamp);

    {
        // The resharding collection shouldn't exist yet.
        const auto coll = acquireCollection(
            operationContext(),
            CollectionAcquisitionRequest{kReshardingNss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(operationContext()),
                                         AcquisitionPrerequisites::kRead},
            MODE_IS);
        ASSERT_FALSE(coll.exists());
    }

    // Simulate a refresh for the temporary resharding collection.
    loadOneChunkMetadataForTemporaryReshardingColl(kReshardingNss,
                                                   kOrigNss,
                                                   kReshardingKey,
                                                   kReshardingUUID,
                                                   kReshardingEpoch,
                                                   kReshardingTimestamp);

    const std::vector<BSONObj> indexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSON("v" << 2 << "key"
                 << BSON("a" << 1 << "b"
                             << "hashed")
                 << "name"
                 << "indexOne")};
    // When creating collection, only _id index should be created.
    const std::vector<BSONObj> expectedIndexes = {
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName)};
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
        expectRefreshReturnForOriginalColl(
            kOrigNss, kShardKey, kOrigUUID, kOrigEpoch, kOrigTimestamp);
        expectListCollections(
            kOrigNss,
            kOrigUUID,
            {BSON("name" << kOrigNss.coll() << "options" << BSONObj() << "info"
                         << BSON("readOnly" << false << "uuid" << kOrigUUID) << "idIndex"
                         << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                     << IndexConstants::kIdIndexName))},
            HostAndPort(shards[1].getHost()));
        expectListIndexes(kOrigNss, kOrigUUID, indexes, HostAndPort(shards[0].getHost()));
    });

    verifyTempReshardingCollectionAndMetadata();

    future.default_timed_get();

    verifyCollectionAndIndexes(kReshardingNss, kReshardingUUID, expectedIndexes);
}

}  // namespace
}  // namespace mongo
