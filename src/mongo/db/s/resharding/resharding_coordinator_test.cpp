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

#include <boost/optional.hpp>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class ReshardingCoordinatorPersistenceTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        ShardType shard0;
        shard0.setName("shard0000");
        shard0.setHost("shard0000:1234");
        ShardType shard1;
        shard1.setName("shard0001");
        shard1.setHost("shard0001:1234");
        setupShards({shard0, shard1});

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace.ns());
        client.createCollection(CollectionType::ConfigNS.ns());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc(
        CoordinatorStateEnum state, boost::optional<Timestamp> fetchTimestamp = boost::none) {
        CommonReshardingMetadata meta(
            _reshardingUUID, _originalNss, UUID::gen(), _newShardKey.toBSON());
        ReshardingCoordinatorDocument doc(_tempNss,
                                          state,
                                          {DonorShardEntry(ShardId("shard0000"))},
                                          {RecipientShardEntry(ShardId("shard0001"))});
        doc.setCommonReshardingMetadata(meta);
        emplaceFetchTimestampIfExists(doc, std::move(fetchTimestamp));
        return doc;
    }

    CollectionType makeOriginalCollectionCatalogEntry(
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        OID epoch,
        Date_t lastUpdated) {
        UUID uuid = UUID::gen();
        BSONObj shardKey;
        if (coordinatorDoc.getState() >= CoordinatorStateEnum::kCommitted &&
            coordinatorDoc.getState() != CoordinatorStateEnum::kError) {
            uuid = _reshardingUUID;
            shardKey = _newShardKey.toBSON();
        } else {
            uuid = _originalUUID;
            shardKey = _oldShardKey.toBSON();
        }

        CollectionType collType(
            coordinatorDoc.getNss(), std::move(epoch), lastUpdated, std::move(uuid));
        collType.setKeyPattern(shardKey);
        collType.setUnique(false);
        if (reshardingFields)
            collType.setReshardingFields(std::move(reshardingFields.get()));

        return collType;
    }

    // Returns the chunk for the donor shard.
    ChunkType makeAndInsertChunksForDonorShard(const NamespaceString& nss,
                                               OID epoch,
                                               const ShardKeyPattern& shardKey,
                                               std::vector<OID> ids) {
        auto chunks = makeChunks(nss, epoch, shardKey, ids);

        // Only the chunk corresponding to shard0000 is stored as a donor in the coordinator state
        // document constructed.
        auto donorChunk = chunks[0];
        insertChunkAndZoneEntries({donorChunk}, {});
        return donorChunk;
    }

    // Returns the chunk for the recipient shard.
    ChunkType makeAndInsertChunksForRecipientShard(const NamespaceString& nss,
                                                   OID epoch,
                                                   const ShardKeyPattern& shardKey,
                                                   std::vector<OID> ids) {
        auto chunks = makeChunks(nss, epoch, shardKey, ids);

        // Only the chunk corresponding to shard0001 is stored as a recipient in the coordinator
        // state document constructed.
        auto recipientChunk = chunks[1];
        insertChunkAndZoneEntries({recipientChunk}, {});
        return recipientChunk;
    }

    std::vector<ChunkType> makeChunks(const NamespaceString& nss,
                                      OID epoch,
                                      const ShardKeyPattern& shardKey,
                                      std::vector<OID> ids) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two chunks, one on each shard with the given namespace and epoch
        ChunkVersion version(1, 0, epoch);
        ChunkType chunk1(nss, chunkRanges[0], version, ShardId("shard0000"));
        chunk1.setName(ids[0]);
        ChunkType chunk2(nss, chunkRanges[1], version, ShardId("shard0001"));
        chunk2.setName(ids[1]);

        return std::vector<ChunkType>{chunk1, chunk2};
    }

    std::vector<TagsType> makeZones(const NamespaceString& nss, const ShardKeyPattern& shardKey) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two zones for the given namespace
        TagsType tag1(nss, "zone1", chunkRanges[0]);
        TagsType tag2(nss, "zone2", chunkRanges[1]);

        return std::vector<TagsType>{tag1, tag2};
    }

    ReshardingCoordinatorDocument insertStateAndCatalogEntries(
        CoordinatorStateEnum state,
        OID epoch,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        auto coordinatorDoc = makeCoordinatorDoc(state, fetchTimestamp);
        client.insert(NamespaceString::kConfigReshardingOperationsNamespace.ns(),
                      coordinatorDoc.toBSON());

        TypeCollectionReshardingFields reshardingFields(coordinatorDoc.get_id());
        reshardingFields.setState(coordinatorDoc.getState());
        reshardingFields.setDonorFields(
            TypeCollectionDonorFields(coordinatorDoc.getReshardingKey()));

        auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
            coordinatorDoc,
            reshardingFields,
            std::move(epoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        client.insert(CollectionType::ConfigNS.ns(), originalNssCatalogEntry.toBSON());

        auto tempNssCatalogEntry = resharding::createTempReshardingCollectionType(
            opCtx, coordinatorDoc, ChunkVersion(1, 1, OID::gen()), BSONObj());
        client.insert(CollectionType::ConfigNS.ns(), tempNssCatalogEntry.toBSON());

        return coordinatorDoc;
    }

    void insertChunkAndZoneEntries(std::vector<ChunkType> chunks, std::vector<TagsType> zones) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        for (const auto& chunk : chunks) {
            client.insert(ChunkType::ConfigNS.ns(), chunk.toConfigBSON());
        }

        for (const auto& zone : zones) {
            client.insert(TagsType::ConfigNS.ns(), zone.toBSON());
        }
    }

    void readReshardingCoordinatorDocAndAssertMatchesExpected(
        OperationContext* opCtx, ReshardingCoordinatorDocument expectedCoordinatorDoc) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace.ns(),
                                  Query(BSON("nss" << expectedCoordinatorDoc.getNss().ns())));

        auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
            IDLParserErrorContext("ReshardingCoordinatorTest"), doc);

        ASSERT_EQUALS(coordinatorDoc.get_id(), expectedCoordinatorDoc.get_id());
        ASSERT_EQUALS(coordinatorDoc.getNss(), expectedCoordinatorDoc.getNss());
        ASSERT_EQUALS(coordinatorDoc.getTempReshardingNss(),
                      expectedCoordinatorDoc.getTempReshardingNss());
        ASSERT_EQUALS(coordinatorDoc.getReshardingKey().toBSON().woCompare(
                          expectedCoordinatorDoc.getReshardingKey().toBSON()),
                      0);
        ASSERT(coordinatorDoc.getState() == expectedCoordinatorDoc.getState());
        if (expectedCoordinatorDoc.getFetchTimestamp()) {
            ASSERT_EQUALS(coordinatorDoc.getFetchTimestamp().get(),
                          expectedCoordinatorDoc.getFetchTimestamp().get());
        } else {
            ASSERT(!coordinatorDoc.getFetchTimestamp());
        }

        auto expectedDonorShards = expectedCoordinatorDoc.getDonorShards();
        auto onDiskDonorShards = coordinatorDoc.getDonorShards();
        ASSERT_EQUALS(onDiskDonorShards.size(), expectedDonorShards.size());
        for (auto it = expectedDonorShards.begin(); it != expectedDonorShards.end(); it++) {
            auto shardId = it->getId();
            auto onDiskIt =
                std::find_if(onDiskDonorShards.begin(),
                             onDiskDonorShards.end(),
                             [shardId](DonorShardEntry d) { return d.getId() == shardId; });
            ASSERT(onDiskIt != onDiskDonorShards.end());
            if (it->getMinFetchTimestamp()) {
                ASSERT(onDiskIt->getMinFetchTimestamp());
                ASSERT_EQUALS(onDiskIt->getMinFetchTimestamp().get(),
                              it->getMinFetchTimestamp().get());
            } else {
                ASSERT(!onDiskIt->getMinFetchTimestamp());
            }
            ASSERT(onDiskIt->getState() == it->getState());
        }

        auto expectedRecipientShards = expectedCoordinatorDoc.getRecipientShards();
        auto onDiskRecipientShards = coordinatorDoc.getRecipientShards();
        ASSERT_EQUALS(onDiskRecipientShards.size(), expectedRecipientShards.size());
        for (auto it = expectedRecipientShards.begin(); it != expectedRecipientShards.end(); it++) {
            auto shardId = it->getId();
            auto onDiskIt =
                std::find_if(onDiskRecipientShards.begin(),
                             onDiskRecipientShards.end(),
                             [shardId](RecipientShardEntry r) { return r.getId() == shardId; });
            ASSERT(onDiskIt != onDiskRecipientShards.end());
            if (it->getStrictConsistencyTimestamp()) {
                ASSERT(onDiskIt->getStrictConsistencyTimestamp());
                ASSERT_EQUALS(onDiskIt->getStrictConsistencyTimestamp().get(),
                              it->getStrictConsistencyTimestamp().get());
            } else {
                ASSERT(!onDiskIt->getStrictConsistencyTimestamp());
            }
            ASSERT(onDiskIt->getState() == it->getState());
        }
    }

    void readOriginalCollectionCatalogEntryAndAssertReshardingFieldsMatchExpected(
        OperationContext* opCtx, CollectionType expectedCollType, bool doneState) {
        DBDirectClient client(opCtx);
        CollectionType onDiskEntry(
            client.findOne(CollectionType::ConfigNS.ns(), Query(BSON("_id" << _originalNss.ns()))));

        auto expectedReshardingFields = expectedCollType.getReshardingFields();
        if (doneState ||
            (expectedReshardingFields &&
             expectedReshardingFields->getState() >= CoordinatorStateEnum::kCommitted &&
             expectedReshardingFields->getState() != CoordinatorStateEnum::kError)) {
            ASSERT_EQUALS(onDiskEntry.getNss(), _originalNss);
            ASSERT(onDiskEntry.getUuid() == _reshardingUUID);
            ASSERT_EQUALS(onDiskEntry.getKeyPattern().toBSON().woCompare(_newShardKey.toBSON()), 0);
            ASSERT_NOT_EQUALS(onDiskEntry.getEpoch(), _originalEpoch);
        }

        if (!expectedReshardingFields)
            return;

        ASSERT(onDiskEntry.getReshardingFields());
        auto onDiskReshardingFields = onDiskEntry.getReshardingFields().get();
        ASSERT(onDiskReshardingFields.getUuid() == expectedReshardingFields->getUuid());
        ASSERT(onDiskReshardingFields.getState() == expectedReshardingFields->getState());

        ASSERT(onDiskReshardingFields.getDonorFields());
        ASSERT_EQUALS(
            onDiskReshardingFields.getDonorFields()->getReshardingKey().toBSON().woCompare(
                expectedReshardingFields->getDonorFields()->getReshardingKey().toBSON()),
            0);

        // 'recipientFields' should only in the entry for the temporary collection.
        ASSERT(!onDiskReshardingFields.getRecipientFields());
    }

    void readTemporaryCollectionCatalogEntryAndAssertReshardingFieldsMatchExpected(
        OperationContext* opCtx, boost::optional<CollectionType> expectedCollType) {
        DBDirectClient client(opCtx);
        auto doc =
            client.findOne(CollectionType::ConfigNS.ns(), Query(BSON("_id" << _tempNss.ns())));
        if (!expectedCollType) {
            ASSERT(doc.isEmpty());
            return;
        }

        auto expectedReshardingFields = expectedCollType->getReshardingFields().get();
        CollectionType onDiskEntry(doc);
        ASSERT(onDiskEntry.getReshardingFields());

        auto onDiskReshardingFields = onDiskEntry.getReshardingFields().get();
        ASSERT_EQUALS(onDiskReshardingFields.getUuid(), expectedReshardingFields.getUuid());
        ASSERT(onDiskReshardingFields.getState() == expectedReshardingFields.getState());

        ASSERT(onDiskReshardingFields.getRecipientFields());
        ASSERT_EQUALS(onDiskReshardingFields.getRecipientFields()->getOriginalNamespace(),
                      expectedReshardingFields.getRecipientFields()->getOriginalNamespace());

        if (expectedReshardingFields.getRecipientFields()->getFetchTimestamp()) {
            ASSERT(onDiskReshardingFields.getRecipientFields()->getFetchTimestamp());
            ASSERT_EQUALS(onDiskReshardingFields.getRecipientFields()->getFetchTimestamp().get(),
                          expectedReshardingFields.getRecipientFields()->getFetchTimestamp().get());
        } else {
            ASSERT(!onDiskReshardingFields.getRecipientFields()->getFetchTimestamp());
        }

        // 'donorFields' should not exist for the temporary collection.
        ASSERT(!onDiskReshardingFields.getDonorFields());
    }

    void readChunkCatalogEntriesAndAssertMatchExpected(OperationContext* opCtx,
                                                       std::vector<ChunkType> expectedChunks) {
        auto nss = expectedChunks[0].getNS();

        DBDirectClient client(opCtx);
        std::vector<ChunkType> foundChunks;
        auto cursor = client.query(ChunkType::ConfigNS, Query(BSON("ns" << nss.ns())));
        while (cursor->more()) {
            auto d = uassertStatusOK(ChunkType::fromConfigBSON(cursor->nextSafe().getOwned()));
            foundChunks.push_back(d);
        }

        ASSERT_EQUALS(foundChunks.size(), expectedChunks.size());
        for (auto it = expectedChunks.begin(); it != expectedChunks.end(); it++) {
            auto id = it->getName();
            auto onDiskIt = std::find_if(foundChunks.begin(), foundChunks.end(), [id](ChunkType c) {
                return c.getName() == id;
            });
            ASSERT(onDiskIt != foundChunks.end());
            ASSERT_EQUALS(onDiskIt->toConfigBSON().woCompare(it->toConfigBSON()), 0);
        }
    }

    void readTagCatalogEntriesAndAssertMatchExpected(OperationContext* opCtx,
                                                     std::vector<TagsType> expectedZones) {
        auto nss = expectedZones[0].getNS();

        DBDirectClient client(opCtx);
        std::vector<TagsType> foundZones;
        auto cursor = client.query(TagsType::ConfigNS, Query(BSON("ns" << nss.ns())));
        while (cursor->more()) {
            foundZones.push_back(
                uassertStatusOK(TagsType::fromBSON(cursor->nextSafe().getOwned())));
        }

        ASSERT_EQUALS(foundZones.size(), expectedZones.size());
        for (auto it = expectedZones.begin(); it != expectedZones.end(); it++) {
            auto tagName = it->getTag();
            auto onDiskIt = std::find_if(foundZones.begin(),
                                         foundZones.end(),
                                         [tagName](TagsType t) { return t.getTag() == tagName; });
            ASSERT(onDiskIt != foundZones.end());
            ASSERT_EQUALS(onDiskIt->toBSON().woCompare(it->toBSON()), 0);
        }
    }

    void assertStateAndCatalogEntriesMatchExpected(
        OperationContext* opCtx,
        ReshardingCoordinatorDocument expectedCoordinatorDoc,
        OID collectionEpoch) {
        readReshardingCoordinatorDocAndAssertMatchesExpected(opCtx, expectedCoordinatorDoc);

        // Check the resharding fields in the config.collections entry for the original collection
        TypeCollectionReshardingFields originalReshardingFields(expectedCoordinatorDoc.get_id());
        originalReshardingFields.setState(expectedCoordinatorDoc.getState());
        TypeCollectionDonorFields donorField(expectedCoordinatorDoc.getReshardingKey());
        originalReshardingFields.setDonorFields(donorField);
        auto originalCollType = makeOriginalCollectionCatalogEntry(
            expectedCoordinatorDoc,
            originalReshardingFields,
            std::move(collectionEpoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        readOriginalCollectionCatalogEntryAndAssertReshardingFieldsMatchExpected(
            opCtx,
            originalCollType,
            expectedCoordinatorDoc.getState() == CoordinatorStateEnum::kDone);

        // Check the resharding fields in the config.collections entry for the temp collection. If
        // the expected state is >= kCommitted, the entry for the temp collection should have been
        // removed.
        boost::optional<CollectionType> tempCollType = boost::none;
        if (expectedCoordinatorDoc.getState() < CoordinatorStateEnum::kCommitted ||
            expectedCoordinatorDoc.getState() == CoordinatorStateEnum::kError) {
            tempCollType = resharding::createTempReshardingCollectionType(
                opCtx, expectedCoordinatorDoc, ChunkVersion(1, 1, OID::gen()), BSONObj());
        }

        readTemporaryCollectionCatalogEntryAndAssertReshardingFieldsMatchExpected(opCtx,
                                                                                  tempCollType);
    }

    void persistInitialStateAndCatalogUpdatesExpectSuccess(
        OperationContext* opCtx,
        ReshardingCoordinatorDocument expectedCoordinatorDoc,
        std::vector<ChunkType> initialChunks,
        std::vector<TagsType> newZones) {
        // Create original collection's catalog entry as well as both config.chunks and config.tags
        // collections.
        {
            auto opCtx = operationContext();
            DBDirectClient client(opCtx);

            TypeCollectionReshardingFields reshardingFields(expectedCoordinatorDoc.get_id());
            reshardingFields.setState(expectedCoordinatorDoc.getState());
            reshardingFields.setDonorFields(
                TypeCollectionDonorFields(expectedCoordinatorDoc.getReshardingKey()));

            auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
                expectedCoordinatorDoc,
                reshardingFields,
                _originalEpoch,
                opCtx->getServiceContext()->getPreciseClockSource()->now());
            client.insert(CollectionType::ConfigNS.ns(), originalNssCatalogEntry.toBSON());

            client.createCollection(ChunkType::ConfigNS.ns());
            client.createCollection(TagsType::ConfigNS.ns());
        }

        resharding::persistInitialStateAndCatalogUpdates(
            opCtx, expectedCoordinatorDoc, initialChunks, newZones);

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _originalEpoch);

        // Check that chunks and tags entries have been correctly created
        readChunkCatalogEntriesAndAssertMatchExpected(opCtx, initialChunks);
        readTagCatalogEntriesAndAssertMatchExpected(opCtx, newZones);
    }

    void persistStateTransitionUpdateExpectSuccess(
        OperationContext* opCtx, ReshardingCoordinatorDocument expectedCoordinatorDoc) {
        resharding::persistStateTransitionAndCatalogUpdatesThenBumpShardVersions(
            opCtx, expectedCoordinatorDoc);

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _originalEpoch);
    }

    void persistCommittedStateExpectSuccess(OperationContext* opCtx,
                                            ReshardingCoordinatorDocument expectedCoordinatorDoc,
                                            Timestamp fetchTimestamp,
                                            std::vector<ChunkType> expectedChunks,
                                            std::vector<TagsType> expectedZones) {
        resharding::persistCommittedState(operationContext(), expectedCoordinatorDoc, _finalEpoch);

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _finalEpoch);

        // Check that chunks and tags under the temp namespace have been removed
        DBDirectClient client(opCtx);
        auto chunkDoc =
            client.findOne(ChunkType::ConfigNS.ns(), Query(BSON("ns" << _tempNss.ns())));
        ASSERT(chunkDoc.isEmpty());

        auto tagDoc = client.findOne(TagsType::ConfigNS.ns(), Query(BSON("ns" << _tempNss.ns())));
        ASSERT(tagDoc.isEmpty());

        // Check that chunks and tags entries previously under the temporary namespace have been
        // correctly updated to the original namespace
        readChunkCatalogEntriesAndAssertMatchExpected(opCtx, expectedChunks);
        readTagCatalogEntriesAndAssertMatchExpected(opCtx, expectedZones);
    }

    void removeCoordinatorDocAndReshardingFieldsExpectSuccess(
        OperationContext* opCtx, ReshardingCoordinatorDocument expectedCoordinatorDoc) {
        resharding::removeCoordinatorDocAndReshardingFields(opCtx, expectedCoordinatorDoc);

        // Check that the entry is removed from config.reshardingOperations
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace.ns(),
                                  Query(BSON("nss" << expectedCoordinatorDoc.getNss().ns())));
        ASSERT(doc.isEmpty());

        // Check that the resharding fields are removed from the config.collections entry
        auto collType = makeOriginalCollectionCatalogEntry(
            expectedCoordinatorDoc,
            boost::none,
            _finalEpoch,
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        readOriginalCollectionCatalogEntryAndAssertReshardingFieldsMatchExpected(
            opCtx, collType, true);
    }

    void assertChunkVersionDidNotIncreaseAfterStateTransition(
        const ChunkType& chunk, const ChunkVersion& collectionVersion) {
        auto chunkAfterTransition = getChunkDoc(operationContext(), chunk.getMin());
        ASSERT_EQ(chunkAfterTransition.getStatus(), Status::OK());
        ASSERT_EQ(chunkAfterTransition.getValue().getVersion().majorVersion(),
                  collectionVersion.majorVersion());
    }

    void assertChunkVersionIncreasedAfterStateTransition(const ChunkType& chunk,
                                                         const ChunkVersion& collectionVersion) {
        auto chunkAfterTransition = getChunkDoc(operationContext(), chunk.getMin());
        ASSERT_EQ(chunkAfterTransition.getStatus(), Status::OK());
        ASSERT_EQ(chunkAfterTransition.getValue().getVersion().majorVersion(),
                  collectionVersion.majorVersion() + 1);
    }

    NamespaceString _originalNss = NamespaceString("db.foo");
    UUID _originalUUID = UUID::gen();
    OID _originalEpoch = OID::gen();

    NamespaceString _tempNss = NamespaceString("db.system.resharding." + _originalUUID.toString());
    UUID _reshardingUUID = UUID::gen();
    OID _tempEpoch = OID::gen();

    OID _finalEpoch = OID::gen();

    ShardKeyPattern _oldShardKey = ShardKeyPattern(BSON("oldSK" << 1));
    ShardKeyPattern _newShardKey = ShardKeyPattern(BSON("newSK" << 1));

    const std::vector<ChunkRange> _oldChunkRanges = {
        ChunkRange(_oldShardKey.getKeyPattern().globalMin(), BSON("oldSK" << 12345)),
        ChunkRange(BSON("oldSK" << 12345), _oldShardKey.getKeyPattern().globalMax()),
    };
    const std::vector<ChunkRange> _newChunkRanges = {
        ChunkRange(_newShardKey.getKeyPattern().globalMin(), BSON("newSK" << 0)),
        ChunkRange(BSON("newSK" << 0), _newShardKey.getKeyPattern().globalMax()),
    };
};

TEST_F(ReshardingCoordinatorPersistenceTest, PersistInitialInfoSucceeds) {
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kInitializing);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kPreparingToDonate.
    auto donorChunk = makeAndInsertChunksForDonorShard(
        _originalNss, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});
    auto collectionVersion = donorChunk.getVersion();

    auto initialChunks =
        makeChunks(_tempNss, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});
    auto newZones = makeZones(_tempNss, _newShardKey);

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    persistInitialStateAndCatalogUpdatesExpectSuccess(
        operationContext(), expectedCoordinatorDoc, initialChunks, newZones);

    // Confirm the shard version was increased for the donor shard.
    auto donorChunkPostTransition = getChunkDoc(operationContext(), donorChunk.getMin());
    ASSERT_EQ(donorChunkPostTransition.getStatus(), Status::OK());
    ASSERT_EQ(donorChunkPostTransition.getValue().getVersion().majorVersion(),
              collectionVersion.majorVersion() + 1);
}

TEST_F(ReshardingCoordinatorPersistenceTest, PersistBasicStateTransitionSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kCloning, _originalEpoch);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kPreparingToDonate.
    auto donorChunk = makeAndInsertChunksForDonorShard(
        _originalNss, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});
    auto collectionVersion = donorChunk.getVersion();

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kMirroring);

    persistStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);
    assertChunkVersionIncreasedAfterStateTransition(donorChunk, collectionVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, PersistFetchTimestampStateTransitionSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kPreparingToDonate, _originalEpoch);

    auto recipientChunk = makeAndInsertChunksForRecipientShard(
        _tempNss, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});
    auto collectionVersion = recipientChunk.getVersion();

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kCloning);
    emplaceFetchTimestampIfExists(expectedCoordinatorDoc, Timestamp(1, 1));

    persistStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);
    assertChunkVersionIncreasedAfterStateTransition(recipientChunk, collectionVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, PersistCommitSucceeds) {
    Timestamp fetchTimestamp = Timestamp(1, 1);
    auto coordinatorDoc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kMirroring, _originalEpoch, fetchTimestamp);
    auto initialChunksIds = std::vector{OID::gen(), OID::gen()};

    auto tempNssChunks = makeChunks(_tempNss, _tempEpoch, _newShardKey, initialChunksIds);
    auto recipientChunk = tempNssChunks[1];
    insertChunkAndZoneEntries(tempNssChunks, makeZones(_tempNss, _newShardKey));

    insertChunkAndZoneEntries(
        makeChunks(_originalNss, OID::gen(), _oldShardKey, std::vector{OID::gen(), OID::gen()}),
        makeZones(_originalNss, _oldShardKey));

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitted);

    // The new epoch to use for the resharded collection to indicate that the collection is a
    // new incarnation of the namespace
    auto updatedChunks = makeChunks(_originalNss, _finalEpoch, _newShardKey, initialChunksIds);
    auto updatedZones = makeZones(_originalNss, _newShardKey);

    persistCommittedStateExpectSuccess(
        operationContext(), expectedCoordinatorDoc, fetchTimestamp, updatedChunks, updatedZones);

    assertChunkVersionDidNotIncreaseAfterStateTransition(recipientChunk,
                                                         recipientChunk.getVersion());
}

TEST_F(ReshardingCoordinatorPersistenceTest, PersistTransitionToErrorSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kPreparingToDonate, _originalEpoch);

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kError);

    persistStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);
}

TEST_F(ReshardingCoordinatorPersistenceTest, PersistTransitionToDoneSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kDropping, _finalEpoch);

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);

    removeCoordinatorDocAndReshardingFieldsExpectSuccess(operationContext(),
                                                         expectedCoordinatorDoc);
}

TEST_F(ReshardingCoordinatorPersistenceTest,
       PersistStateTransitionWhenCoordinatorDocDoesNotExistFails) {
    // Do not insert initial entry into config.reshardingOperations. Attempt to update coordinator
    // state documents.
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kCloning, Timestamp(1, 1));
    ASSERT_THROWS_CODE(resharding::persistStateTransitionAndCatalogUpdatesThenBumpShardVersions(
                           operationContext(), coordinatorDoc),
                       AssertionException,
                       50577);
}

TEST_F(ReshardingCoordinatorPersistenceTest,
       PersistInitialStateOriginalNamespaceCatalogEntryMissingFails) {
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kInitializing);
    auto initialChunks =
        makeChunks(_tempNss, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});
    auto newZones = makeZones(_tempNss, _newShardKey);

    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Do not create the config.collections entry for the original collection
    ASSERT_THROWS_CODE(resharding::persistInitialStateAndCatalogUpdates(
                           operationContext(), coordinatorDoc, initialChunks, newZones),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);
}

}  // namespace
}  // namespace mongo
