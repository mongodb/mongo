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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/periodic_runner_factory.h"

namespace mongo {

class ShardCatalogHistoryCleanupTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto* svc = getServiceContext();

        auto* storageEngine = svc->getStorageEngine();

        storageEngine->startTimestampMonitor(
            {&shard_catalog_helper::kShardCatalogHistoryCleanupTimestampListener});
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
};

TEST_F(ShardCatalogHistoryCleanupTest, ShardCatalogHistoryCleanupCalledOnTimestampMonitorAdvance) {
    const ShardId kShardName{"testShard"};
    const ShardId kAnotherShardName{"anotherTestShard"};

    // Imagining the test is running on a primary in sharded cluster
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    auto* replCoord = repl::ReplicationCoordinator::get(getServiceContext());
    ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    ShardingState::get(getServiceContext())
        ->setRecoveryCompleted({OID::gen(),
                                ClusterRole::ShardServer,
                                ConnectionString(HostAndPort("dummy", 1)),
                                kShardName});

    auto opCtx = cc().makeOperationContext();

    auto shardingState = ShardingState::get(getServiceContext());
    ASSERT_EQ(shardingState->shardId(), ShardId(kShardName));
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

    auto currentlyOwnedChunkType =
        ChunkType(collectionUUID,
                  ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 100)),
                  ChunkVersion(CollectionGeneration{epoch, oldTimestamp}, placement),
                  kShardName);
    currentlyOwnedChunkType.setHistory({ChunkHistory(oldTimestamp, kShardName)});
    currentlyOwnedChunkType.setOnCurrentShardSince(oldTimestamp);
    auto outdatedPreviouslyOwnedChunkType =
        ChunkType(collectionUUID,
                  ChunkRange(BSON("_id" << 100), BSON("_id" << 200)),
                  ChunkVersion(CollectionGeneration{epoch, oldTimestamp}, placement),
                  kAnotherShardName);
    outdatedPreviouslyOwnedChunkType.setHistory(
        {ChunkHistory(oldTimestamp, kAnotherShardName), ChunkHistory(oldestTimestamp, kShardName)});
    outdatedPreviouslyOwnedChunkType.setOnCurrentShardSince(oldTimestamp);
    auto notOutdatedPreviouslyOwnedChunkType =
        ChunkType(collectionUUID,
                  ChunkRange(BSON("_id" << 200), BSON("_id" << 300)),
                  ChunkVersion(CollectionGeneration{epoch, newTimestamp}, placement),
                  kAnotherShardName);
    notOutdatedPreviouslyOwnedChunkType.setHistory(
        {ChunkHistory(newTimestamp, kAnotherShardName), ChunkHistory(oldestTimestamp, kShardName)});
    notOutdatedPreviouslyOwnedChunkType.setOnCurrentShardSince(newTimestamp);

    DBDirectClient client(opCtx.get());
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                  currentlyOwnedChunkType.toConfigBSON());
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                  outdatedPreviouslyOwnedChunkType.toConfigBSON());
    client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                  notOutdatedPreviouslyOwnedChunkType.toConfigBSON());

    waitForTimestampMonitorPass();
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

    ASSERT_EQ(chunks.size(), 2u);

    // Note: Parsing from ChunkType overrode the chunk version, so we have to set it manually
    std::vector<ChunkType> expectedChunks = {currentlyOwnedChunkType,
                                             notOutdatedPreviouslyOwnedChunkType};
    for (auto& chunk : expectedChunks) {
        chunk.setVersion(ChunkVersion(CollectionGeneration(epoch, newTimestamp), placement));
    }

    std::sort(
        expectedChunks.begin(),
        expectedChunks.end(),
        [](const ChunkType& lhs, const ChunkType& rhs) { return lhs.getRange() < rhs.getRange(); });
    std::sort(chunks.begin(), chunks.end(), [](const ChunkType& lhs, const ChunkType& rhs) {
        return lhs.getRange() < rhs.getRange();
    });

    ASSERT(std::equal(chunks.begin(),
                      chunks.end(),
                      expectedChunks.begin(),
                      expectedChunks.end(),
                      [](const ChunkType& lhs, const ChunkType& rhs) {
                          return lhs.getRange() == rhs.getRange();
                      }));
}

}  // namespace mongo
