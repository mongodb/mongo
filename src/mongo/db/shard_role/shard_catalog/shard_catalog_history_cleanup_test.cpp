/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/shard_role/shard_catalog/shard_catalog_history_cleanup.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_catalog_history_cleanup_control.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/periodic_runner_factory.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

namespace {
static const ShardId kAnotherShardName{"anotherTestShard"};
static const KeyPattern kKeyPattern{BSON("_id" << 1)};
}  // namespace

class ShardCatalogHistoryCleanupTest : public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        auto* svc = getServiceContext();

        auto* storageEngine = svc->getStorageEngine();

        storageEngine->startTimestampMonitor(
            {&shard_catalog_helper::kShardCatalogHistoryCleanupTimestampListener});

        // Imagining the test is running on a primary in sharded cluster
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        auto* replCoord = repl::ReplicationCoordinator::get(getServiceContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }

    void waitForTimestampMonitorPass() {
        auto* svc = getServiceContext();
        auto storageEngine = svc->getStorageEngine();
        auto timestampMonitor = storageEngine->getTimestampMonitor();
        using TimestampListener = StorageEngine::TimestampMonitor::TimestampListener;
        auto pf = makePromiseFuture<void>();
        auto listener =
            TimestampListener([promise = &pf.promise](OperationContext* opCtx, auto) mutable {
                promise->emplaceValue();
            });
        timestampMonitor->addListener(&listener);
        pf.future.wait();
        timestampMonitor->removeListener(&listener);
    }

    std::pair<CollectionType, std::vector<ChunkType>> makeCollectionAndChunks(
        const NamespaceString& nss,
        bool isCurrentlyOwned,
        bool isOutdatedButActive,
        bool isFullyOutdated,
        Timestamp currentTimestamp,
        Timestamp intermediateTimestamp,
        Timestamp staleTimestamp) {

        auto collectionUUID = UUID::gen();
        auto epoch = OID::gen();
        auto placement = CollectionPlacement(1, 1);
        CollectionType collEntry{
            nss, epoch, staleTimestamp, Date_t::now(), collectionUUID, kKeyPattern};

        std::vector<ChunkType> chunks;
        if (isCurrentlyOwned) {
            auto currentlyOwnedChunkType =
                ChunkType(collectionUUID,
                          ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 100)),
                          ChunkVersion(CollectionGeneration{epoch, currentTimestamp}, placement),
                          kMyShardName);
            currentlyOwnedChunkType.setHistory({ChunkHistory(currentTimestamp, kMyShardName)});
            currentlyOwnedChunkType.setOnCurrentShardSince(currentTimestamp);
            chunks.emplace_back(std::move(currentlyOwnedChunkType));
        }
        if (isOutdatedButActive) {
            auto unownedButVisibleChunk =
                ChunkType(collectionUUID,
                          ChunkRange(BSON("_id" << 100), BSON("_id" << 200)),
                          ChunkVersion(CollectionGeneration{epoch, currentTimestamp}, placement),
                          kAnotherShardName);
            unownedButVisibleChunk.setHistory(
                {ChunkHistory(intermediateTimestamp, kAnotherShardName),
                 ChunkHistory(staleTimestamp, kMyShardName)});
            unownedButVisibleChunk.setOnCurrentShardSince(intermediateTimestamp);
            chunks.emplace_back(std::move(unownedButVisibleChunk));
        }
        if (isFullyOutdated) {
            auto staleChunk =
                ChunkType(collectionUUID,
                          ChunkRange(BSON("_id" << 200), BSON("_id" << 300)),
                          ChunkVersion(CollectionGeneration{epoch, currentTimestamp}, placement),
                          kAnotherShardName);
            staleChunk.setHistory({ChunkHistory(staleTimestamp - 1, kAnotherShardName),
                                   ChunkHistory(staleTimestamp - 2, kMyShardName)});
            staleChunk.setOnCurrentShardSince(staleTimestamp - 1);

            chunks.emplace_back(std::move(staleChunk));
        }

        return {std::move(collEntry), std::move(chunks)};
    }
    void setupCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool isCurrentlyOwned,
                         bool isOutdatedButActive,
                         bool isFullyOutdated,
                         Timestamp currentTimestamp,
                         Timestamp outdatedTimestamp,
                         Timestamp staleTimestamp) {
        auto [coll, chunks] = makeCollectionAndChunks(nss,
                                                      isCurrentlyOwned,
                                                      isOutdatedButActive,
                                                      isFullyOutdated,
                                                      currentTimestamp,
                                                      outdatedTimestamp,
                                                      staleTimestamp);
        createTestCollection(opCtx, nss);
        auto uuid = *CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, nss);
        coll.setUuid(uuid);
        for (auto& chunk : chunks) {
            chunk.setCollectionUUID(uuid);
        }

        DBDirectClient client{opCtx};
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      coll.asShardCatalogType().toBSON());
        std::vector<BSONObj> chunkBsons;
        for (const auto& chunk : chunks) {
            chunkBsons.emplace_back(chunk.toConfigBSON());
        }
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunkBsons);
    }
};

TEST_F(ShardCatalogHistoryCleanupTest, ShardCatalogHistoryCleanupCalledOnTimestampMonitorAdvance) {
    RAIIServerParameterControllerForTest ddlFlag{"featureFlagAuthoritativeShardsDDL", true};
    RAIIServerParameterControllerForTest crudFlag{"featureFlagAuthoritativeShardsCRUD", true};

    auto opCtx = operationContext();

    auto storageEngine = getServiceContext()->getStorageEngine();
    auto staleTimestamp = storageEngine->getOldestTimestamp() + 20;
    auto oldestTimestamp = staleTimestamp + 10;
    auto currentTimestamp = oldestTimestamp + 10;

    storageEngine->setOldestTimestamp(oldestTimestamp, false /*force*/);
    waitForTimestampMonitorPass();

    // Filling kConfigShardCatalogChunksNamespace directly through DBClient to avoid
    // waitForMajorityConcern logic handling
    auto [coll, chunks] = makeCollectionAndChunks(
        NamespaceStringUtil::deserialize(
            boost::none, "test.collection", SerializationContext::stateDefault()),
        true,
        true,
        true,
        currentTimestamp,
        oldestTimestamp,
        staleTimestamp);

    std::vector<BSONObj> chunksBSON;
    for (const auto& chunk : chunks) {
        chunksBSON.emplace_back(chunk.toConfigBSON());
    }
    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, chunksBSON);
    client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                  coll.asShardCatalogType().toBSON());

    waitForTimestampMonitorPass();

    size_t numChunks = 0;
    FindCommandRequest findRequest{NamespaceString::kConfigShardCatalogChunksNamespace};
    auto cursor = client.find(std::move(findRequest));
    while (cursor->more()) {
        BSONObj obj = cursor->nextSafe();
        StatusWith<ChunkType> chunkRes =
            ChunkType::parseFromConfigBSON(obj, coll.getEpoch(), coll.getTimestamp());
        ASSERT_OK(chunkRes.getStatus());

        const auto& chunk = chunkRes.getValue();

        bool isCurrentlyOwned = chunk.getShard() == kMyShardName;
        bool isStillVisible = chunk.getOnCurrentShardSince() >= staleTimestamp;
        LOGV2_INFO(12620100,
                   "Checking chunk",
                   "chunk"_attr = obj,
                   "isCurrentlyOwned"_attr = isCurrentlyOwned,
                   "isStillVisible"_attr = isStillVisible);
        ASSERT_TRUE(isCurrentlyOwned || isStillVisible);
        numChunks++;
    }
    ASSERT_EQ(2, numChunks);  // currently owned + unowned but visible
}

TEST_F(ShardCatalogHistoryCleanupTest, ShardCatalogHistoryCleanupStopsOnPause) {
    RAIIServerParameterControllerForTest ddlFlag{"featureFlagAuthoritativeShardsDDL", true};
    RAIIServerParameterControllerForTest crudFlag{"featureFlagAuthoritativeShardsCRUD", true};

    auto opCtx = operationContext();

    auto shardingState = ShardingState::get(getServiceContext());
    ASSERT_EQ(shardingState->shardId(), kMyShardName);
    ASSERT(shardingState->enabled());

    auto storageEngine = getServiceContext()->getStorageEngine();
    auto oldestTimestamp = storageEngine->getOldestTimestamp();
    auto oldTimestamp = Timestamp(3, 1);
    storageEngine->setOldestTimestamp(oldTimestamp, false /*force*/);
    waitForTimestampMonitorPass();
    auto newTimestamp = Timestamp(4, 1);
    storageEngine->setOldestTimestamp(newTimestamp, false /*force*/);

    // Filling kConfigShardCatalogChunksNamespace directly through DBClient to avoid
    // waitForMajorityConcern logic handling
    auto collectionUUID = UUID::gen();
    auto epoch = OID::gen();
    auto placement = CollectionPlacement(1, 1);

    auto outdatedPreviouslyOwnedChunkType =
        ChunkType(collectionUUID,
                  ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                  ChunkVersion(CollectionGeneration{epoch, oldTimestamp}, placement),
                  kAnotherShardName);
    outdatedPreviouslyOwnedChunkType.setHistory({ChunkHistory(oldTimestamp, kAnotherShardName),
                                                 ChunkHistory(oldestTimestamp, kMyShardName)});
    outdatedPreviouslyOwnedChunkType.setOnCurrentShardSince(oldTimestamp);

    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                  outdatedPreviouslyOwnedChunkType.toConfigBSON());

    // Pausing the cleanup task
    ShardCatalogHistoryCleanupControl::get(opCtx).pause();
    waitForTimestampMonitorPass();

    std::vector<ChunkType> chunks;
    FindCommandRequest findRequest{NamespaceString::kConfigShardCatalogChunksNamespace};
    auto cursor = client.find(std::move(findRequest));
    while (cursor->more()) {
        BSONObj obj = cursor->nextSafe().getOwned();
        StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, epoch, newTimestamp);
        ASSERT(chunkRes.getStatus().isOK());
        chunks.push_back(chunkRes.getValue());
    }

    // Despite the cleanup being triggered, the outdated chunk still persisted
    ASSERT_EQ(chunks.size(), 1u);

    // Resuming the cleanup task
    ShardCatalogHistoryCleanupControl::get(opCtx).resume();
    waitForTimestampMonitorPass();

    chunks.clear();

    FindCommandRequest secondFindRequest{NamespaceString::kConfigShardCatalogChunksNamespace};
    cursor = client.find(std::move(secondFindRequest));
    while (cursor->more()) {
        BSONObj obj = cursor->nextSafe().getOwned();
        StatusWith<ChunkType> chunkRes = ChunkType::parseFromConfigBSON(obj, epoch, newTimestamp);
        ASSERT(chunkRes.getStatus().isOK());
        chunks.push_back(chunkRes.getValue());
    }

    // After resuming the cleanup, the outdated chunk should be dropped
    ASSERT(chunks.empty());
}

TEST_F(ShardCatalogHistoryCleanupTest, DeletesStaleCollectionEntries) {
    RAIIServerParameterControllerForTest ddlFlag{"featureFlagAuthoritativeShardsDDL", true};
    RAIIServerParameterControllerForTest crudFlag{"featureFlagAuthoritativeShardsCRUD", true};

    auto storageEngine = getServiceContext()->getStorageEngine();
    auto oldestTimestamp = storageEngine->getOldestTimestamp() + 20;
    auto oldTimestamp = oldestTimestamp + 1;
    storageEngine->setOldestTimestamp(oldTimestamp, false /*force*/);
    auto newTimestamp = oldTimestamp + 1;

    const auto kStaleNss = NamespaceStringUtil::deserialize(
        boost::none, "test.stale_collection", SerializationContext::stateDefault());
    const auto kCurrentNss = NamespaceStringUtil::deserialize(
        boost::none, "test.valid_collection", SerializationContext::stateDefault());
    setupCollection(operationContext(),
                    kStaleNss,
                    false,
                    false,
                    true,
                    newTimestamp,
                    oldTimestamp,
                    oldestTimestamp);
    setupCollection(operationContext(),
                    kCurrentNss,
                    true,
                    true,
                    false,
                    newTimestamp,
                    oldTimestamp,
                    oldestTimestamp);

    waitForTimestampMonitorPass();

    DBDirectClient client{operationContext()};
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kCurrentNss.toString_forTest())),
              1);
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kStaleNss.toString_forTest())),
              0);
    const auto currentUuid = *CollectionCatalog::get(operationContext())
                                  ->lookupUUIDByNSS(operationContext(), kCurrentNss);
    const auto staleUuid =
        *CollectionCatalog::get(operationContext())->lookupUUIDByNSS(operationContext(), kStaleNss);
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogChunksNamespace,
                           BSON(ChunkType::collectionUUID() << staleUuid)),
              0);
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogChunksNamespace,
                           BSON(ChunkType::collectionUUID() << currentUuid)),
              2);
}

TEST_F(ShardCatalogHistoryCleanupTest, SkipsCleanupWhenCollectionCriticalSectionActive) {
    RAIIServerParameterControllerForTest ddlFlag{"featureFlagAuthoritativeShardsDDL", true};
    RAIIServerParameterControllerForTest crudFlag{"featureFlagAuthoritativeShardsCRUD", true};

    auto storageEngine = getServiceContext()->getStorageEngine();
    auto oldestTimestamp = storageEngine->getOldestTimestamp() + 20;
    auto oldTimestamp = oldestTimestamp + 1;
    storageEngine->setOldestTimestamp(oldTimestamp, false /*force*/);
    auto newTimestamp = oldTimestamp + 1;

    const auto kStaleNss = NamespaceStringUtil::deserialize(
        boost::none, "test.stale_collection", SerializationContext::stateDefault());
    setupCollection(operationContext(),
                    kStaleNss,
                    false /* isCurrentlyOwned */,
                    false /* isOutdatedButActive */,
                    true /* isFullyOutdated */,
                    newTimestamp,
                    oldTimestamp,
                    oldestTimestamp);

    const auto csReason = BSON("reason" << "test");

    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), kStaleNss, AcquisitionPrerequisites::kWrite),
                              MODE_X);
        auto csr = CollectionShardingRuntime::acquireExclusive(operationContext(), kStaleNss);
        csr->enterCriticalSectionCatchUpPhase(operationContext(), csReason);
        csr->enterCriticalSectionCommitPhase(operationContext(), csReason);
    }

    waitForTimestampMonitorPass();

    // Within the critical section the cleanup shouldn't run
    DBDirectClient client{operationContext()};
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kStaleNss.toString_forTest())),
              1);

    {
        auto acq =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), kStaleNss, AcquisitionPrerequisites::kWrite),
                              MODE_X);
        auto csr = CollectionShardingRuntime::acquireExclusive(operationContext(), kStaleNss);
        csr->exitCriticalSection(operationContext(), csReason);
    }

    waitForTimestampMonitorPass();

    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kStaleNss.toString_forTest())),
              0);
}

TEST_F(ShardCatalogHistoryCleanupTest, SkipsCleanupWhenDatabaseCriticalSectionActive) {
    RAIIServerParameterControllerForTest ddlFlag{"featureFlagAuthoritativeShardsDDL", true};
    RAIIServerParameterControllerForTest crudFlag{"featureFlagAuthoritativeShardsCRUD", true};

    auto storageEngine = getServiceContext()->getStorageEngine();
    auto oldestTimestamp = storageEngine->getOldestTimestamp() + 20;
    auto oldTimestamp = oldestTimestamp + 1;
    storageEngine->setOldestTimestamp(oldTimestamp, false /*force*/);
    auto newTimestamp = oldTimestamp + 1;

    const auto kStaleNss = NamespaceStringUtil::deserialize(
        boost::none, "test.stale_collection", SerializationContext::stateDefault());
    setupCollection(operationContext(),
                    kStaleNss,
                    false /* isCurrentlyOwned */,
                    false /* isOutdatedButActive */,
                    true /* isFullyOutdated */,
                    newTimestamp,
                    oldTimestamp,
                    oldestTimestamp);

    const auto csReason = BSON("reason" << "test");

    {
        AutoGetDb autoDb(operationContext(), kStaleNss.dbName(), MODE_X);
        auto dsr =
            DatabaseShardingRuntime::acquireExclusive(operationContext(), kStaleNss.dbName());
        dsr->enterCriticalSectionCatchUpPhase(operationContext(), csReason);
        dsr->enterCriticalSectionCommitPhase(operationContext(), csReason);
    }

    waitForTimestampMonitorPass();

    // Within the critical section the cleanup shouldn't run
    DBDirectClient client{operationContext()};
    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kStaleNss.toString_forTest())),
              1);

    {
        AutoGetDb autoDb(operationContext(), kStaleNss.dbName(), MODE_X);
        auto dsr =
            DatabaseShardingRuntime::acquireExclusive(operationContext(), kStaleNss.dbName());
        dsr->exitCriticalSection(operationContext(), csReason);
    }

    waitForTimestampMonitorPass();

    ASSERT_EQ(client.count(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                           BSON(CollectionType::kNssFieldName << kStaleNss.toString_forTest())),
              0);
}
}  // namespace mongo
