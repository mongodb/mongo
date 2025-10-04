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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_gen.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/index_on_config.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using namespace resharding;
using unittest::assertGet;

const std::string kZone1 = "zone1";
const std::string kZone2 = "zone2";

PhaseTransitionFn createPreparingToDonateDaoUpdate(
    ReshardingCoordinatorDocument expectedCoordinatorDoc) {
    return [expectedCoordinatorDoc](OperationContext* opCtx, TxnNumber txnNumber) {
        DBDirectClient client(opCtx);
        const BSONObj query(BSON(ReshardingCoordinatorDocument::kReshardingUUIDFieldName
                                 << expectedCoordinatorDoc.getReshardingUUID() << "ns"
                                 << expectedCoordinatorDoc.getSourceNss().ns_forTest()));

        BSONArrayBuilder donorShardsArrayBuilder;
        for (const auto& shard : expectedCoordinatorDoc.getDonorShards()) {
            donorShardsArrayBuilder.append(shard.toBSON());
        }

        BSONArrayBuilder recipientShardsArrayBuilder;
        for (const auto& shard : expectedCoordinatorDoc.getRecipientShards()) {
            recipientShardsArrayBuilder.append(shard.toBSON());
        }

        auto updates =
            BSON("$set" << BSON("state" << "preparing-to-donate"
                                        << "donorShards" << donorShardsArrayBuilder.arr()
                                        << "recipientShards" << recipientShardsArrayBuilder.arr())
                        << "$unset"
                        << BSON("presetReshardedChunks" << ""
                                                        << "zones"
                                                        << ""));

        client.update(NamespaceString::kConfigReshardingOperationsNamespace, query, updates);
        return expectedCoordinatorDoc;
    };
}

class ReshardingCoordinatorPersistenceTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        ShardType shard0;
        shard0.setName("shard0000");
        shard0.setHost("shard0000:1234");
        shard0.setTags({kZone1});
        ShardType shard1;
        shard1.setName("shard0001");
        shard1.setHost("shard0001:1234");
        shard1.setTags({kZone2});
        setupShards({shard0, shard1});
        setClusterHasTwoShards();

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace);
        client.createCollection(CollectionType::ConfigNS);
        client.createIndex(TagsType::ConfigNS, BSON("ns" << 1 << "min" << 1));
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        _metrics = ReshardingMetrics::makeInstance_forTest(
            _originalUUID,
            _newShardKey.toBSON(),
            _originalNss,
            ReshardingMetrics::Role::kCoordinator,
            getServiceContext()->getFastClockSource()->now(),
            getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interrupt();
        ConfigServerTestFixture::tearDown();
    }

    void setClusterHasTwoShards() {
        // Set the cluster cardinality parameter to true
        ShardedClusterCardinalityParam cardinality;
        ClusterServerParameter baseCSP;
        baseCSP.setClusterParameterTime(LogicalTime(Timestamp(Date_t::now())));
        baseCSP.set_id("shardedClusterCardinalityForDirectConns"_sd);
        cardinality.setClusterServerParameter(baseCSP);
        cardinality.setHasTwoOrMoreShards(true);
        auto param = ServerParameterSet::getClusterParameterSet()->get(
            "shardedClusterCardinalityForDirectConns");
        ASSERT_OK(param->set(cardinality.toBSON(), boost::none));
    }

    ReshardingCoordinatorDocument makeCoordinatorDoc(
        CoordinatorStateEnum state,
        bool useUserUUID = false,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        CommonReshardingMetadata meta(
            _reshardingUUID, _originalNss, UUID::gen(), _tempNss, _newShardKey.toBSON());
        if (useUserUUID) {
            meta.setUserReshardingUUID(_reshardingUUID);
        }
        ReshardingCoordinatorDocument doc(state,
                                          {DonorShardEntry(ShardId("shard0000"), {})},
                                          {RecipientShardEntry(ShardId("shard0001"), {})});
        doc.setCommonReshardingMetadata(meta);
        emplaceCloneTimestampIfExists(doc, std::move(fetchTimestamp));
        return doc;
    }

    CollectionType makeOriginalCollectionCatalogEntry(
        ReshardingCoordinatorDocument coordinatorDoc,
        boost::optional<TypeCollectionReshardingFields> reshardingFields,
        OID epoch,
        Date_t lastUpdated) {
        UUID uuid = UUID::gen();
        BSONObj shardKey;
        if (coordinatorDoc.getState() >= CoordinatorStateEnum::kCommitting) {
            uuid = _reshardingUUID;
            shardKey = _newShardKey.toBSON();
        } else {
            uuid = _originalUUID;
            shardKey = _oldShardKey.toBSON();
        }

        CollectionType collType(coordinatorDoc.getSourceNss(),
                                std::move(epoch),
                                Timestamp(1, 2),
                                lastUpdated,
                                std::move(uuid),
                                shardKey);
        if (reshardingFields)
            collType.setReshardingFields(std::move(reshardingFields.value()));

        if (coordinatorDoc.getState() == CoordinatorStateEnum::kDone ||
            coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced ||
            coordinatorDoc.getState() == CoordinatorStateEnum::kAborting) {
            collType.setAllowMigrations(true);
        } else if (coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate) {
            collType.setAllowMigrations(false);
        }
        return collType;
    }

    // Returns the chunk for the donor shard.
    ChunkType makeAndInsertChunksForDonorShard(const UUID& uuid,
                                               OID epoch,
                                               const ShardKeyPattern& shardKey,
                                               std::vector<OID> ids) {
        auto chunks = makeChunks(uuid, epoch, shardKey, ids);

        // Only the chunk corresponding to shard0000 is stored as a donor in the coordinator state
        // document constructed.
        auto donorChunk = chunks[0];
        insertChunkAndZoneEntries(chunks, {});
        return donorChunk;
    }

    // Returns the chunk for the recipient shard.
    ChunkType makeAndInsertChunksForRecipientShard(const UUID& uuid,
                                                   OID epoch,
                                                   const ShardKeyPattern& shardKey,
                                                   std::vector<OID> ids) {
        auto chunks = makeChunks(uuid, epoch, shardKey, ids);

        // Only the chunk corresponding to shard0001 is stored as a recipient in the coordinator
        // state document constructed.
        auto recipientChunk = chunks[1];
        insertChunkAndZoneEntries({recipientChunk}, {});
        return recipientChunk;
    }

    std::vector<ChunkType> makeChunks(const UUID& uuid,
                                      OID epoch,
                                      const ShardKeyPattern& shardKey,
                                      std::vector<OID> ids) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two chunks, one on each shard with the given namespace and epoch
        ChunkVersion version({epoch, Timestamp(1, 2)}, {1, 0});
        ChunkType chunk1(uuid, chunkRanges[0], version, ShardId("shard0000"));
        chunk1.setName(ids[0]);
        version.incMinor();
        ChunkType chunk2(uuid, chunkRanges[1], version, ShardId("shard0001"));
        chunk2.setName(ids[1]);

        return std::vector<ChunkType>{chunk1, chunk2};
    }

    std::vector<TagsType> makeZones(const NamespaceString& nss, const ShardKeyPattern& shardKey) {
        auto chunkRanges =
            _newShardKey.isShardKey(shardKey.toBSON()) ? _newChunkRanges : _oldChunkRanges;

        // Create two zones for the given namespace
        TagsType tag1(nss, kZone1, chunkRanges[0]);
        TagsType tag2(nss, kZone2, chunkRanges[1]);

        return std::vector<TagsType>{tag1, tag2};
    }

    ReshardingCoordinatorDocument insertStateAndCatalogEntries(
        CoordinatorStateEnum state,
        OID epoch,
        bool useUserUUID = false,
        boost::optional<Timestamp> fetchTimestamp = boost::none) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        auto coordinatorDoc = makeCoordinatorDoc(state, useUserUUID, fetchTimestamp);
        client.insert(NamespaceString::kConfigReshardingOperationsNamespace,
                      coordinatorDoc.toBSON());

        TypeCollectionReshardingFields reshardingFields(coordinatorDoc.getReshardingUUID());
        reshardingFields.setState(coordinatorDoc.getState());
        reshardingFields.setDonorFields(TypeCollectionDonorFields(
            coordinatorDoc.getTempReshardingNss(),
            coordinatorDoc.getReshardingKey(),
            extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards())));

        auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
            coordinatorDoc,
            reshardingFields,
            std::move(epoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        client.insert(CollectionType::ConfigNS, originalNssCatalogEntry.toBSON());

        auto tempNssCatalogEntry =
            createTempReshardingCollectionType(opCtx,
                                               coordinatorDoc,
                                               ChunkVersion({OID::gen(), Timestamp(1, 2)}, {1, 1}),
                                               BSONObj(),
                                               boost::none);
        client.insert(CollectionType::ConfigNS, tempNssCatalogEntry.toBSON());

        return coordinatorDoc;
    }

    void insertChunkAndZoneEntries(std::vector<ChunkType> chunks, std::vector<TagsType> zones) {
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);

        for (const auto& chunk : chunks) {
            client.insert(NamespaceString::kConfigsvrChunksNamespace, chunk.toConfigBSON());
        }

        for (const auto& zone : zones) {
            client.insert(TagsType::ConfigNS, zone.toBSON());
        }
    }

    void readReshardingCoordinatorDocAndAssertMatchesExpected(
        OperationContext* opCtx, ReshardingCoordinatorDocument expectedCoordinatorDoc) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace,
                                  BSON("ns" << expectedCoordinatorDoc.getSourceNss().ns_forTest()));

        auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
            doc, IDLParserContext("ReshardingCoordinatorTest"));

        ASSERT_EQUALS(coordinatorDoc.getReshardingUUID(),
                      expectedCoordinatorDoc.getReshardingUUID());
        ASSERT_EQUALS(coordinatorDoc.getSourceNss(), expectedCoordinatorDoc.getSourceNss());
        ASSERT_EQUALS(coordinatorDoc.getTempReshardingNss(),
                      expectedCoordinatorDoc.getTempReshardingNss());
        ASSERT_EQUALS(coordinatorDoc.getReshardingKey().toBSON().woCompare(
                          expectedCoordinatorDoc.getReshardingKey().toBSON()),
                      0);
        ASSERT(coordinatorDoc.getState() == expectedCoordinatorDoc.getState());
        if (expectedCoordinatorDoc.getCloneTimestamp()) {
            ASSERT(coordinatorDoc.getCloneTimestamp());
            ASSERT_EQUALS(coordinatorDoc.getCloneTimestamp().value(),
                          expectedCoordinatorDoc.getCloneTimestamp().value());
        } else {
            ASSERT(!coordinatorDoc.getCloneTimestamp());
        }

        // Confirm the (non)existence of the CoordinatorDocument abortReason.
        if (expectedCoordinatorDoc.getAbortReason()) {
            ASSERT(coordinatorDoc.getAbortReason());
            ASSERT_BSONOBJ_EQ(coordinatorDoc.getAbortReason().value(),
                              expectedCoordinatorDoc.getAbortReason().value());
        } else {
            ASSERT(!coordinatorDoc.getAbortReason());
        }

        if (!expectedCoordinatorDoc.getPresetReshardedChunks()) {
            ASSERT(!coordinatorDoc.getPresetReshardedChunks());
        }

        if (!expectedCoordinatorDoc.getZones()) {
            ASSERT(!coordinatorDoc.getZones());
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
            if (it->getMutableState().getMinFetchTimestamp()) {
                ASSERT(onDiskIt->getMutableState().getMinFetchTimestamp());
                ASSERT_EQUALS(onDiskIt->getMutableState().getMinFetchTimestamp().value(),
                              it->getMutableState().getMinFetchTimestamp().value());
            } else {
                ASSERT(!onDiskIt->getMutableState().getMinFetchTimestamp());
            }
            ASSERT(onDiskIt->getMutableState().getState() == it->getMutableState().getState());
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
            ASSERT(onDiskIt->getMutableState().getState() == it->getMutableState().getState());
        }
    }

    // Reads the original collection's catalog entry from disk and validates that the
    // reshardingFields and allowMigration matches the expected.
    void assertOriginalCollectionCatalogEntryMatchesExpected(
        OperationContext* opCtx,
        CollectionType expectedCollType,
        const ReshardingCoordinatorDocument& expectedCoordinatorDoc) {
        DBDirectClient client(opCtx);
        CollectionType onDiskEntry(
            client.findOne(CollectionType::ConfigNS, BSON("_id" << _originalNss.ns_forTest())));

        ASSERT_EQUALS(onDiskEntry.getAllowMigrations(), expectedCollType.getAllowMigrations());

        auto expectedReshardingFields = expectedCollType.getReshardingFields();
        auto expectedCoordinatorState = expectedCoordinatorDoc.getState();
        if (expectedCoordinatorState == CoordinatorStateEnum::kDone ||
            (expectedReshardingFields &&
             expectedReshardingFields->getState() >= CoordinatorStateEnum::kCommitting)) {
            ASSERT_EQUALS(onDiskEntry.getNss(), _originalNss);
            ASSERT(onDiskEntry.getUuid() == _reshardingUUID);
            ASSERT_EQUALS(onDiskEntry.getKeyPattern().toBSON().woCompare(_newShardKey.toBSON()), 0);
            ASSERT_NOT_EQUALS(onDiskEntry.getEpoch(), _originalEpoch);
        }

        if (!expectedReshardingFields)
            return;

        ASSERT(onDiskEntry.getReshardingFields());
        auto onDiskReshardingFields = onDiskEntry.getReshardingFields().value();
        ASSERT(onDiskReshardingFields.getReshardingUUID() ==
               expectedReshardingFields->getReshardingUUID());
        ASSERT(onDiskReshardingFields.getState() == expectedReshardingFields->getState());

        ASSERT(onDiskReshardingFields.getDonorFields());
        ASSERT_EQUALS(
            onDiskReshardingFields.getDonorFields()->getReshardingKey().toBSON().woCompare(
                expectedReshardingFields->getDonorFields()->getReshardingKey().toBSON()),
            0);

        if (auto expectedAbortReason = expectedCoordinatorDoc.getAbortReason()) {
            auto userCanceled = getStatusFromAbortReason(expectedCoordinatorDoc) ==
                ErrorCodes::ReshardCollectionAborted;
            ASSERT(onDiskReshardingFields.getUserCanceled() == userCanceled);
        } else {
            ASSERT(onDiskReshardingFields.getUserCanceled() == boost::none);
        }

        // Check the reshardingFields.recipientFields.
        if (expectedCoordinatorState != CoordinatorStateEnum::kAborting) {
            // Don't bother checking the recipientFields if the coordinator state is already
            // kAborting.
            if (expectedCoordinatorState < CoordinatorStateEnum::kCommitting) {
                // Until CoordinatorStateEnum::kCommitting, recipientsFields only live on the
                // temporaryNss entry in config.collections.
                ASSERT(!onDiskReshardingFields.getRecipientFields());
            } else {
                // The entry for the temporaryNss has been removed, recipientFields are appended to
                // the originalCollection's reshardingFields.
                ASSERT(onDiskReshardingFields.getRecipientFields());
            }
        }
    }

    // Reads the temporary collection's catalog entry from disk and validates that the
    // reshardingFields and allowMigration matches the expected.
    void assertTemporaryCollectionCatalogEntryMatchesExpected(
        OperationContext* opCtx, boost::optional<CollectionType> expectedCollType) {
        DBDirectClient client(opCtx);
        auto doc = client.findOne(CollectionType::ConfigNS, BSON("_id" << _tempNss.ns_forTest()));
        if (!expectedCollType) {
            ASSERT(doc.isEmpty());
            return;
        }

        CollectionType onDiskEntry(doc);

        ASSERT_EQUALS(onDiskEntry.getAllowMigrations(), expectedCollType->getAllowMigrations());

        auto expectedReshardingFields = expectedCollType->getReshardingFields().value();
        ASSERT(onDiskEntry.getReshardingFields());

        auto onDiskReshardingFields = onDiskEntry.getReshardingFields().value();
        ASSERT_EQUALS(onDiskReshardingFields.getReshardingUUID(),
                      expectedReshardingFields.getReshardingUUID());
        ASSERT(onDiskReshardingFields.getState() == expectedReshardingFields.getState());

        ASSERT(onDiskReshardingFields.getRecipientFields());
        ASSERT_EQUALS(onDiskReshardingFields.getRecipientFields()->getSourceNss(),
                      expectedReshardingFields.getRecipientFields()->getSourceNss());

        if (expectedReshardingFields.getRecipientFields()->getCloneTimestamp()) {
            ASSERT(onDiskReshardingFields.getRecipientFields()->getCloneTimestamp());
            ASSERT_EQUALS(
                onDiskReshardingFields.getRecipientFields()->getCloneTimestamp().value(),
                expectedReshardingFields.getRecipientFields()->getCloneTimestamp().value());
        } else {
            ASSERT(!onDiskReshardingFields.getRecipientFields()->getCloneTimestamp());
        }

        ASSERT(onDiskReshardingFields.getUserCanceled() ==
               expectedReshardingFields.getUserCanceled());

        // 'donorFields' should not exist for the temporary collection.
        ASSERT(!onDiskReshardingFields.getDonorFields());
    }

    void readChunkCatalogEntriesAndAssertMatchExpected(OperationContext* opCtx,
                                                       const UUID& uuid,
                                                       std::vector<ChunkType> expectedChunks,
                                                       const OID& collEpoch,
                                                       const Timestamp& collTimestamp) {
        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{NamespaceString::kConfigsvrChunksNamespace};
        findRequest.setFilter(BSON("uuid" << uuid));
        auto cursor = client.find(std::move(findRequest));

        std::vector<ChunkType> foundChunks;
        while (cursor->more()) {
            auto d = uassertStatusOK(ChunkType::parseFromConfigBSON(
                cursor->nextSafe().getOwned(), collEpoch, collTimestamp));
            foundChunks.push_back(d);
        }

        ASSERT_EQUALS(foundChunks.size(), expectedChunks.size());
        for (auto it = expectedChunks.begin(); it != expectedChunks.end(); it++) {
            auto id = it->getName();
            auto onDiskIt = std::find_if(foundChunks.begin(), foundChunks.end(), [id](ChunkType c) {
                return c.getName() == id;
            });
            ASSERT(onDiskIt != foundChunks.end());
            auto expectedBSON = it->toConfigBSON().removeField(ChunkType::collectionUUID());
            auto onDiskBSON = onDiskIt->toConfigBSON().removeField(ChunkType::collectionUUID());
            ASSERT_BSONOBJ_EQ(expectedBSON, onDiskBSON);
        }
    }

    void readTagCatalogEntriesAndAssertMatchExpected(OperationContext* opCtx,
                                                     std::vector<TagsType> expectedZones) {
        auto nss = expectedZones[0].getNS();

        DBDirectClient client(opCtx);
        FindCommandRequest findRequest{TagsType::ConfigNS};
        findRequest.setFilter(BSON("ns" << nss.ns_forTest()));
        auto cursor = client.find(std::move(findRequest));

        std::vector<TagsType> foundZones;
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

        // Check the resharding fields and allowMigrations in the config.collections entry for the
        // original collection
        TypeCollectionReshardingFields expectedReshardingFields(
            expectedCoordinatorDoc.getReshardingUUID());
        expectedReshardingFields.setState(expectedCoordinatorDoc.getState());
        TypeCollectionDonorFields donorField(
            expectedCoordinatorDoc.getTempReshardingNss(),
            expectedCoordinatorDoc.getReshardingKey(),
            extractShardIdsFromParticipantEntries(expectedCoordinatorDoc.getRecipientShards()));
        expectedReshardingFields.setDonorFields(donorField);
        if (auto abortReason = expectedCoordinatorDoc.getAbortReason()) {
            expectedReshardingFields.setUserCanceled(
                getStatusFromAbortReason(expectedCoordinatorDoc) ==
                ErrorCodes::ReshardCollectionAborted);
        }

        auto expectedOriginalCollType = makeOriginalCollectionCatalogEntry(
            expectedCoordinatorDoc,
            expectedReshardingFields,
            std::move(collectionEpoch),
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        assertOriginalCollectionCatalogEntryMatchesExpected(
            opCtx, expectedOriginalCollType, expectedCoordinatorDoc);

        // Check the resharding fields and allowMigrations in the config.collections entry for the
        // temp collection. If the expected state is >= kCommitting, the entry for the temp
        // collection should have been removed.
        boost::optional<CollectionType> expectedTempCollType = boost::none;
        if (expectedCoordinatorDoc.getState() < CoordinatorStateEnum::kCommitting) {
            expectedTempCollType = createTempReshardingCollectionType(
                opCtx,
                expectedCoordinatorDoc,
                ChunkVersion({OID::gen(), Timestamp(1, 2)}, {1, 1}),
                BSONObj(),
                boost::none);

            // It's necessary to add the userCanceled field because the call into
            // createTempReshardingCollectionType assumes that the collection entry is
            // being created in a non-aborted state.
            if (auto abortReason = expectedCoordinatorDoc.getAbortReason()) {
                auto reshardingFields = expectedTempCollType->getReshardingFields();
                reshardingFields->setUserCanceled(
                    getStatusFromAbortReason(expectedCoordinatorDoc) ==
                    ErrorCodes::ReshardCollectionAborted);
                expectedTempCollType->setReshardingFields(std::move(reshardingFields));
            }
        }

        assertTemporaryCollectionCatalogEntryMatchesExpected(opCtx, expectedTempCollType);
    }

    void assertCatalogPlacementHistoryEntryMatchExpected(
        OperationContext* opCtx,
        const std::vector<ChunkType>& expectedChunks,
        const NamespaceString& nss,
        const UUID& reshardingUUID) {
        DBDirectClient client(opCtx);
        std::set<ShardId> expectedCollPlacement;
        for (const auto& chunk : expectedChunks) {
            expectedCollPlacement.insert(chunk.getShard());
        }

        FindCommandRequest reshardedCollPlacementReq(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        reshardedCollPlacementReq.setFilter(BSON("nss" << nss.ns_forTest()));
        reshardedCollPlacementReq.setSort(BSON("timestamp" << -1));
        const auto placementDoc = client.findOne(reshardedCollPlacementReq);
        ASSERT(!placementDoc.isEmpty());
        const auto placement = NamespacePlacementType::parse(
            placementDoc, IDLParserContext("writeDecisionPersistedStateExpectSuccess"));
        ASSERT_EQ(reshardingUUID, placement.getUuid());
        ASSERT_EQ(expectedCollPlacement.size(), placement.getShards().size());
        for (const auto& shardId : placement.getShards()) {
            ASSERT(expectedCollPlacement.find(shardId) != expectedCollPlacement.end());
        }
    }

    void setupSourceCollection(OperationContext* opCtx,
                               ReshardingCoordinatorDocument coordinatorDoc) {
        setupDatabase(DatabaseName::createDatabaseName_forTest(boost::none, "db"),
                      ShardId("shard0000"));
        DBDirectClient client(opCtx);

        TypeCollectionReshardingFields reshardingFields(coordinatorDoc.getReshardingUUID());
        reshardingFields.setState(coordinatorDoc.getState());
        reshardingFields.setDonorFields(TypeCollectionDonorFields(
            coordinatorDoc.getTempReshardingNss(),
            coordinatorDoc.getReshardingKey(),
            extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards())));

        auto originalNssCatalogEntry = makeOriginalCollectionCatalogEntry(
            coordinatorDoc,
            reshardingFields,
            _originalEpoch,
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        client.insert(CollectionType::ConfigNS, originalNssCatalogEntry.toBSON());

        client.createCollection(NamespaceString::kConfigsvrChunksNamespace);
        client.createCollection(TagsType::ConfigNS);

        ASSERT_OK(createIndexOnConfigCollection(
            opCtx,
            NamespaceString::kConfigsvrChunksNamespace,
            BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1),
            true));
    }

    void writeInitialStateAndCatalogUpdatesExpectSuccess(
        OperationContext* opCtx,
        ReshardingCoordinatorDocument coordinatorDoc,
        std::vector<ChunkType> initialChunks,
        std::vector<TagsType> newZones) {
        setupSourceCollection(opCtx, coordinatorDoc);

        auto reshardingCoordinatorExternalState = ReshardingCoordinatorExternalStateImpl();

        insertCoordDocAndChangeOrigCollEntry(opCtx, _metrics.get(), coordinatorDoc);

        auto zones = *coordinatorDoc.getZones();
        auto shardsAndChunks =
            reshardingCoordinatorExternalState.calculateParticipantShardsAndChunks(
                opCtx, coordinatorDoc, zones);

        auto expectedCoordinatorDoc = coordinatorDoc;

        expectedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
        expectedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));

        expectedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);
        expectedCoordinatorDoc.setZones(boost::none);
        expectedCoordinatorDoc.setPresetReshardedChunks(boost::none);

        PhaseTransitionFn mockTransitionFn =
            createPreparingToDonateDaoUpdate(expectedCoordinatorDoc);
        writeParticipantShardsAndTempCollInfo(opCtx,
                                              _metrics.get(),
                                              expectedCoordinatorDoc,
                                              std::move(mockTransitionFn),
                                              initialChunks,
                                              zones,
                                              boost::none);

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _originalEpoch);

        // Check that chunks and tags entries have been correctly created
        readChunkCatalogEntriesAndAssertMatchExpected(
            opCtx, _reshardingUUID, initialChunks, _originalEpoch, _originalTimestamp);
        readTagCatalogEntriesAndAssertMatchExpected(opCtx, newZones);
    }

    void writeStateTransitionUpdateExpectSuccess(
        OperationContext* opCtx, ReshardingCoordinatorDocument expectedCoordinatorDoc) {
        writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
            opCtx, _metrics.get(), expectedCoordinatorDoc, boost::none);

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _originalEpoch);
    }

    void writeDecisionPersistedStateExpectSuccess(
        OperationContext* opCtx,
        ReshardingCoordinatorDocument expectedCoordinatorDoc,
        Timestamp fetchTimestamp,
        std::vector<ChunkType> expectedChunks,
        std::vector<TagsType> expectedZones) {
        std::set<ShardId> reshardedCollectionPlacement;
        for (const auto& chunk : expectedChunks) {
            reshardedCollectionPlacement.insert(chunk.getShard());
        }

        writeDecisionPersistedState(operationContext(),
                                    _metrics.get(),
                                    expectedCoordinatorDoc,
                                    _finalEpoch,
                                    _finalTimestamp,
                                    std::vector<ShardId>(reshardedCollectionPlacement.begin(),
                                                         reshardedCollectionPlacement.end()));

        // Check that config.reshardingOperations and config.collections entries are updated
        // correctly
        assertStateAndCatalogEntriesMatchExpected(opCtx, expectedCoordinatorDoc, _finalEpoch);

        // Check that chunks and tags under the temp namespace have been removed
        DBDirectClient client(opCtx);
        auto chunkDoc = client.findOne(NamespaceString::kConfigsvrChunksNamespace,
                                       BSON("ns" << _tempNss.ns_forTest()));
        ASSERT(chunkDoc.isEmpty());

        auto tagDoc = client.findOne(TagsType::ConfigNS, BSON("ns" << _tempNss.ns_forTest()));
        ASSERT(tagDoc.isEmpty());

        assertCatalogPlacementHistoryEntryMatchExpected(
            opCtx, expectedChunks, _originalNss, _reshardingUUID);
    }

    void cleanupSourceCollectionExpectSuccess(OperationContext* opCtx,
                                              ReshardingCoordinatorDocument expectedCoordinatorDoc,
                                              std::vector<ChunkType> expectedChunks,
                                              std::vector<TagsType> expectedZones) {
        removeChunkDocs(opCtx, expectedCoordinatorDoc.getSourceUUID());
        // Check that chunks and tags entries previously under the temporary namespace have been
        // correctly updated to the original namespace

        readChunkCatalogEntriesAndAssertMatchExpected(
            opCtx, _reshardingUUID, expectedChunks, _finalEpoch, _finalTimestamp);
        readTagCatalogEntriesAndAssertMatchExpected(opCtx, expectedZones);
    }

    void removeCoordinatorDocAndReshardingFieldsExpectSuccess(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
        auto updatedCoordinatorDoc = removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
            opCtx, _metrics.get(), coordinatorDoc);

        ASSERT_EQ(updatedCoordinatorDoc.getState(), CoordinatorStateEnum::kDone);

        // Check that the on disk document is same as the in memory document returned above.
        auto expectedCoordinatorDoc = coordinatorDoc;
        expectedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
        ASSERT_BSONOBJ_EQ(updatedCoordinatorDoc.toBSON(), expectedCoordinatorDoc.toBSON());

        // Check that the entry is removed from config.reshardingOperations
        DBDirectClient client(opCtx);
        auto doc = client.findOne(NamespaceString::kConfigReshardingOperationsNamespace,
                                  BSON("ns" << updatedCoordinatorDoc.getSourceNss().ns_forTest()));
        ASSERT(doc.isEmpty());

        // Check that the resharding fields are removed from the config.collections entry and
        // allowMigrations is set back to true.
        auto expectedOriginalCollType = makeOriginalCollectionCatalogEntry(
            updatedCoordinatorDoc,
            boost::none,
            _finalEpoch,
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        assertOriginalCollectionCatalogEntryMatchesExpected(
            opCtx, expectedOriginalCollType, updatedCoordinatorDoc);
    }

    void quiesceCoordinatorDocAndReshardingFieldsExpectSuccess(
        OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
        auto updatedCoordinatorDoc = removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
            opCtx, _metrics.get(), coordinatorDoc);

        // Check that the on disk document is same as the in memory document returned above.
        auto expectedCoordinatorDoc = coordinatorDoc;
        expectedCoordinatorDoc.setState(CoordinatorStateEnum::kQuiesced);
        expectedCoordinatorDoc.setQuiescePeriodEnd(updatedCoordinatorDoc.getQuiescePeriodEnd());
        ASSERT_BSONOBJ_EQ(updatedCoordinatorDoc.toBSON(), expectedCoordinatorDoc.toBSON());

        // Check that the entry is marked as quiesced in config.reshardingOperations
        readReshardingCoordinatorDocAndAssertMatchesExpected(opCtx, updatedCoordinatorDoc);

        // Check that the resharding fields are removed from the config.collections entry and
        // allowMigrations is set back to true.
        auto expectedOriginalCollType = makeOriginalCollectionCatalogEntry(
            updatedCoordinatorDoc,
            boost::none,
            _finalEpoch,
            opCtx->getServiceContext()->getPreciseClockSource()->now());
        assertOriginalCollectionCatalogEntryMatchesExpected(
            opCtx, expectedOriginalCollType, updatedCoordinatorDoc);
    }

    void transitionToErrorExpectSuccess(ErrorCodes::Error errorCode) {
        auto coordinatorDoc =
            insertStateAndCatalogEntries(CoordinatorStateEnum::kPreparingToDonate, _originalEpoch);
        auto initialChunksIds = std::vector{OID::gen(), OID::gen()};

        auto tempNssChunks =
            makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, initialChunksIds);
        auto recipientChunk = tempNssChunks[1];
        insertChunkAndZoneEntries(tempNssChunks, makeZones(_tempNss, _newShardKey));

        insertChunkAndZoneEntries(
            makeChunks(
                _originalUUID, OID::gen(), _oldShardKey, std::vector{OID::gen(), OID::gen()}),
            makeZones(_originalNss, _oldShardKey));

        // Persist the updates on disk
        auto expectedCoordinatorDoc = coordinatorDoc;
        expectedCoordinatorDoc.setState(CoordinatorStateEnum::kAborting);
        auto abortReason = Status{errorCode, "reason to abort"};
        emplaceTruncatedAbortReasonIfExists(expectedCoordinatorDoc, abortReason);

        writeStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);
    }

    NamespaceString _originalNss = NamespaceString::createNamespaceString_forTest("db.foo");
    UUID _originalUUID = UUID::gen();
    OID _originalEpoch = OID::gen();
    Timestamp _originalTimestamp{3};

    NamespaceString _tempNss = NamespaceString::createNamespaceString_forTest(
        "db.system.resharding." + _originalUUID.toString());
    UUID _reshardingUUID = UUID::gen();
    OID _tempEpoch = OID::gen();

    OID _finalEpoch = OID::gen();
    Timestamp _finalTimestamp{6};

    ShardKeyPattern _oldShardKey = ShardKeyPattern(BSON("oldSK" << 1));
    ShardKeyPattern _newShardKey = ShardKeyPattern(BSON("newSK" << 1));

    std::unique_ptr<ReshardingMetrics> _metrics;

    const std::vector<ChunkRange> _oldChunkRanges = {
        ChunkRange(_oldShardKey.getKeyPattern().globalMin(), BSON("oldSK" << 12345)),
        ChunkRange(BSON("oldSK" << 12345), _oldShardKey.getKeyPattern().globalMax()),
    };
    const std::vector<ChunkRange> _newChunkRanges = {
        ChunkRange(_newShardKey.getKeyPattern().globalMin(), BSON("newSK" << 0)),
        ChunkRange(BSON("newSK" << 0), _newShardKey.getKeyPattern().globalMax()),
    };
};

TEST_F(ReshardingCoordinatorPersistenceTest, WriteInitialInfoSucceeds) {
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kInitializing);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kPreparingToDonate.
    auto donorChunk = makeAndInsertChunksForDonorShard(
        _originalUUID, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});
    auto collectionPlacementVersion = donorChunk.getVersion();

    auto initialChunks =
        makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    auto newZones = makeZones(_tempNss, _newShardKey);
    std::vector<BSONObj> zonesBSON;
    std::vector<ReshardingZoneType> reshardingZoneTypes;
    for (const auto& zone : newZones) {
        zonesBSON.push_back(zone.toBSON());

        ReshardingZoneType zoneType(zone.getTag(), zone.getMinKey(), zone.getMaxKey());
        reshardingZoneTypes.push_back(zoneType);
    }

    std::vector<ReshardedChunk> presetBSONChunks;
    for (const auto& chunk : initialChunks) {
        presetBSONChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    expectedCoordinatorDoc.setZones(std::move(reshardingZoneTypes));
    expectedCoordinatorDoc.setPresetReshardedChunks(presetBSONChunks);

    writeInitialStateAndCatalogUpdatesExpectSuccess(
        operationContext(), expectedCoordinatorDoc, initialChunks, newZones);

    // Confirm the placement version was increased for the donor shard. The collection placement
    // version was bumped twice in 'writeInitialStateAndCatalogUpdatesExpectSuccess': once when
    // reshardingFields is inserted to the collection doc, and once again when the state transitions
    // to kPreparingToDonate.
    const auto postTransitionCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(std::partial_ordering::less,
              collectionPlacementVersion <=> postTransitionCollectionPlacementVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, ThrowsWhenZoneSpecifiedDoesNotExist) {
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kInitializing);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kPreparingToDonate.
    auto donorChunk = makeAndInsertChunksForDonorShard(
        _originalUUID, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});

    auto initialChunks =
        makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    auto newZones = makeZones(_tempNss, _newShardKey);
    std::vector<BSONObj> zonesBSON;
    std::vector<ReshardingZoneType> reshardingZoneTypes;
    for (const auto& zone : newZones) {
        zonesBSON.push_back(zone.toBSON());

        ReshardingZoneType zoneType("badZone", zone.getMinKey(), zone.getMaxKey());
        reshardingZoneTypes.push_back(zoneType);
    }

    std::vector<ReshardedChunk> presetBSONChunks;
    for (const auto& chunk : initialChunks) {
        presetBSONChunks.emplace_back(chunk.getShard(), chunk.getMin(), chunk.getMax());
    }

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    expectedCoordinatorDoc.setZones(std::move(reshardingZoneTypes));
    expectedCoordinatorDoc.setPresetReshardedChunks(presetBSONChunks);

    setupSourceCollection(operationContext(), expectedCoordinatorDoc);

    auto reshardingCoordinatorExternalState = ReshardingCoordinatorExternalStateImpl();

    insertCoordDocAndChangeOrigCollEntry(
        operationContext(), _metrics.get(), expectedCoordinatorDoc);

    auto zones = *expectedCoordinatorDoc.getZones();
    auto shardsAndChunks = reshardingCoordinatorExternalState.calculateParticipantShardsAndChunks(
        operationContext(), expectedCoordinatorDoc, zones);

    expectedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    expectedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);
    expectedCoordinatorDoc.setZones(boost::none);
    expectedCoordinatorDoc.setPresetReshardedChunks(boost::none);

    PhaseTransitionFn mockTransitionFn = createPreparingToDonateDaoUpdate(expectedCoordinatorDoc);
    ASSERT_THROWS_CODE(writeParticipantShardsAndTempCollInfo(operationContext(),
                                                             _metrics.get(),
                                                             expectedCoordinatorDoc,
                                                             std::move(mockTransitionFn),
                                                             initialChunks,
                                                             zones,
                                                             boost::none),
                       DBException,
                       ErrorCodes::ZoneNotFound);
}

TEST_F(ReshardingCoordinatorPersistenceTest, BasicStateTransitionSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kCloning, _originalEpoch);

    // Ensure the chunks for the original and temporary namespaces exist since they will be bumped
    // as a product of the state transition to kBlockingWrites.
    makeAndInsertChunksForDonorShard(
        _originalUUID, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});
    auto initialOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));

    makeAndInsertChunksForRecipientShard(
        _reshardingUUID, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});
    auto initialTempCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _tempNss));

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kBlockingWrites);

    writeStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);
    auto finalOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialOriginalCollectionPlacementVersion <=>
                  finalOriginalCollectionPlacementVersion);

    auto finalTempCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _tempNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialTempCollectionPlacementVersion <=> finalTempCollectionPlacementVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionWithFetchTimestampSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kPreparingToDonate, _originalEpoch);

    // Ensure the chunks for the original and temporary namespaces exist since they will be bumped
    // as a product of the state transition to kCloning.
    makeAndInsertChunksForDonorShard(
        _originalUUID, _originalEpoch, _oldShardKey, std::vector{OID::gen(), OID::gen()});

    makeAndInsertChunksForRecipientShard(
        _reshardingUUID, _tempEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kCloning);
    emplaceCloneTimestampIfExists(expectedCoordinatorDoc, Timestamp(1, 1));
    emplaceApproxBytesToCopyIfExists(expectedCoordinatorDoc, [] {
        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(0);
        approxCopySize.setApproxDocumentsToCopy(0);
        return approxCopySize;
    }());

    auto initialOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    auto initialTempCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _tempNss));

    writeStateTransitionUpdateExpectSuccess(operationContext(), expectedCoordinatorDoc);

    auto finalOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialOriginalCollectionPlacementVersion <=>
                  finalOriginalCollectionPlacementVersion);

    auto finalTempCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _tempNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialTempCollectionPlacementVersion <=> finalTempCollectionPlacementVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionToDecisionPersistedSucceeds) {
    Timestamp fetchTimestamp = Timestamp(1, 1);
    auto coordinatorDoc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kBlockingWrites, _originalEpoch, false, fetchTimestamp);
    auto initialChunksIds = std::vector{OID::gen(), OID::gen()};

    auto tempNssChunks = makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, initialChunksIds);
    auto recipientChunk = tempNssChunks[1];
    insertChunkAndZoneEntries(tempNssChunks, makeZones(_tempNss, _newShardKey));

    insertChunkAndZoneEntries(
        makeChunks(_originalUUID, OID::gen(), _oldShardKey, std::vector{OID::gen(), OID::gen()}),
        makeZones(_originalNss, _oldShardKey));

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    // The new epoch to use for the resharded collection to indicate that the collection is a
    // new incarnation of the namespace
    auto updatedChunks = makeChunks(_originalUUID, _finalEpoch, _newShardKey, initialChunksIds);
    auto updatedZones = makeZones(_originalNss, _newShardKey);

    auto initialCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));


    writeDecisionPersistedStateExpectSuccess(
        operationContext(), expectedCoordinatorDoc, fetchTimestamp, updatedChunks, updatedZones);

    // Since the epoch is changed, there is no need to bump the chunk versions with the transition.
    auto finalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(initialCollectionPlacementVersion.toLong(), finalCollectionPlacementVersion.toLong());
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionToErrorSucceeds) {
    transitionToErrorExpectSuccess(ErrorCodes::InternalError);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionToErrorFromManualAbortSucceeds) {
    transitionToErrorExpectSuccess(ErrorCodes::ReshardCollectionAborted);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionToDoneSucceeds) {
    auto coordinatorDoc =
        insertStateAndCatalogEntries(CoordinatorStateEnum::kCommitting, _finalEpoch);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kDone.
    makeAndInsertChunksForRecipientShard(
        _reshardingUUID, _finalEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    auto initialOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));

    removeCoordinatorDocAndReshardingFieldsExpectSuccess(operationContext(), coordinatorDoc);

    auto finalOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialOriginalCollectionPlacementVersion <=>
                  finalOriginalCollectionPlacementVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionToQuiescedSucceeds) {
    auto coordinatorDoc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kCommitting, _finalEpoch, true /* useUserUUID */);

    // Ensure the chunks for the original namespace exist since they will be bumped as a product of
    // the state transition to kDone.
    makeAndInsertChunksForRecipientShard(
        _reshardingUUID, _finalEpoch, _newShardKey, std::vector{OID::gen(), OID::gen()});

    auto initialOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));

    quiesceCoordinatorDocAndReshardingFieldsExpectSuccess(operationContext(), coordinatorDoc);

    auto finalOriginalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(std::partial_ordering::less,
              initialOriginalCollectionPlacementVersion <=>
                  finalOriginalCollectionPlacementVersion);
}

TEST_F(ReshardingCoordinatorPersistenceTest, StateTransitionWhenCoordinatorDocDoesNotExistFails) {
    // Do not insert initial entry into config.reshardingOperations. Attempt to update coordinator
    // state documents.
    auto coordinatorDoc =
        makeCoordinatorDoc(CoordinatorStateEnum::kCloning, false, Timestamp(1, 1));
    ASSERT_THROWS_CODE(writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
                           operationContext(), _metrics.get(), coordinatorDoc),
                       AssertionException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(ReshardingCoordinatorPersistenceTest,
       WriteInitialStateOriginalNamespaceCatalogEntryMissingFails) {
    auto coordinatorDoc = makeCoordinatorDoc(CoordinatorStateEnum::kInitializing);

    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Do not create the config.collections entry for the original collection
    ASSERT_THROWS_CODE(
        insertCoordDocAndChangeOrigCollEntry(operationContext(), _metrics.get(), coordinatorDoc),
        AssertionException,
        ErrorCodes::NamespaceNotFound);
}

TEST_F(ReshardingCoordinatorPersistenceTest, SourceCleanupBetweenTransitionsSucceeds) {

    Timestamp fetchTimestamp = Timestamp(1, 1);
    auto coordinatorDoc = insertStateAndCatalogEntries(
        CoordinatorStateEnum::kBlockingWrites, _originalEpoch, false, fetchTimestamp);
    auto initialChunksIds = std::vector{OID::gen(), OID::gen()};

    auto tempNssChunks = makeChunks(_reshardingUUID, _tempEpoch, _newShardKey, initialChunksIds);
    auto recipientChunk = tempNssChunks[1];
    insertChunkAndZoneEntries(tempNssChunks, makeZones(_tempNss, _newShardKey));

    insertChunkAndZoneEntries(
        makeChunks(_originalUUID, OID::gen(), _oldShardKey, std::vector{OID::gen(), OID::gen()}),
        makeZones(_originalNss, _oldShardKey));

    // Persist the updates on disk
    auto expectedCoordinatorDoc = coordinatorDoc;
    expectedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    // The new epoch to use for the resharded collection to indicate that the collection is a
    // new incarnation of the namespace
    auto updatedChunks = makeChunks(_originalUUID, _finalEpoch, _newShardKey, initialChunksIds);
    auto updatedZones = makeZones(_originalNss, _newShardKey);

    auto initialCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));


    writeDecisionPersistedStateExpectSuccess(
        operationContext(), expectedCoordinatorDoc, fetchTimestamp, updatedChunks, updatedZones);

    // Since the epoch is changed, there is no need to bump the chunk versions with the transition.
    auto finalCollectionPlacementVersion =
        assertGet(getCollectionPlacementVersion(operationContext(), _originalNss));
    ASSERT_EQ(initialCollectionPlacementVersion.toLong(), finalCollectionPlacementVersion.toLong());

    cleanupSourceCollectionExpectSuccess(
        operationContext(), expectedCoordinatorDoc, updatedChunks, updatedZones);
}

}  // namespace
}  // namespace mongo
